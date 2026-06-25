// fitom/CFITOM.cpp
// FITOM コアシングルトン実装
//
// 旧 FITOM.cpp からの変更:
//   - boost::thread → std::thread
//   - FMVOICE* / CFMBank → PatchManager
//   - SCCI 依存を除去 (PortFactory 経由)
//   - tables.h (ROM::devmap) を FITOMdefine.h の定数で代替

#include "fitom/CFITOM.h"
#include "fitom/IPort.h"
#include "fitom/DeviceFactory.h"
#include "fitom/SccWaveData.h"
#include "fitom/PcmBankData.h"
#include "fitom/DrumData.h"
#include "fitom/Log.h"
#include "fitom/FITOMdefine.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>

namespace fitom {

// ================================================================
//  静的デバイス情報テーブル (旧 ROM::devmap に対応)
// ================================================================
namespace {

struct DevMapEntry {
    uint32_t    devid;
    const char* name;
    uint32_t    voicetype;
    uint32_t    voicegroup;
    uint32_t    regsize;
};

static const DevMapEntry kDevMap[] = {
    {DEVICE_OPN,   "OPN",   VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x100},
    {DEVICE_OPNA,  "OPNA",  VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x200},
    {DEVICE_OPN2,  "OPN2",  VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x200},
    {DEVICE_OPN2C, "OPN2C", VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x200},
    {DEVICE_OPN2L, "OPN2L", VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x200},
    {DEVICE_OPN3,  "OPN3",  VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x400},
    {DEVICE_OPM,   "OPM",   VOICE_TYPE_FM4,  VOICE_GROUP_OPM,  0x100},
    {DEVICE_OPP,   "OPP",   VOICE_TYPE_FM4,  VOICE_GROUP_OPM,  0x100},
    {DEVICE_OPZ,   "OPZ",   VOICE_TYPE_FM4,  VOICE_GROUP_OPM,  0x100},
    {DEVICE_OPL,   "OPL",   VOICE_TYPE_FM2,  VOICE_GROUP_OPL2, 0x100},
    {DEVICE_OPL2,  "OPL2",  VOICE_TYPE_FM2,  VOICE_GROUP_OPL2, 0x100},
    {DEVICE_OPL3,  "OPL3",  VOICE_TYPE_FM4,  VOICE_GROUP_OPL3, 0x200},
    {DEVICE_OPLL,  "OPLL",  VOICE_TYPE_FM2,  VOICE_GROUP_OPLL, 0x040},
    {DEVICE_SSG,   "SSG",   VOICE_TYPE_PSG,  VOICE_GROUP_PSG,  0x020},
    {DEVICE_PSG,   "PSG",   VOICE_TYPE_PSG,  VOICE_GROUP_PSG,  0x020},
    {DEVICE_DCSG,  "DCSG",  VOICE_TYPE_PSG,  VOICE_GROUP_PSG,  0x010},
    {DEVICE_SCC,   "SCC",   VOICE_TYPE_PSG,  VOICE_GROUP_PSG,  0x0B0},
    {DEVICE_ADPCM, "ADPCM", VOICE_TYPE_PCM,  VOICE_GROUP_PCM,  0},
    {DEVICE_PCMD8, "PCMD8", VOICE_TYPE_PCM,  VOICE_GROUP_PCM,  0},
    {DEVICE_NONE,  nullptr, 0,               0,                0},
};

const DevMapEntry* findDevMap(uint32_t deviceId) {
    for (int i = 0; kDevMap[i].devid != DEVICE_NONE; ++i) {
        if (kDevMap[i].devid == deviceId) return &kDevMap[i];
    }
    return nullptr;
}

} // anonymous namespace

// ================================================================
//  静的ユーティリティ
// ================================================================

uint32_t CFITOM::getDeviceVoiceType(uint32_t deviceId) {
    const auto* e = findDevMap(deviceId);
    return e ? e->voicetype : 0;
}

uint32_t CFITOM::getDeviceVoiceGroupMask(uint32_t deviceId) {
    const auto* e = findDevMap(deviceId);
    return e ? e->voicegroup : 0;
}

const std::string CFITOM::getDeviceNameFromId(uint32_t deviceId) {
    const auto* e = findDevMap(deviceId);
    return e && e->name ? e->name : "Unknown";
}

uint32_t CFITOM::getDeviceRegSize(uint32_t deviceId) {
    const auto* e = findDevMap(deviceId);
    return e ? e->regsize : 0;
}

uint32_t CFITOM::getDeviceIdFromName(const std::string& name) {
    for (int i = 0; kDevMap[i].devid != DEVICE_NONE; ++i) {
        if (kDevMap[i].name && name == kDevMap[i].name)
            return kDevMap[i].devid;
    }
    return DEVICE_NONE;
}

// ================================================================
//  初期化
// ================================================================

int CFITOM::init(std::unique_ptr<FITOMConfig> config,
                 std::unique_ptr<PatchManager> patchMgr)
{
    FITOM_LOG_INFO("FITOM initializing...");

    config_   = std::move(config);
    patchMgr_ = std::move(patchMgr);

    // デバイス初期化
    initDevices();

    // MIDI チャンネルと MidiProcessor を構築
    int midiInputCount = config_->getMidiInputCount();
    if (midiInputCount == 0) {
        FITOM_LOG_WARN("No MIDI inputs configured");
    }

    // 各 MIDI 入力ポートに対して MidiProcessor を生成
    for (int p = 0; p < MAX_MPUS && p < midiInputCount; ++p) {
        // channel_map からチャンネルを構築
        for (int ch = 0; ch < 16; ++ch) {
            bool isRhythm = false;
            for (const auto& cm : config_->getChannelMap()) {
                if (cm.midiCh == ch && cm.deviceIndex < 0) {
                    isRhythm = true; break;
                }
            }
            if (isRhythm) {
                channels_[p][ch] = std::make_unique<CRhythmCh>(
                    static_cast<uint8_t>(ch), this);
            } else {
                auto instCh = std::make_unique<CInstCh>(
                    static_cast<uint8_t>(ch), this);
                // ポリフォニーを channel_map から設定
                uint8_t poly = 1;
                for (const auto& cm : config_->getChannelMap()) {
                    if (cm.midiCh == ch) { poly = static_cast<uint8_t>(cm.poly); break; }
                }
                instCh->setup(patchMgr_.get(), this, poly);
                channels_[p][ch] = std::move(instCh);
            }
        }

        processors_[p] = std::make_unique<MidiProcessor>(
            channels_[p], this, /*clockEnabled=*/(p == 0));
    }

    // 各チャンネルのデフォルト音色をロード
    for (int p = 0; p < MAX_MPUS; ++p) {
        if (!processors_[p]) continue;
        for (int ch = 0; ch < 16; ++ch) {
            auto* midicch = channels_[p][ch].get();
            if (midicch && midicch->isInst()) {
                midicch->progChange(0); // デフォルト音色
            }
        }
    }

    resetAllCtrl();
    allNoteOff();

    FITOM_LOG_INFO("FITOM initialized: "
        << getDeviceCount() << " devices, "
        << midiInputCount   << " MIDI inputs");
    return 0;
}

void CFITOM::exit(bool /*save*/)
{
    stopTimerThread();
    allNoteOff();
    FITOM_LOG_INFO("FITOM exited");
}

void CFITOM::initDevices()
{
    // DeviceFactory を使って IPort → ISoundDevice を生成する。
    // Config が IPort を所有し、CFITOM が ISoundDevice を所有する分離構造。
    // (Config は DeviceFactory に依存しない; 依存はここで断ち切る)
    int n = config_->getDeviceCount();
    devices_.clear();
    devices_.resize(n);

    for (int i = 0; i < n; ++i) {
        IPort*   port       = config_->getDevicePort(i);
        uint32_t deviceType = config_->getDeviceType(i);
        int      sampleRate = config_->getAudioSampleRate();

        if (!port) {
            FITOM_LOG_WARN("Device[" << i << "] '"
                << config_->getDeviceLabel(i) << "': port is null, skipping");
            continue;
        }
        if (deviceType == DEVICE_NONE) {
            FITOM_LOG_WARN("Device[" << i << "] '"
                << config_->getDeviceLabel(i) << "': deviceType unknown, skipping");
            continue;
        }

        // B-2: 2 ポートチップ (OPNA/OPN2/OPL3) の処理
        // プロファイルに extra_port が指定されていれば SplitPort を生成する。
        // 未指定 (= エミュレーター or 1ポートHW) なら port をそのまま渡す。
        IPort* extraPort = port;  // デフォルト: 1ポート (FmEnginePort はこれで動く)

        // B-2: DeviceEntry の port2 が設定されていれば SplitPort を生成
        IPort* port2 = config_->getDevicePort2(i);
        if (port2 && port2 != port) {
            auto sp = std::make_unique<SplitPort>(port, port2);
            extraPort = sp.get();
            splitPorts_.push_back(std::move(sp));
            FITOM_LOG_INFO("Device[" << i << "]: SplitPort created (extra_slot)");
        }

        // エミュレーター (FmEnginePort): port == extraPort → SplitPort 不要
        // HW 2ポート: extraPort が別 IPort → createCOPNA 内で SplitPort を利用
        auto dev = DeviceFactory::create(deviceType, port, sampleRate,
                                         (extraPort != port) ? extraPort : nullptr);
        if (!dev) {
            FITOM_LOG_ERR("Device[" << i << "] '"
                << config_->getDeviceLabel(i)
                << "': DeviceFactory::create failed (type=0x"
                << std::hex << deviceType << ")");
            continue;
        }

        dev->init();

        // SCC デバイスには SccWaveRegistry を注入する
        if (deviceType == DEVICE_SCC || deviceType == DEVICE_SCCP) {
            if (auto* scc = dynamic_cast<CSCC*>(dev.get())) {
                scc->setWaveRegistry(&patchMgr_->sccWaveRegistry());
                FITOM_LOG_DEBUG("Device[" << i << "]: SccWaveRegistry injected");
            }
        }

        // B-3: PCM/ADPCM デバイスには PcmBankRegistry を注入して初期化する
        if (auto* adpcm = dynamic_cast<CAdPcmBase*>(dev.get())) {
            adpcm->setPcmRegistry(&patchMgr_->pcmRegistry(), 0);
            adpcm->initPcmData();
            FITOM_LOG_DEBUG("Device[" << i << "]: PcmBankRegistry injected");
        }

        FITOM_LOG_INFO("Device[" << i << "]: "
            << config_->getDeviceLabel(i)
            << " → " << dev->getDescriptor());

        devices_[i] = std::move(dev);
    }
}

// ================================================================
//  デバイスアクセス
// ================================================================

ISoundDevice* CFITOM::getDevice(int index) const
{
    if (index < 0 || index >= static_cast<int>(devices_.size())) return nullptr;
    return devices_[index].get();
}

int CFITOM::getDeviceCount() const
{
    return config_ ? config_->getDeviceCount() : 0;
}

const DrumNote* CFITOM::getDrum(int bankNo, uint8_t prog, uint8_t midiNote) const
{
    if (!patchMgr_) return nullptr;
    const DrumPatch* dp = patchMgr_->resolveDrum(bankNo, prog);
    if (!dp) return nullptr;
    return dp->getNote(midiNote);
}

// ================================================================
//  タイマー・ポーリング
// ================================================================

void CFITOM::timerCallback(uint32_t tick)
{
    std::lock_guard<std::mutex> lk(processMutex_);
    for (int p = 0; p < MAX_MPUS; ++p) {
        if (processors_[p]) processors_[p]->timerCallback(tick);
    }
    // デバイスタイマー
    int n = getDeviceCount();
    for (int i = 0; i < n; ++i) {
        auto* dev = getDevice(i);
        if (dev) dev->timerCallback(tick);
    }
}

int CFITOM::pollingCallback()
{
    int ret = 0;
    std::lock_guard<std::mutex> lk(processMutex_);
    for (int p = 0; p < MAX_MPUS; ++p) {
        if (processors_[p]) {
            processors_[p]->pollingCallback();
            ++ret;
        }
    }
    return ret;
}

void CFITOM::midiClockCallback()
{
    std::lock_guard<std::mutex> lk(processMutex_);
    ++timerTick_;
    for (int p = 0; p < MAX_MPUS; ++p) {
        if (processors_[p]) processors_[p]->midiClockCallback(timerTick_);
    }
}

// ================================================================
//  グローバル操作
// ================================================================

void CFITOM::allNoteOff()
{
    for (int p = 0; p < MAX_MPUS; ++p) {
        for (int ch = 0; ch < 16; ++ch) {
            if (channels_[p][ch]) channels_[p][ch]->allNoteOff();
        }
    }
}

void CFITOM::resetAllCtrl()
{
    for (int p = 0; p < MAX_MPUS; ++p) {
        for (int ch = 0; ch < 16; ++ch) {
            if (channels_[p][ch]) channels_[p][ch]->resetAllCtrl();
        }
    }
}

void CFITOM::setMasterVolume(uint8_t vol)
{
    if (config_) config_->setMasterVolume(vol);
}

uint8_t CFITOM::getMasterVolume() const
{
    return config_ ? config_->getMasterVolume() : 100;
}

// ================================================================
//  タイマースレッド (非 MFC 環境用)
// ================================================================

void CFITOM::startTimerThread(uint32_t intervalMs)
{
    if (timerRunning_.load()) return;
    timerRunning_.store(true);
    timerThread_ = std::thread([this, intervalMs]() {
        timerThreadFunc(intervalMs);
    });
    FITOM_LOG_INFO("Timer thread started (" << intervalMs << "ms)");
}

void CFITOM::stopTimerThread()
{
    if (!timerRunning_.load()) return;
    timerRunning_.store(false);
    if (timerThread_.joinable()) timerThread_.join();
    FITOM_LOG_INFO("Timer thread stopped");
}

void CFITOM::timerThreadFunc(uint32_t intervalMs)
{
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    const auto interval = std::chrono::milliseconds(intervalMs);

    while (timerRunning_.load()) {
        next += interval;
        timerCallback(++timerTick_);
        std::this_thread::sleep_until(next);
    }
}

// ================================================================
//  MidiProcessor
// ================================================================

MidiProcessor::MidiProcessor(
    std::array<std::unique_ptr<IMidiCh>, 16>& channels,
    CFITOM* parent, bool clockEnabled)
    : channels_(channels), parent_(parent), clockEnabled_(clockEnabled)
{}

void MidiProcessor::receiveByte(const uint8_t* data, size_t len,
                                uint64_t /*timestampNs*/)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];

        // SysEx 中
        if (state_ == State::SysEx) {
            if (b == 0xF7) {
                processSysEx();
                state_ = State::Ready;
            } else if (sysexPt_ < sizeof(sysexBuf_)) {
                sysexBuf_[sysexPt_++] = b;
            }
            continue;
        }

        // ステータスバイト
        if (b & 0x80) {
            if (b == 0xF0) { // SysEx 開始
                sysexPt_ = 0;
                state_   = State::SysEx;
                continue;
            }
            if (b >= 0xF8) { // リアルタイムメッセージ
                if (b == 0xF8 && clockEnabled_) midiClockCallback(0);
                continue;
            }
            // ランニングステータスリセット
            currentStatus_ = b;
            msgBuf_[0]     = b;
            msgPt_         = 1;

            // 1バイトメッセージ
            if (b == 0xF6 || b == 0xF7) {
                msgPt_ = 0; continue;
            }
            // 2バイトメッセージ (0xC0-0xDF, 0xF1, 0xF3)
            uint8_t type = b & 0xF0;
            if (type == 0xC0 || type == 0xD0 || b == 0xF1 || b == 0xF3) {
                state_ = State::Wait1;
            } else {
                state_ = State::Wait2;
            }
            continue;
        }

        // データバイト
        if (state_ == State::Wait2) {
            msgBuf_[msgPt_++] = b;
            state_             = State::Wait1;
        } else if (state_ == State::Wait1) {
            msgBuf_[msgPt_++] = b;
            processMessage();
            // ランニングステータス用にリセット
            msgPt_ = 1;
            uint8_t type = msgBuf_[0] & 0xF0;
            if (type == 0xC0 || type == 0xD0) {
                state_ = State::Wait1;
            } else if (msgBuf_[0] < 0xF0) {
                state_ = State::Wait2;
            } else {
                state_ = State::Ready;
            }
        }
    }
}

