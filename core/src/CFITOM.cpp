// fitom/CFITOM.cpp
// FITOM コアシングルトン実装
//
// 旧 FITOM.cpp からの変更:
//   - boost::thread → std::thread
//   - FMVOICE* / CFMBank → PatchManager
//   - SCCI 依存を除去 (PortFactory 経由)
//   - tables.h (ROM::devmap) を FITOMdefine.h の定数で代替

#include "fitom/CFITOM.h"
#include "fitom/FnumUtils.h"
#include "fitom/IPort.h"
#include "fitom/DeviceFactory.h"
#include "fitom/MultiDevice.h"
#include "fitom/SccWaveData.h"
#include "fitom/PcmBankData.h"
#include "fitom/DrumData.h"
#include "fitom/Log.h"
#include "fitom/FITOMdefine.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
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
        for (int ch = 0; ch < 16; ++ch) {
            // GM規格準拠: MIDI ch10 (0-indexed: ch9) は固定でリズムチャンネル。
            // (channel_mapは廃止。ポリフォニー数もデバイス依存のため
            //  固定設定ではなく、CInstCh::progChange()実行時に解決された
            //  パッチのデバイスチャンネル数から動的に決定される)
            bool isRhythm = (ch == 9);
            if (isRhythm) {
                channels_[p][ch] = std::make_unique<CRhythmCh>(
                    static_cast<uint8_t>(ch), this);
            } else {
                auto instCh = std::make_unique<CInstCh>(
                    static_cast<uint8_t>(ch), this);
                instCh->setup(patchMgr_.get(), this);
                channels_[p][ch] = std::move(instCh);
            }
        }

        processors_[p] = std::make_unique<MidiProcessor>(
            channels_[p], this, /*clockEnabled=*/(p == 0));
    }

    // 各チャンネルのデフォルト音色をロード (GM準拠: bank0:0/prog0)。
    // CInstCh/CRhythmCh 両方に適用する。Program Changeを一度も受信せず
    // Note Onが来た場合でも即座に発音できるようにするため
    // (progChangeが未実行だとresolver_/currentPatch_が未設定のままとなり、
    //  該当チャンネルが永久に無音になってしまう)。
    for (int p = 0; p < MAX_MPUS; ++p) {
        if (!processors_[p]) continue;
        for (int ch = 0; ch < 16; ++ch) {
            auto* midicch = channels_[p][ch].get();
            if (midicch) {
                midicch->progChange(0); // デフォルト音色 (リズムCHはデフォルトドラムキット)
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
    // 冪等性ガード: 二重exit防止。
    // ~CFITOM(){ exit(); } という設計のため、アプリ側が明示的に
    // fitomInst.exit()を呼んだ後にプログラムが終了すると、
    // CFITOM::instance()(関数ローカルstatic)のデストラクタが、
    // プログラム終了時に自動的に2回目のexit()を呼んでしまう。
    // この時点でBoost.Logの静的状態が(他の翻訳単位の静的破棄順序により)
    // 既に破棄されている可能性があり、FITOM_LOG_INFO呼び出しが
    // クラッシュを引き起こしていた(fitom_cliでCtrl+C終了時に顕在化)。
    // このガードにより、2回目以降の呼び出しは何もせず安全に即return する。
    if (exited_) return;
    exited_ = true;

    stopTimerThread();
    allNoteOff();
    FITOM_LOG_INFO("FITOM exited");
}

// ── レイテンシ同期 ────────────────────────────────────────────────────────────
// 全デバイスの write→発音レイテンシを収集し、最大値に全デバイスを揃える。
// 物理チップ (HWPlugin_GetLatencySamples=0) は最大値分だけキューで遅らせる。
// FMエンジン内蔵hwif はすでに自身のバッファ分のレイテンシを持つため、
// 自身のレイテンシ == 最大値 となれば何もしない。
void CFITOM::syncDeviceLatency()
{
    // すべての HWPort を収集
    // Config が IPort を所有しており、HWPort かどうかは dynamic_cast で判定
    uint32_t maxLatency = 0;
    int n = config_->getDeviceCount();

    // パス1: 最大レイテンシを収集
    for (int i = 0; i < n; ++i) {
        auto* hwPort = dynamic_cast<HWPort*>(config_->getDevicePort(i));
        if (!hwPort) continue;
        uint32_t lat = hwPort->getLatencySamples();
        if (lat > maxLatency) maxLatency = lat;
        // extra_slot の port2 も確認
        auto* hwPort2 = dynamic_cast<HWPort*>(config_->getDevicePort2(i));
        if (hwPort2) {
            lat = hwPort2->getLatencySamples();
            if (lat > maxLatency) maxLatency = lat;
        }
    }

    if (maxLatency == 0) {
        FITOM_LOG_INFO("syncDeviceLatency: all devices are immediate (latency=0)");
        return;
    }

    FITOM_LOG_INFO("syncDeviceLatency: max_latency=" << maxLatency << " samples");

    // パス2: 全デバイスに最大レイテンシを設定
    for (int i = 0; i < n; ++i) {
        auto* hwPort = dynamic_cast<HWPort*>(config_->getDevicePort(i));
        if (hwPort) {
            hwPort->setDelaySamples(maxLatency);
            FITOM_LOG_DEBUG("Device[" << i << "] port: delay="
                << maxLatency << " samples");
        }
        auto* hwPort2 = dynamic_cast<HWPort*>(config_->getDevicePort2(i));
        if (hwPort2) {
            hwPort2->setDelaySamples(maxLatency);
            FITOM_LOG_DEBUG("Device[" << i << "] port2: delay="
                << maxLatency << " samples");
        }
    }
}

// MultiDev_new.cpp で定義される CLinearPanDevice 生成関数
extern std::unique_ptr<ISoundDevice> createCLinearPanDevice(ISoundDevice* left, ISoundDevice* right);

std::unique_ptr<ISoundDevice> CFITOM::createLeveledDevice(
    uint32_t deviceType, IPort* port, IPort* stereoPairPort,
    int sampleRate, IPort* extraPort, bool rhythmMode)
{
    auto dev = DeviceFactory::create(deviceType, port, sampleRate, extraPort, rhythmMode);
    if (!dev) return nullptr;
    dev->init();

    if (!stereoPairPort) return dev;

    // リニアステレオ化 (CLinearPanDevice): R側チップも生成してラップする。
    auto rdev = DeviceFactory::create(deviceType, stereoPairPort, sampleRate, nullptr, rhythmMode);
    if (!rdev) {
        FITOM_LOG_WARN("createLeveledDevice: stereo pair (R) creation failed, "
            "falling back to mono (L only)");
        return dev;
    }
    rdev->init();

    ISoundDevice* lRaw = dev.get();
    ISoundDevice* rRaw = rdev.get();
    spanSubChips_.push_back(std::move(dev));
    spanSubChips_.push_back(std::move(rdev));
    return fitom::createCLinearPanDevice(lRaw, rRaw);
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
        int      sampleRate = config_->getDeviceSampleRate(i);

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
        IPort* extraPort = port;  // デフォルト: 1ポート

        // B-2: DeviceEntry の port2 が設定されていれば SplitPort を生成
        IPort* port2 = config_->getDevicePort2(i);
        if (port2 && port2 != port) {
            auto sp = std::make_unique<SplitPort>(port, port2);
            extraPort = sp.get();
            splitPorts_.push_back(std::move(sp));
            FITOM_LOG_INFO("Device[" << i << "]: SplitPort created (extra_slot)");
        }

        // 1ポートのみ使うデバイス: port == extraPort → SplitPort 不要
        // HW 2ポート: extraPort が別 IPort → createCOPNA 内で SplitPort を利用
        IPort* stereoPairPort = config_->getDeviceStereoPairPort(i);
        auto dev = createLeveledDevice(deviceType, port, stereoPairPort, sampleRate,
                                       (extraPort != port) ? extraPort : nullptr,
                                       config_->getDeviceRhythmMode(i));
        if (!dev) {
            FITOM_LOG_ERR("Device[" << i << "] '"
                << config_->getDeviceLabel(i)
                << "': DeviceFactory::create failed (type=0x"
                << std::hex << deviceType << ")");
            continue;
        }

        // 同種デバイス自動束ね: spanGroups があれば追加のチップ(モノラルまたは
        // ステレオペア)を生成し CSpanDevice で束ねる (旧FITOMの isSpannable 相当)。
        int spanCount = config_->getDeviceSpanGroupCount(i);
        if (spanCount > 0) {
            auto spanDev = std::make_unique<CSpanDevice>();
            spanDev->addDevice(dev.get());
            spanSubChips_.push_back(std::move(dev));

            for (int k = 0; k < spanCount; ++k) {
                IPort* sp       = config_->getDeviceSpanGroupPrimary(i, k);
                IPort* spStereo = config_->getDeviceSpanGroupStereoPair(i, k);
                if (!sp) continue;
                auto subDev = createLeveledDevice(deviceType, sp, spStereo, sampleRate,
                                                   nullptr, config_->getDeviceRhythmMode(i));
                if (!subDev) {
                    FITOM_LOG_WARN("Device[" << i << "]: span sub-chip[" << k
                        << "] creation failed, skipped");
                    continue;
                }
                spanDev->addDevice(subDev.get());
                spanSubChips_.push_back(std::move(subDev));
            }
            FITOM_LOG_INFO("Device[" << i << "]: " << config_->getDeviceLabel(i)
                << " spanned across " << (spanCount + 1) << " physical chips ("
                << (int)spanDev->getChCount() << "ch total)");
            dev = std::move(spanDev);
        }

        // SCC デバイスには SccWaveRegistry を注入する (非対応チップは空実装で無視される)
        if (deviceType == DEVICE_SCC || deviceType == DEVICE_SCCP) {
            dev->setWaveRegistry(&patchMgr_->sccWaveRegistry());
            FITOM_LOG_DEBUG("Device[" << i << "]: SccWaveRegistry injected");
        }

        // B-3: PCM/ADPCM デバイスには PcmBankRegistry を注入して初期化する
        // (非対応チップは空実装で無視される)
        dev->setPcmRegistry(&patchMgr_->pcmRegistry(), 0);
        dev->initPcmData();

        FITOM_LOG_INFO("Device[" << i << "]: "
            << config_->getDeviceLabel(i)
            << " → " << dev->getDescriptor());

        devices_[i] = std::move(dev);
    }

    // ── レイテンシ同期 ────────────────────────────────────────────────────
    // 全デバイスの発音レイテンシ (write→実音 のサンプル数) を収集し、
    // 最大値に合わせて物理チップ側をキューイング遅延させる。
    // これにより「物理チップ + FMエンジン内蔵hwif」混在構成でも
    // ノート ON/OFF のタイミングが揃う。
    syncDeviceLatency();

    // マスターピッチを FnumRegistry と全デバイスに反映
    const double pitch = config_ ? config_->getMasterPitch() : 440.0;
    fitom::FnumRegistry::instance().setMasterPitch(pitch);
    for (auto& dev : devices_) {
        if (dev) dev->onMasterPitchChanged(pitch);
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

int CFITOM::findDeviceIndex(const ISoundDevice* dev) const
{
    if (!dev) return -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].get() == dev) return static_cast<int>(i);
    }
    return -1;
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

// ─── マスターピッチ ──────────────────────────────────────────────────────────

void CFITOM::setMasterPitch(double pitchHz)
{
    // 範囲チェック: 430〜450Hz
    pitchHz = std::clamp(pitchHz, 430.0, 450.0);

    // FnumRegistry キャッシュを更新 (setMasterPitch でキャッシュクリア)
    fitom::FnumRegistry::instance().setMasterPitch(pitchHz);

    // Config に保存
    if (config_) config_->setMasterPitch(pitchHz);

    // 全デバイスに通知 (発音中チャンネルの F-number を即時再計算)
    for (auto& dev : devices_) {
        if (dev) dev->onMasterPitchChanged(pitchHz);
    }

    FITOM_LOG_INFO("MasterPitch: " << pitchHz << " Hz");
}

double CFITOM::getMasterPitch() const
{
    return config_ ? config_->getMasterPitch() : 440.0;
}

// ─── Universal SysEx: マスターチューニング ──────────────────────────────────
// setMasterPitch()(ユーザー設定の絶対Hz基準、config_に保存)とは別に、
// SysEx由来の相対オフセット(セント/半音)を保持し、実際にFnumRegistryへ
// 反映する実効ピッチは両者を合成した値にする。ユーザー設定の基準Hz自体は
// 上書きしない (SysExのMaster Tuningを解除すれば元の基準Hzに戻る)。
void CFITOM::updateEffectiveMasterPitch()
{
    const double baseHz = getMasterPitch();
    const double semitoneOffset = static_cast<double>(masterCoarseTuneSemitones_)
                                 + static_cast<double>(masterFineTuneCents_) / 100.0;
    double effectiveHz = baseHz * std::pow(2.0, semitoneOffset / 12.0);
    effectiveHz = std::clamp(effectiveHz, 430.0, 450.0);

    fitom::FnumRegistry::instance().setMasterPitch(effectiveHz);
    for (auto& dev : devices_) {
        if (dev) dev->onMasterPitchChanged(effectiveHz);
    }
    FITOM_LOG_INFO("MasterPitch (effective): " << effectiveHz << " Hz (base=" << baseHz
        << "Hz, coarse=" << (int)masterCoarseTuneSemitones_
        << "semitones, fine=" << masterFineTuneCents_ << "cents)");
}

void CFITOM::setMasterFineTune(int16_t cents)
{
    masterFineTuneCents_ = cents;
    updateEffectiveMasterPitch();
}

void CFITOM::setMasterCoarseTune(int8_t semitones)
{
    masterCoarseTuneSemitones_ = semitones;
    updateEffectiveMasterPitch();
}

void CFITOM::setScaleTuning(const std::array<int8_t, 12>& table)
{
    scaleTuning_ = table;
    // 発音中の全ノートに即時反映する。各CInstCh/CRhythmChが
    // 次回のピッチ関連イベント(NoteOn/PitchBend等)でgetScaleTuningCents()
    // を参照するため、ここでは全チャンネルにapplyPitchBendToAll相当の
    // 再適用を促す (allNoteOff/resetAllCtrlと同様、全チャンネル走査)。
    for (int p = 0; p < MAX_MPUS; ++p) {
        for (int ch = 0; ch < 16; ++ch) {
            if (channels_[p][ch]) channels_[p][ch]->refreshPitch();
        }
    }
    FITOM_LOG_INFO("Scale/Octave Tuning updated");
}

// ─────────────────────────────────────────────────────────────────────────────

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

void MidiProcessor::switchChannelRole(uint8_t ch, bool toRhythm)
{
    if (ch >= 16) return;

    // 既存の発音を全て停止する。オブジェクト差し替え(unique_ptr再代入)で
    // 旧チャンネルオブジェクトが破棄される際、ChState::owner等が
    // ダングリングポインタになることを防ぐため、差し替え前に必ず行う。
    if (channels_[ch]) channels_[ch]->allNoteOff();

    if (toRhythm) {
        channels_[ch] = std::make_unique<CRhythmCh>(ch, parent_);
        FITOM_LOG_INFO("MIDI ch=" << (int)(ch + 1)
            << ": GM2 Bank Select MSB=0x78 — switched to rhythm channel");
    } else {
        auto instCh = std::make_unique<CInstCh>(ch, parent_);
        instCh->setup(&parent_->getPatchManager(), parent_);
        channels_[ch] = std::move(instCh);
        FITOM_LOG_INFO("MIDI ch=" << (int)(ch + 1)
            << ": GM2 Bank Select MSB — switched to melodic channel");
    }
    // GM2仕様: 役割切替後はデフォルト音色(bank0:0/prog0)から始まる
    // (CFITOM::init時の全チャンネル初期化と同じ扱い)。
    channels_[ch]->progChange(0);
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

    // GM2規格: Bank Select MSB(CC#0)の0x78(DEVICE_RHYTHM)/0x79は、
    // チャンネルの役割(リズム/メロディ)自体を動的に切り替える特殊予約値。
    // (旧FITOMのCMidiInst::Control()と同じロジック)。
    // この2値は通常のバンク選択値としては一切使わせず、常にここで消費する
    // (bankSelMSB()には転送しない)。これにより、CInstCh側の bankSelM_ に
    // 0x78/0x79が入ることはなく、CC#0=0x01-0x6Fを「直接モードのVoicePatchType
    // 指定」として一意に解釈できる(値空間の衝突を防ぐ設計)。
    if (cc == 0) {
        if (val == DEVICE_RHYTHM) {          // 0x78: リズムチャンネルへ切替
            if (midicch->isInst()) switchChannelRole(ch, /*toRhythm=*/true);
            return;
        }
        if (val == 0x79) {                   // 0x79: メロディチャンネルへ切替
            if (midicch->isRhythm()) switchChannelRole(ch, /*toRhythm=*/false);
            return;
        }
    }

    switch (cc) {
    case 0:   midicch->bankSelMSB(val); break;
    case 1:   midicch->setModulation(val); break;
    case 2:   midicch->setBreathCtrl(val); break;
    case 4:   midicch->setFootCtrl(val); break;
    case 7:   midicch->setVolume(val); break;
    case 10:  midicch->setPanpot(val); break;
    case 11:  midicch->setExpression(val); break;
    case 14:  midicch->setHwLfoDepth(val); break;  // 非標準: HW LFO Depth
    case 15:  midicch->setHwLfoRate(val); break;   // 非標準: HW LFO Rate
    case 32:  midicch->bankSelLSB(val); break;
    case 64:  midicch->setSustain(val); break;      // Sustain (Damper) Pedal
    case 65:  midicch->setPortamento(val >= 64); break;
    case 66:  midicch->setSostenuto(val >= 64); break;
    // 67: Soft Pedal — 非対応 (FM音源には直接対応するパラメータがないため)
    case 68:  midicch->setLegato(val >= 64); break;
    case 5:   midicch->setPortTime(val); break;
    case 76:  midicch->setSoftLfoRate(val); break;   // GM2 Sound Controller 7: Vibrato Rate
    case 77:  midicch->setSoftLfoDepth(val); break;  // GM2 Sound Controller 8: Vibrato Depth
    case 78:  midicch->setSoftLfoDelay(val); break;  // GM2 Sound Controller 9: Vibrato Delay
    case 84:  midicch->setPortamentoSource(val); break; // Portamento Control (Source Note)
    case 120: midicch->allSoundOff(); break;        // All Sound Off (force damp)
    case 121: midicch->resetAllCtrl(); break;
    case 123: midicch->allNoteOff(); break;
    // RPN / NRPN
    case 98:  rpn_[ch].reg = (rpn_[ch].reg & 0x3F80) | val; rpn_[ch].isNrpn = true;  break;
    case 99:  rpn_[ch].reg = (rpn_[ch].reg & 0x007F) | (static_cast<uint16_t>(val) << 7); rpn_[ch].isNrpn = true; break;
    case 100: rpn_[ch].reg = (rpn_[ch].reg & 0x3F80) | val; rpn_[ch].isNrpn = false; break;
    case 101: rpn_[ch].reg = (rpn_[ch].reg & 0x007F) | (static_cast<uint16_t>(val) << 7); rpn_[ch].isNrpn = false; break;
    case 6:   // Data Entry MSB
    {
        // RPN 7F/7F (RPN NULL): 明示的にRPN/NRPNの選択を解除する規格上の
        // 値。この状態でData Entryを受けても何も適用しない
        // (MIDI規格上の必須動作。現状はrpn_[ch].regの初期値も0x3FFFの
        //  ため、未選択状態と区別できるようにするための明示ガード)。
        if (rpn_[ch].reg == 0x3FFF) break;
        uint16_t data = (static_cast<uint16_t>(val) << 7) | rpn_[ch].lsb;
        if (rpn_[ch].isNrpn) midicch->setNRPNRegister(rpn_[ch].reg, data);
        else                  midicch->setRPNRegister(rpn_[ch].reg, data);
        break;
    }
    case 38: rpn_[ch].lsb = val; break; // Data Entry LSB
    case 96:  // Data Increment
        if (rpn_[ch].reg != 0x3FFF) midicch->dataIncrement(rpn_[ch].reg, rpn_[ch].isNrpn);
        break;
    case 97:  // Data Decrement
        if (rpn_[ch].reg != 0x3FFF) midicch->dataDecrement(rpn_[ch].reg, rpn_[ch].isNrpn);
        break;
    case 126: midicch->setMonoMode(val); break;  // Mono Mode On
    case 127: midicch->setPolyMode(); break;     // Poly Mode On
    default:
        FITOM_LOG_DEBUG("CC#" << (int)cc << "=" << (int)val << " ch=" << (int)ch << " unhandled");
        break;
    }
}

void MidiProcessor::processSysEx()
{
    // sysexBuf_には先頭の0xF0は含まれない(状態遷移時に除去済み)。
    // 構成: [0]=Manufacturer ID (1byte、またはExtended IDの場合00H+2byte)
    if (sysexPt_ < 1) return;

    uint8_t id0 = sysexBuf_[0];

    // ─── プライベート(メーカー固有) SysEx: manufacturer ID = 00H 48H 01H ───
    // (拡張ID形式。1byte目が00Hの場合、後続2byteでメーカーを識別する)
    // 将来実装用のスタブ分岐のみ用意する (processPrivateSysEx()参照)。
    if (id0 == 0x00 && sysexPt_ >= 3
        && sysexBuf_[1] == 0x48 && sysexBuf_[2] == 0x01) {
        processPrivateSysEx();
        return;
    }

    // ─── Universal SysEx (Real Time / Non-Realtime) ───────────────
    // 構成: [0]=ID(0x7E/0x7F) [1]=Device ID [2]=Sub-ID1 [3]=Sub-ID2 [4..]=データ
    if (sysexPt_ < 3) return;

    uint8_t id   = sysexBuf_[0];
    // uint8_t devId = sysexBuf_[1]; // Device ID判定は行わず、常に自分宛として扱う
    uint8_t sub1 = sysexBuf_[2];

    if (id != 0x7F) return; // Universal Real Time のみ対応 (Non-Realtime 0x7E は対象外)

    if (sub1 == 0x04 && sysexPt_ >= 6) {
        // ─── Device Control ───────────────────────────────────
        uint8_t sub2 = sysexBuf_[3];
        uint8_t lsb  = sysexBuf_[4];
        uint8_t msb  = sysexBuf_[5];
        uint16_t value14 = (static_cast<uint16_t>(msb) << 7) | lsb;

        switch (sub2) {
        case 0x01: // Master Volume (スタブ実装: MSBのみ使用、7bit精度)
            parent_->setMasterVolume(msb);
            FITOM_LOG_INFO("SysEx: Master Volume = " << (int)msb);
            break;
        case 0x03: { // Master Fine Tuning (14bit, center=0x2000, ±100cents)
            int16_t cents = static_cast<int16_t>(
                (static_cast<int32_t>(value14) - 8192) * 100 / 8192);
            parent_->setMasterFineTune(cents);
            FITOM_LOG_INFO("SysEx: Master Fine Tuning = " << cents << " cents");
            break;
        }
        case 0x04: { // Master Coarse Tuning (MSBのみ有効, center=0x40, ±64semitones)
            int8_t semitones = static_cast<int8_t>(msb) - 64;
            parent_->setMasterCoarseTune(semitones);
            FITOM_LOG_INFO("SysEx: Master Coarse Tuning = " << (int)semitones << " semitones");
            break;
        }
        default:
            FITOM_LOG_DEBUG("SysEx: Device Control sub2=0x" << std::hex << (int)sub2
                << " unhandled");
            break;
        }
    } else if (sub1 == 0x08 && sysexPt_ >= 4) {
        // ─── MIDI Tuning Standard (Realtime) ──────────────────
        uint8_t sub2 = sysexBuf_[3];
        if (sub2 == 0x08 && sysexPt_ >= 17) {
            // Scale/Octave Tuning (1byte形式)。
            // 本来はチャンネルマスク(複数バイト)で適用対象chを絞れるが、
            // 簡略実装としてマスクは読み飛ばし、常に全チャンネルへ適用する。
            // [4]=テーブル番号(未使用) [5..16]=12半音ぶんのオフセット
            // (各1バイト、0-127、中心64。100/64cents/step換算)
            std::array<int8_t, 12> table{};
            for (int i = 0; i < 12; ++i) {
                uint8_t raw = sysexBuf_[5 + i];
                table[i] = static_cast<int8_t>(
                    (static_cast<int32_t>(raw) - 64) * 100 / 64);
            }
            parent_->setScaleTuning(table);
            FITOM_LOG_INFO("SysEx: Scale/Octave Tuning updated (1byte form)");
        } else {
            FITOM_LOG_DEBUG("SysEx: MIDI Tuning Standard sub2=0x" << std::hex << (int)sub2
                << " unhandled");
        }
    }
}

// プライベート(メーカー固有)SysEx: manufacturer ID = 00H 48H 01H
// (拡張ID形式)。将来実装用のスタブのみ。呼び出し時点でsysexBuf_[0..2]が
// 00H 48H 01Hであることは呼び出し元(processSysEx)で確認済み。
// sysexBuf_[3]以降、sysexPt_までがメーカー固有のペイロード。
// SysEx(private, manufacturer 00H 48H 01H)によるHwPatchパラメータ
// オーバーライド。プロトコル:
//   [3]   sub-cmd (0x01固定、将来の拡張用に予約)
//   [4]   target-type (0x00=MIDIチャンネル / 0x01=プリセットバンク直接編集)
//   [5..] target-addr (target-typeにより可変長、下記参照)
//   [layerOffset]   layer (対象ToneLayerインデックス。target-type=0x01では無視)
//   [jsonOffset..]  JSONペイロード(ASCII、オーバーライドしたい
//                   フィールドのみを持つオブジェクト)
//
// target-addr:
//   target-type=0x00: [5]=MIDIチャンネル(0-15)                    (1byte)
//   target-type=0x01: [5]=VoicePatchType [6]=HwBankインデックス
//                      [7]=HwProg番号                               (3byte)
void MidiProcessor::processPrivateSysEx()
{
    if (sysexPt_ < 5) {
        FITOM_LOG_DEBUG("SysEx: private message too short (missing sub-cmd/target-type), ignored");
        return;
    }
    const uint8_t subCmd = sysexBuf_[3];
    if (subCmd != 0x01) {
        FITOM_LOG_DEBUG("SysEx: private sub-cmd=0x" << std::hex << (int)subCmd
            << std::dec << " unhandled");
        return;
    }

    const uint8_t targetType = sysexBuf_[4];
    size_t addrLen;
    if (targetType == 0x00)      addrLen = 1; // ch
    else if (targetType == 0x01) addrLen = 3; // voicePatchType, hwBank, hwProg
    else {
        FITOM_LOG_DEBUG("SysEx: HwPatch override target-type=0x" << std::hex << (int)targetType
            << std::dec << " unhandled");
        return;
    }

    const size_t layerOffset = 5 + addrLen;
    if (sysexPt_ < layerOffset + 1) {
        FITOM_LOG_DEBUG("SysEx: HwPatch override message too short (missing layer byte)");
        return;
    }
    const uint8_t layer = sysexBuf_[layerOffset];
    const size_t jsonOffset = layerOffset + 1;
    const std::string jsonText(reinterpret_cast<const char*>(&sysexBuf_[jsonOffset]),
                                sysexPt_ - jsonOffset);

    if (targetType == 0x00) {
        const uint8_t ch = sysexBuf_[5];
        if (ch >= 16) {
            FITOM_LOG_WARN("SysEx: HwPatch override invalid channel=" << (int)ch);
            return;
        }
        IMidiCh* midicch = channels_[ch].get();
        if (!midicch) return;
        if (!midicch->mergeHwPatchOverride(layer, jsonText)) {
            FITOM_LOG_WARN("SysEx: HwPatch override (channel) failed ch=" << (int)ch
                << " layer=" << (int)layer);
        }
        return;
    }

    // targetType == 0x01: プリセットバンク直接編集
    if (!parent_) return;
    const uint8_t voicePatchType = sysexBuf_[5];
    const uint8_t hwBank         = sysexBuf_[6];
    const uint8_t hwProg         = sysexBuf_[7];

    PatchManager& pm = parent_->getPatchManager();
    const uint32_t group = FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    HwBank* bank = pm.hwRegistry().findMutable(group, hwBank);
    if (!bank) {
        FITOM_LOG_WARN("SysEx: HwPatch override (bank) target not found: voicePatchType=0x"
            << std::hex << (int)voicePatchType << std::dec << " hwBank=" << (int)hwBank);
        return;
    }
    if (hwProg >= BANK_PROG_SIZE) {
        FITOM_LOG_WARN("SysEx: HwPatch override (bank) hwProg out of range: " << (int)hwProg);
        return;
    }
    HwPatch& target = bank->patches[hwProg];
    if (!target.isValid()) {
        // このSysExは既存プリセットの編集専用であり、空きスロットから
        // 新規パッチを作る用途ではない(id/nameを設定する手段が
        // この経路には無いため、マージしても発見不能な音色になる)。
        FITOM_LOG_WARN("SysEx: HwPatch override (bank) target slot is empty (hwBank="
            << (int)hwBank << " hwProg=" << (int)hwProg << "), ignored");
        return;
    }
    std::string err;
    if (!pm.mergeHwPatchFromJsonText(jsonText, target, &err)) {
        FITOM_LOG_WARN("SysEx: HwPatch override (bank) JSON parse failed: " << err);
    }
}

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