void MidiProcessor::processMessage()
{
    uint8_t status = msgBuf_[0];
    uint8_t ch     = status & 0x0F;
    uint8_t type   = status & 0xF0;

    IMidiCh* midicch = channels_[ch].get();
    if (!midicch) return;

    switch (type) {
    case 0x90: // Note On
        if (msgBuf_[2] > 0) midicch->noteOn(msgBuf_[1], msgBuf_[2]);
        else                 midicch->noteOff(msgBuf_[1]);
        break;
    case 0x80: // Note Off
        midicch->noteOff(msgBuf_[1]);
        break;
    case 0xB0: // Control Change
        processControl(ch, msgBuf_[1], msgBuf_[2]);
        break;
    case 0xC0: // Program Change
        midicch->progChange(msgBuf_[1]);
        break;
    case 0xD0: // Channel Pressure (未使用)
        break;
    case 0xE0: // Pitch Bend
    {
        uint16_t pb = static_cast<uint16_t>(msgBuf_[1]) | (static_cast<uint16_t>(msgBuf_[2]) << 7);
        midicch->setPitchBend(pb);
        break;
    }
    case 0xA0: // Polyphonic Pressure (未使用)
        break;
    }
}

void MidiProcessor::processControl(uint8_t ch, uint8_t cc, uint8_t val)
{
    IMidiCh* midicch = channels_[ch].get();
    if (!midicch) return;

    switch (cc) {
    case 0:   midicch->bankSelMSB(val); break;
    case 1:   midicch->setModulation(val); break;
    case 2:   midicch->setBreathCtrl(val); break;
    case 4:   midicch->setFootCtrl(val); break;
    case 7:   midicch->setVolume(val); break;
    case 10:  midicch->setPanpot(val); break;
    case 11:  midicch->setExpression(val); break;
    case 32:  midicch->bankSelLSB(val); break;
    case 65:  midicch->setPortamento(val >= 64); break;
    case 66:  midicch->setSostenuto(val >= 64); break;
    case 67:  midicch->setSustain(val); break;
    case 68:  midicch->setLegato(val >= 64); break;
    case 5:   midicch->setPortTime(val); break;
    case 84:  midicch->setPortamento(true); break; // Source Note
    case 120: midicch->allNoteOff(); break;        // All Sound Off
    case 121: midicch->resetAllCtrl(); break;
    case 123: midicch->allNoteOff(); break;
    // RPN / NRPN
    case 98:  rpn_[ch].reg = (rpn_[ch].reg & 0x3F80) | val; rpn_[ch].isNrpn = true;  break;
    case 99:  rpn_[ch].reg = (rpn_[ch].reg & 0x007F) | (static_cast<uint16_t>(val) << 7); rpn_[ch].isNrpn = true; break;
    case 100: rpn_[ch].reg = (rpn_[ch].reg & 0x3F80) | val; rpn_[ch].isNrpn = false; break;
    case 101: rpn_[ch].reg = (rpn_[ch].reg & 0x007F) | (static_cast<uint16_t>(val) << 7); rpn_[ch].isNrpn = false; break;
    case 6:   // Data Entry MSB
    {
        uint16_t data = (static_cast<uint16_t>(val) << 7) | rpn_[ch].lsb;
        if (rpn_[ch].isNrpn) midicch->setNRPNRegister(rpn_[ch].reg, data);
        else                  midicch->setRPNRegister(rpn_[ch].reg, data);
        break;
    }
    case 38: rpn_[ch].lsb = val; break; // Data Entry LSB
    default:
        FITOM_LOG_DEBUG("CC#" << (int)cc << "=" << (int)val << " ch=" << (int)ch << " unhandled");
        break;
    }
}

void MidiProcessor::processSysEx() { /* 将来拡張 */ }

void MidiProcessor::timerCallback(uint32_t tick)
{
    for (int ch = 0; ch < 16; ++ch) {
        if (channels_[ch]) channels_[ch]->timerCallback(tick);
    }
}

void MidiProcessor::pollingCallback()
{
    for (int ch = 0; ch < 16; ++ch) {
        if (channels_[ch]) channels_[ch]->pollingCallback();
    }
}

void MidiProcessor::midiClockCallback(uint32_t tick)
{
    for (int ch = 0; ch < 16; ++ch) {
        if (channels_[ch]) channels_[ch]->midiClockCallback(tick);
    }
}

} // namespace fitom
