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
#include <unordered_map>

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
    {DEVICE_OPNB,  "OPNB",  VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x200},
    {DEVICE_2610B, "2610B", VOICE_TYPE_FM4,  VOICE_GROUP_OPNA, 0x200},
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
    // OPL4AWM自身のレジスタ空間(0x00-0xF9)は0x100に収まる。実チップ上は
    // getHighBankOffset()により0x200オフセットされて配置されるため、
    // buildPhysicalChipList()側でオフセットと合算して表示サイズを求める。
    {DEVICE_OPL4AWM, "AWM", VOICE_TYPE_PCM,  VOICE_GROUP_PCM,  0x100},
    {DEVICE_NONE,  nullptr, 0,               0,                0},
};

const DevMapEntry* findDevMap(uint32_t deviceId) {
    for (int i = 0; kDevMap[i].devid != DEVICE_NONE; ++i) {
        if (kDevMap[i].devid == deviceId) return &kDevMap[i];
    }
    return nullptr;
}

// ================================================================
//  サブデバイス種別 → チャンネル名接頭辞 (チャンネルレベルメーター用)
//
//  kDevMapとは目的が異なる(kDevMapはOPN2/OPN2C/OPN2L等を別名として
//  区別するが、こちらは「見た目上どう呼ぶか」の分類なのでFM系を
//  まとめて"FM"にする、等)ため、別テーブルとして持つ。
// ================================================================
struct ChannelPrefixEntry {
    uint32_t    devid;
    const char* prefix;
};

static const ChannelPrefixEntry kChannelPrefixMap[] = {
    // FM系 (OPN/OPM/OPL/OPLL 各ファミリー) はまとめて "FM"
    {DEVICE_OPN,     "FM"}, {DEVICE_OPN2,   "FM"}, {DEVICE_OPNA,   "FM"},
    {DEVICE_OPNB,    "FM"}, {DEVICE_OPN2C,  "FM"}, {DEVICE_OPN2L,  "FM"},
    {DEVICE_2610B,   "FM"},
    {DEVICE_OPM,     "FM"}, {DEVICE_OPP,    "FM"}, {DEVICE_OPZ,    "FM"}, {DEVICE_OPZ2, "FM"},
    {DEVICE_OPL,     "FM"}, {DEVICE_OPL2,   "FM"},
    {DEVICE_OPLL,    "FM"}, {DEVICE_OPLL2,  "FM"}, {DEVICE_OPLLP,  "FM"},
    {DEVICE_OPLLX,   "FM"}, {DEVICE_VRC7,   "FM"},
    // OPL3は4OP/2OPの2つのサブデバイスに分かれる
    {DEVICE_OPL3,    "4OP"},
    {DEVICE_OPL3_2,  "2OP"},
    // 内蔵リズム音源
    {DEVICE_OPNA_RHY, "RHY"}, {DEVICE_OPL_RHY, "RHY"}, {DEVICE_OPLL_RHY, "RHY"},
    // PSG系
    {DEVICE_SSG,   "SSG"}, {DEVICE_DCSG, "DCSG"},
    {DEVICE_SCC,   "SCC"}, {DEVICE_SCCP, "SCC"},
    {DEVICE_SAA,   "SAA"},
    // ADPCM/PCM系
    {DEVICE_ADPCMA,       "PA"},
    {DEVICE_ADPCMB,       "PB"},
    {DEVICE_ADPCMB_OPNA,  "PB"},
    {DEVICE_ADPCMB_Y8950, "PB"},
    {DEVICE_ADPCM,        "PCM"},
    {DEVICE_PCMD8,        "PCM"},
    // AWM (サンプル音源)
    {DEVICE_OPL4AWM, "AWM"},
    {DEVICE_NONE, nullptr},
};

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

std::string CFITOM::getSubDeviceChannelPrefix(uint32_t deviceType) {
    for (int i = 0; kChannelPrefixMap[i].prefix != nullptr; ++i) {
        if (kChannelPrefixMap[i].devid == deviceType) return kChannelPrefixMap[i].prefix;
    }
    return "CH";
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

    // MPU(MAX_MPUS=4)は常に全面分生成する。実際にMIDIバイトが流れ込むか
    // どうか(=物理/仮想MIDI入力ポートが割り当てられているか)はMidiProcessor
    // の存在とは独立な関心事であるため(GUIのMIDIポート設定ダイアログから、
    // 実行中に未割り当てのMPUへ新たにポートを割り当てられるようにするため、
    // 2026年7月に変更。以前はconfig_->getMidiInputCount()分しか生成せず、
    // 未設定のMPUにはMidiProcessor自体が存在しなかった)。
    for (int p = 0; p < MAX_MPUS; ++p) {
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
            channels_[p], this, /*clockEnabled=*/(p == 0), /*mpuIndex=*/p);
    }

    // SF2直行パス: プロファイルのsf2_channel_windows[]を初期状態として
    // 窓テーブルへ反映する(2026年7月新設。演奏中のプライベートSysEx
    // sub-cmd 0x04によるsetSf2ChannelWindow()呼び出しと等価な初期化)。
    for (const auto& w : config_->getSf2ChannelWindows()) {
        setSf2ChannelWindow(w.mpu, w.ch, w.fluidsynthChan);
    }

    // 内部用MIDIパイプ(backends/midi_pipe)専用のチャンネル/MidiProcessorを
    // 構築する。MAX_MPUS本と同様に常に生成する(プロファイルのmidi_backend/
    // midi_inputs設定を一切関知しない。実際にパイプDLLがロード・オープン
    // されるかどうかはアプリ層[FITOMBridge::initInternalMidiPipe()等]の
    // 責務)。clockEnabled=falseとする理由: パッチエディタが誤って0xF8
    // (MIDIクロック)を送っても、MPU0が担うマスタークロック挙動に影響
    // させないため。
    for (int ch = 0; ch < 16; ++ch) {
        bool isRhythm = (ch == 9);
        if (isRhythm) {
            internalPipeChannels_[ch] = std::make_unique<CRhythmCh>(
                static_cast<uint8_t>(ch), this);
        } else {
            auto instCh = std::make_unique<CInstCh>(
                static_cast<uint8_t>(ch), this);
            instCh->setup(patchMgr_.get(), this);
            internalPipeChannels_[ch] = std::move(instCh);
        }
    }
    internalPipeProcessor_ = std::make_unique<MidiProcessor>(
        internalPipeChannels_, this, /*clockEnabled=*/false);

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
    for (int ch = 0; ch < 16; ++ch) {
        if (internalPipeChannels_[ch]) internalPipeChannels_[ch]->progChange(0);
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

    // HWプラグイン(オーディオストリーム等のバックグラウンドスレッドを
    // 内部で持つ可能性がある)を、プロセスの静的破棄が始まるより前に
    // 明示的にシャットダウンする。config_->getHWPluginRegistry()の
    // 参照が0になった時点でDLLがアンロードされるが、それより前に
    // HWPlugin_Shutdown()(エクスポートされていれば)を呼んでおくことで、
    // DLLアンロード時のスレッド停止処理が暗黙の静的破棄タイミングに
    // 依存しないようにする(Windowsでのローダーロック起因のフリーズを
    // 避けるため。2026年7月、詳細はIHWPlugin.hのHWPlugin_Shutdown宣言
    // コメント参照)。
    if (config_) config_->getHWPluginRegistry().closeAll();

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

// deviceTypeが同一物理ポートの中で、自分専用の高位バンク(0を起点とする
// 自分のレジスタ空間より上位)へオフセットして配置される場合、そのオフセット
// 量を返す。該当しないdeviceTypeは0(オフセット不要=低位ポートのまま)。
// resolveHighBankPort()(実際のポート差し替え)と
// buildPhysicalChipList()(レジスタダンプモニターの表示サイズ算出)の
// 両方から参照される単一の真実の情報源。
uint16_t CFITOM::getHighBankOffset(uint32_t deviceType)
{
    // ADPCM-A(YM2610/2610B)・ADPCM-B(YM2608=OPNA)は実チップ上「port2」
    // (アドレス0x100以降)に配置されるレジスタ体系(ADPCM-B[YM2610/2610B]は
    // この対象外、低位ポートのままで正しい)。
    if (deviceType == DEVICE_ADPCMB_OPNA || deviceType == DEVICE_ADPCMA) return 0x100;
    // DEVICE_OPL4AWM(OPL4のAWM/PCM部)は、FM部が使う2バンク
    // (アドレス0x000-0x1FF)とは別の3つ目のレジスタバンク(アドレス0x200
    // 以降)に配置される。
    if (deviceType == DEVICE_OPL4AWM) return 0x200;
    return 0;
}

// 上記オフセットが必要なデバイスの、portを高位ポート側へ差し替える。
// configuredPort2(プロファイルのextra_port等で明示された物理ポート)が
// あればそれを使い、無ければOffsetPort(port,offset)を自前で生成して
// offsetPorts_で寿命管理する。
IPort* CFITOM::resolveHighBankPort(uint32_t deviceType, IPort* port, IPort* configuredPort2)
{
    uint16_t offset = getHighBankOffset(deviceType);
    if (offset == 0) return port;
    if (configuredPort2) return configuredPort2;
    offsetPorts_.push_back(std::make_unique<OffsetPort>(port, offset));
    return offsetPorts_.back().get();
}

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
    pendingSubDevices_.clear();

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
            if (config_->isSf2Device(i)) {
                // SF2直行パス(docs/sf2-fluidsynth-integration.md参照)用の
                // ラッパーデバイス。ISoundDevice生成は意図的にスキップし、
                // MidiProcessor::processMessage()からの生MIDIバイト列転送
                // 先としてIPortのみ記録する(プロファイル検証により
                // devices[]内でこの分岐に入るのは高々1回のみ)。
                sf2Port_ = port;
                FITOM_LOG_INFO("Device[" << i << "] '" << config_->getDeviceLabel(i)
                    << "': SF2 direct-path wrapper device (ISoundDevice creation "
                       "intentionally skipped)");
            } else {
                FITOM_LOG_WARN("Device[" << i << "] '"
                    << config_->getDeviceLabel(i) << "': deviceType unknown, skipping");
            }
            continue;
        }

        // ADPCM-A/ADPCM-B(OPNA)・OPL4AWMは高位ポート側へ差し替える(下記
        // resolveHighBankPort()参照)。該当しないデバイスタイプでは
        // portをそのまま返すだけの no-op。
        port = resolveHighBankPort(deviceType, port, config_->getDevicePort2(i));

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

        // チャンネルレベルメーター用: このデバイス自身(span/stereo展開前、
        // 単一物理ポート単位)をbuildPhysicalChipList()が後で拾えるよう
        // 記録しておく。stereoPairPort指定時はcreateLeveledDevice()が
        // 既にCLinearPanDeviceへラップして返しており、L/R個別のch構成を
        // 安定して取り出せないため対象外とする(既知の制限)。
        if (!stereoPairPort) {
            if (auto* hwPort = dynamic_cast<HWPort*>(config_->getDevicePort(i))) {
                pendingSubDevices_.push_back({hwPort, dev.get(), deviceType});
            }
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
                // 束ねられた物理チップは代表デバイス(deviceType)と同じ
                // VoicePatchTypeだが、実装クラスが異なる場合がある
                // (例: OPNB=COPNBはOPN2/OPNA=COPNAと同じVOICE_PATCH_OPN2だが
                // ch0/ch3を無効化した別クラス)。代表のdeviceTypeを流用せず、
                // このポート本来のdeviceTypeを使う。
                uint32_t subDeviceType = config_->getDeviceSpanGroupDeviceType(i, k);
                // dedupキー用に、resolveHighBankPort()で差し替えられる前の
                // 元ポート(config_->getDeviceSpanGroupPrimary()と同一)を
                // 別途保持しておく(buildPhysicalChipList()側のspanGroups
                // 登録と同じポインタで突き合わせる必要があるため)。
                auto* spHwPort = dynamic_cast<HWPort*>(sp);
                // 代表デバイスと同様、ADPCM-A/ADPCM-B(OPNA)・OPL4AWMは高位
                // ポートへ差し替える(spanGroup配下にはconfiguredPort2の概念が
                // 無いため常にOffsetPortを自前生成する)。
                sp = resolveHighBankPort(subDeviceType, sp, nullptr);
                // rhythm_modeも代表デバイス(config_->getDeviceRhythmMode(i))を
                // 流用せず、このポート本来の値を使う(deviceTypeと同じ理由。
                // 2026年8月修正: 以前は代表の値を全サブチップへ適用していた
                // ため、rhythm_mode:trueのチップが別のrhythm_mode:falseチップと
                // spanGroupとして束ねられると、束ねられた側のch6-8無効化が
                // 効かなくなっていた[逆に代表側がtrueだと束ねられた側にも
                // 無効化が誤って波及していた])。
                bool subRhythmMode = config_->getDeviceSpanGroupRhythmMode(i, k);
                auto subDev = createLeveledDevice(subDeviceType, sp, spStereo, sampleRate,
                                                   nullptr, subRhythmMode);
                if (!subDev) {
                    FITOM_LOG_WARN("Device[" << i << "]: span sub-chip[" << k
                        << "] creation failed, skipped");
                    continue;
                }
                FITOM_LOG_INFO("Device[" << i << "]: span sub-chip[" << k << "]: "
                    << subDev->getDescriptor());
                spanDev->addDevice(subDev.get());
                // チャンネルレベルメーター用(上記の代表デバイスと同じ理由)。
                // spStereo指定時はcreateLeveledDevice()がCLinearPanDeviceへ
                // ラップして返すため対象外とする(既知の制限)。
                if (spHwPort && !spStereo) {
                    pendingSubDevices_.push_back({spHwPort, subDev.get(), subDeviceType});
                }
                spanSubChips_.push_back(std::move(subDev));
            }
            // getChCount()はチャンネルアドレス空間(enableCh(false)で無効化された
            // chも含む、例: COPNBのch0/ch3)、getAvailableChs()は実際に発音可能な
            // ch数。COPNBのように一部chが恒常的に無効化されているチップが束ねに
            // 混ざると両者が乖離するため、両方をログに出す
            // (2026年7月、ステージング環境からの指摘: 束ね合計chが実際の
            //  発音可能数と食い違って見えると報告があった)。
            FITOM_LOG_INFO("Device[" << i << "]: " << config_->getDeviceLabel(i)
                << " spanned across " << (spanCount + 1) << " physical chips ("
                << (int)spanDev->getChCount() << "ch addressable, "
                << (int)spanDev->getAvailableChs() << "ch available)");
            dev = std::move(spanDev);
        }

        // SCC デバイスには SccWaveRegistry を注入する (非対応チップは空実装で無視される)
        if (deviceType == DEVICE_SCC || deviceType == DEVICE_SCCP) {
            dev->setWaveRegistry(&patchMgr_->sccWaveRegistry());
            FITOM_LOG_DEBUG("Device[" << i << "]: SccWaveRegistry injected");
        }

        // B-3: PCM/ADPCM デバイスには PcmBankRegistry を注入して初期化する
        // (非対応チップは空実装で無視される)。バンク番号は、まずこの
        // デバイスのdeviceType(DEVICE_ADPCMB_OPNA/DEVICE_ADPCMB等)に完全
        // 一致するpcm_banksエントリ(profile.jsonでchip指定されたもの)を
        // 優先して逆引きする。OPNA(YM2608)とOPNB/OPNBB(YM2610/2610B)の
        // ADPCM-Bは同一のVoicePatchTypeを使うが、実際の波形バイナリの
        // バウンダリ整列(OPNA=32byte、OPNB/OPNBB=256byte)が異なり同じ
        // オフセットテーブルを共有できないため(2026-07-24、実機ログで
        // OPNBが誤ってOPNA用オフセットを使っていたことが判明)、
        // PcmBank::deviceTypeで物理チップ単位に明示区別できるようにした。
        // chip指定が無い(=deviceType不一致)場合は、従来通りこのデバイスの
        // VoicePatchType(ADPCM-B/ADPCM-A/PCM-D8)に一致するpcm_banks
        // エントリ(groupのみ指定)にフォールドバックする。複数の異なる
        // PCMバンク(コーデックの異なるADPCM-A用/ADPCM-B用等)を同一
        // プロファイル内で併用できるようにするため(2026年7月、以前は
        // bankNo=0固定で全PCMデバイスが同じバンクを共有しており、2種目
        // 以降のPCMバンクが常に無視されていた不具合を修正)。どちらも
        // 一致するバンクが無い場合は、従来通りbank0にフォールバックする
        // (group/chipいずれも指定しない旧来のpcm_banks記述との後方互換)。
        {
            int pcmBankNo = patchMgr_->pcmRegistry().findBankNoForDeviceType(deviceType);
            if (pcmBankNo < 0) {
                uint8_t vpt = FITOMConfig::deviceTypeToVoicePatchType(deviceType);
                pcmBankNo = patchMgr_->pcmRegistry().findBankNoForVoicePatchType(vpt);
            }
            if (pcmBankNo < 0) pcmBankNo = 0;
            dev->setPcmRegistry(&patchMgr_->pcmRegistry(), pcmBankNo);
        }
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

    // レジスタダンプモニター用、物理チップ単位の一覧を構築する。
    buildPhysicalChipList();
}

// ================================================================
//  物理チップ列挙 (レジスタダンプモニター用)
// ================================================================
void CFITOM::buildPhysicalChipList()
{
    physicalChips_.clear();
    // 同一HWPortを共有するデバイス(サブデバイス自動生成や、2ポートチップの
    // port/port2)を1つの物理チップにまとめるためのdedupテーブル。
    std::unordered_map<HWPort*, size_t> portToChip;

    // primary(+secondary)を1つの物理チップとして登録する。
    // primaryが既に登録済み(=兄弟デバイスが同じ物理ポートを共有している)
    // 場合はエントリを新設しない(代表ラベルは最初に登録したデバイスの
    // ものを使う)が、表示サイズ(dumpSize)は兄弟サブデバイス側の方が
    // 大きい高位バンクを使うことがある(例: OPL4の場合、最初に登録される
    // FM部[DEVICE_OPL3、0x000-0x1FF]より後から登録されるAWM部
    // [DEVICE_OPL4AWM、0x200-0x2FF]の方が高位)ため、合流時にも都度
    // 必要サイズを比較して広げる(2026年7月、ユーザー指摘で発覚: 以前は
    // 最初に登録されたサブデバイスのdeviceTypeでのみdumpSizeを決めており、
    // 後続の高位バンクサブデバイスの分がレジスタダンプの表示範囲から
    // 漏れていた)。
    // 戻り値: 登録した(または既存の)physicalChips_内のインデックス。
    // primaryがnullptrの場合はSIZE_MAXを返す。
    auto registerChip = [&](HWPort* primary, HWPort* secondary,
                             const std::string& label, uint32_t deviceType) -> size_t {
        if (!primary) return static_cast<size_t>(-1);
        // このdeviceTypeが実際に必要とする表示サイズ。高位バンクへ
        // オフセットされるデバイス(getHighBankOffset()参照)は、その
        // オフセット+自身のレジスタ空間サイズが必要な範囲になる
        // (該当しないデバイスはoffset=0なので単純にgetDeviceRegSize()と同じ)。
        uint32_t required = static_cast<uint32_t>(getHighBankOffset(deviceType))
                           + getDeviceRegSize(deviceType);

        auto it = portToChip.find(primary);
        if (it != portToChip.end()) {
            PhysicalChipInfo& existing = physicalChips_[it->second];
            // 分離した物理ポート2つ(secondary構成)は常に0x200固定のため対象外。
            if (!existing.port2 && required > existing.dumpSize) {
                existing.dumpSize = required;
            }
            return it->second;
        }
        size_t idx = physicalChips_.size();
        PhysicalChipInfo info;
        info.label        = label;
        info.deviceType   = deviceType;
        info.physicalName = primary->getPhysicalChipName();
        info.port         = primary;
        info.port2        = secondary;
        if (secondary) {
            // 分離した物理ポート2つ (SPFM 2スロット構成等): 各ポート
            // ローカル0x000-0x0FFを0x000/0x100にpackするため常に0x200。
            info.dumpSize = 0x200;
        } else if (required > info.dumpSize) {
            info.dumpSize = required;
        }
        physicalChips_.push_back(info);
        portToChip[primary] = idx;
        if (secondary) portToChip[secondary] = idx;
        return idx;
    };

    int n = config_->getDeviceCount();
    for (int i = 0; i < n; ++i) {
        // DEVICE_NONEのエントリ(chip:"SF2"のSF2直行パス用ラッパー、または
        // 単に未知のchip文字列)はinitDevices()がISoundDevice生成自体を
        // スキップしている(実レジスタ空間を持たない)ため、レジスタダンプ
        // モニターの物理チップ一覧にも登録しない(2026年7月新設。以前は
        // ここでdeviceTypeを一切見ておらず、SF2ラッパーのIPortへ生MIDI
        // バイト列を書き込んだ際のシャドウレジスタが、無意味な「物理チップ」
        // としてそのままRegisterDumpWindowに表示されてしまっていた)。
        if (config_->getDeviceType(i) == DEVICE_NONE) continue;

        auto* port  = dynamic_cast<HWPort*>(config_->getDevicePort(i));
        auto* port2 = dynamic_cast<HWPort*>(config_->getDevicePort2(i));
        registerChip(port, port2, config_->getDeviceLabel(i), config_->getDeviceType(i));

        // 物理的にL/R固定配線されたステレオペア (CLinearPanDevice) の
        // 相方(R側)も、独立した物理チップとして登録する。
        auto* stereoPort = dynamic_cast<HWPort*>(config_->getDeviceStereoPairPort(i));
        registerChip(stereoPort, nullptr,
                     config_->getDeviceLabel(i) + " (stereo pair)",
                     config_->getDeviceType(i));

        // 同種デバイス自動束ね (spanGroups) の各要素も、それぞれ独立した
        // 物理チップ(束ねられているのはCFITOM::initDevices()側のCSpanDevice
        // 単位であり、物理チップとしては別々のためここでも個別に登録する)。
        int spanCount = config_->getDeviceSpanGroupCount(i);
        for (int k = 0; k < spanCount; ++k) {
            auto* sp       = dynamic_cast<HWPort*>(config_->getDeviceSpanGroupPrimary(i, k));
            auto* spStereo = dynamic_cast<HWPort*>(config_->getDeviceSpanGroupStereoPair(i, k));
            uint32_t subType = config_->getDeviceSpanGroupDeviceType(i, k);
            registerChip(sp, nullptr,
                         config_->getDeviceLabel(i) + " (span " + std::to_string(k) + ")",
                         subType);
            registerChip(spStereo, nullptr,
                         config_->getDeviceLabel(i) + " (span " + std::to_string(k) + " stereo pair)",
                         subType);
        }
    }

    // チャンネルレベルメーター用のサブデバイス内訳を反映する。
    // initDevices()の構築ループがspan/stereo展開“前”に記録しておいた
    // pendingSubDevices_(port,device,deviceType)を、上記で確定した物理
    // チップ一覧(portToChip)へ突き合わせる。同種デバイス自動束ね
    // (spanGroups)で他デバイスに吸収されたデバイスも、その吸収先が
    // 登録したポートと同じportを持つため、ここで正しく同じ物理チップの
    // サブデバイスとして復元される(2026年7月、ユーザー報告により発覚:
    // 以前はspanGroupsを持つ/持たれる側の両方をチャンネルレベルメーターの
    // 対象外としていたため、同系統FMチップが複数ある構成でチャンネルが
    // 大量に欠落していた)。
    for (const auto& psd : pendingSubDevices_) {
        auto it = portToChip.find(psd.port);
        if (it == portToChip.end()) continue;
        PhysicalChipSubDevice sd;
        sd.device     = psd.device;
        sd.deviceType = psd.deviceType;
        sd.chCount    = psd.device->getChCount();
        physicalChips_[it->second].subDevices.push_back(sd);
    }

    FITOM_LOG_INFO("buildPhysicalChipList: " << physicalChips_.size() << " physical chip(s)");
}

std::vector<uint8_t> CFITOM::getPhysicalChipRegisterDump(int index) const
{
    if (index < 0 || index >= static_cast<int>(physicalChips_.size())) return {};
    const auto& info = physicalChips_[index];
    std::vector<uint8_t> result(info.dumpSize, 0);
    if (info.port2) {
        // 分離した物理ポート2つ: それぞれのローカルアドレス0x000-0x0FFを
        // 読み、0x000/0x100に配置する。
        if (info.port)  info.port->getShadowRegRange(0x000, result.data(), 0x100);
        if (info.port2) info.port2->getShadowRegRange(0x000, result.data() + 0x100, 0x100);
    } else if (info.port) {
        // 単一物理ポート: 高位アドレス(0x100以降)もチップドライバが同じ
        // HWPortへ直接(OPL3等)またはOffsetPort経由(OPNA/OPN2等、内部で
        // +0x100して同じportへ書く)で書き込んでいるため、一括で読み出せる。
        info.port->getShadowRegRange(0x000, result.data(), info.dumpSize);
    }
    return result;
}

std::vector<PhysicalChipChannelState> CFITOM::getPhysicalChipChannelStates(int index) const
{
    std::vector<PhysicalChipChannelState> result;
    if (index < 0 || index >= static_cast<int>(physicalChips_.size())) return result;

    for (const auto& sub : physicalChips_[index].subDevices) {
        if (!sub.device) continue;
        for (uint8_t ch = 0; ch < sub.chCount; ++ch) {
            const ChState* cs = sub.device->getChState(ch);
            PhysicalChipChannelState st;
            if (cs) {
                st.sounding  = cs->isActive();
                st.velocity  = cs->velocity;
                st.enabled   = cs->isEnabled();
                st.noteOnSeq = cs->noteOnSeq;
            }
            result.push_back(st);
        }
    }
    return result;
}

std::vector<PhysicalChipChannelState> CFITOM::getLogicalDeviceChannelStates(int deviceIndex) const
{
    std::vector<PhysicalChipChannelState> result;
    ISoundDevice* dev = getDevice(deviceIndex);
    if (!dev) return result;

    uint8_t chCount = dev->getChCount();
    for (uint8_t ch = 0; ch < chCount; ++ch) {
        const ChState* cs = dev->getChState(ch);
        PhysicalChipChannelState st;
        if (cs) {
            st.sounding  = cs->isActive();
            st.velocity  = cs->velocity;
            st.enabled   = cs->isEnabled();
            st.noteOnSeq = cs->noteOnSeq;
        }
        result.push_back(st);
    }
    return result;
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
    if (internalPipeProcessor_) internalPipeProcessor_->timerCallback(tick);
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
    if (internalPipeProcessor_) {
        internalPipeProcessor_->pollingCallback();
        ++ret;
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
    // clockEnabled_=falseのため内部的には無視されるが、他系統と同じ形にしておく。
    if (internalPipeProcessor_) internalPipeProcessor_->midiClockCallback(timerTick_);
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
    for (int ch = 0; ch < 16; ++ch) {
        if (internalPipeChannels_[ch]) internalPipeChannels_[ch]->allNoteOff();
    }
}

void CFITOM::resetAllCtrl()
{
    for (int p = 0; p < MAX_MPUS; ++p) {
        for (int ch = 0; ch < 16; ++ch) {
            if (channels_[p][ch]) channels_[p][ch]->resetAllCtrl();
        }
    }
    for (int ch = 0; ch < 16; ++ch) {
        if (internalPipeChannels_[ch]) internalPipeChannels_[ch]->resetAllCtrl();
    }
}

// ─── マスターピッチ ──────────────────────────────────────────────────────────

// SF2直行パス(docs/sf2-fluidsynth-integration.md参照): マスターボリューム/
// マスターピッチも、新規プロトコルを設計せず既存のGM2 Universal Realtime
// SysExをそのままHWPlugin_WriteBlock経由でSF2ラッパーへ転送するだけで
// 済ませる(8.1節既存のマスターチューニングSysExと同じバイト列)。
namespace {
void buildGm2MasterVolumeSysEx(uint8_t (&out)[8], uint8_t vol)
{
    out[0] = 0xF0; out[1] = 0x7F; out[2] = 0x7F; out[3] = 0x04; out[4] = 0x01;
    out[5] = 0x00;                      // ll (未使用、7bit精度のみ使用)
    out[6] = static_cast<uint8_t>(vol & 0x7F);
    out[7] = 0xF7;
}

void buildGm2MasterFineTuneSysEx(uint8_t (&out)[8], double pitchHz)
{
    double cents = 1200.0 * std::log2(pitchHz / 440.0);
    cents = std::clamp(cents, -100.0, 100.0);
    int value14 = static_cast<int>(std::lround(cents * 8192.0 / 100.0)) + 8192;
    value14 = std::clamp(value14, 0, 16383);
    out[0] = 0xF0; out[1] = 0x7F; out[2] = 0x7F; out[3] = 0x04; out[4] = 0x03;
    out[5] = static_cast<uint8_t>(value14 & 0x7F);        // ll
    out[6] = static_cast<uint8_t>((value14 >> 7) & 0x7F); // mm
    out[7] = 0xF7;
}
} // namespace

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

    // SF2直行パス: 既存のGM2 Universal Realtime SysEx(マスターファイン
    // チューニング)をそのまま転送する(docs/sf2-fluidsynth-integration.md参照)。
    if (sf2Port_) {
        uint8_t buf[8];
        buildGm2MasterFineTuneSysEx(buf, pitchHz);
        sf2Port_->writeBurst(0, buf, sizeof(buf));
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
    for (int ch = 0; ch < 16; ++ch) {
        if (internalPipeChannels_[ch]) internalPipeChannels_[ch]->refreshPitch();
    }
    FITOM_LOG_INFO("Scale/Octave Tuning updated");
}

// ─────────────────────────────────────────────────────────────────────────────

void CFITOM::setMasterVolume(uint8_t vol)
{
    if (config_) config_->setMasterVolume(vol);
    if (sf2Port_) {
        uint8_t buf[8];
        buildGm2MasterVolumeSysEx(buf, vol);
        sf2Port_->writeBurst(0, buf, sizeof(buf));
    }
}

uint8_t CFITOM::getMasterVolume() const
{
    return config_ ? config_->getMasterVolume() : 100;
}

// ─── SF2直行パス (docs/sf2-fluidsynth-integration.md参照、2026年7月新設) ───

bool CFITOM::routeSf2ChannelMessage(int mpu, uint8_t ch, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (mpu < 0 || mpu >= MAX_MPUS || ch >= 16) return false;
    Sf2WindowState& w = sf2Windows_[mpu][ch];
    if (!w.assigned) return false;

    // 窓には含まれるがSF2デバイス未接続(プラグインロード失敗等)の場合は、
    // ネイティブ経路へフォールバックせず単純に読み捨てる(2.6節: 窓に
    // 含まれる限り常にこの経路として動作する)。
    if (!sf2Port_) return true;

    const uint8_t type = status & 0xF0;

    if (type == 0xB0) { // Control Change
        const uint8_t cc = d1, val = d2;
        if (cc == 0) return true; // CC#0(バンクセレクトMSB)は使用しない
        if (cc == 32) {
            // CC#32(バンクセレクトLSB) → sf2_banksのインデックスとして解決する。
            // 該当エントリが無い場合は単純に読み捨てる(直前の解決状態は
            // そのまま維持し、ソフトフォールバックは行わない)。
            Sf2BankRegistry::Resolved r;
            if (config_->getSf2BankRegistry().resolve(val, r)) {
                w.bankResolved   = true;
                w.soundfontIndex = r.soundfontIndex;
                w.sf2Bank        = r.sf2Bank;
            }
            return true;
        }
        uint8_t buf[3] = { static_cast<uint8_t>(0xB0 | w.fluidsynthChan), cc, val };
        sf2Port_->writeBurst(0, buf, sizeof(buf));
        return true;
    }

    if (type == 0xC0) { // Program Change
        // 有効なCC#32が一度も解決されていない間は、プログラムチェンジも
        // (渡すべきsoundfont_index/sf2_bankが無いため)読み捨てる。
        if (!w.bankResolved) return true;
        // F0 00 48 01 05 <chan> <soundfont_index> <bank_msb> <bank_lsb> <prog> F7
        uint8_t buf[11] = {
            0xF0, 0x00, 0x48, 0x01, 0x05,
            w.fluidsynthChan,
            static_cast<uint8_t>(w.soundfontIndex & 0x7F),
            static_cast<uint8_t>((w.sf2Bank >> 7) & 0x7F),
            static_cast<uint8_t>(w.sf2Bank & 0x7F),
            static_cast<uint8_t>(d1 & 0x7F),
            0xF7
        };
        sf2Port_->writeBurst(0, buf, sizeof(buf));
        w.lastProg = static_cast<int>(d1 & 0x7F); // MIDIモニター表示用(2026年8月新設)
        return true;
    }

    // Note On/Off・ピッチベンド等、それ以外の通常のチャンネルメッセージは
    // そのままfluidsynth chanへ転送する(RPN/NRPNのデータエントリー系CCも
    // 上のCC分岐でcc!=0/32のため既にここを通らず転送済み)。
    const uint8_t remappedStatus = static_cast<uint8_t>(type | w.fluidsynthChan);
    if (type == 0xD0) { // Channel Pressure (1データバイト)
        uint8_t buf[2] = { remappedStatus, d1 };
        sf2Port_->writeBurst(0, buf, sizeof(buf));
    } else {
        uint8_t buf[3] = { remappedStatus, d1, d2 };
        sf2Port_->writeBurst(0, buf, sizeof(buf));
        // MIDIモニターのFnumber列表示用(2026年8月新設): fluidsynthへ実際に
        // 送出したNote On(0x90、velocity>0)の生バイト列を保持する。
        // velocity=0のNote On(Note Off相当)や実際のNote Offでは更新しない
        // (「最後に送ったノートオンメッセージ」を表示する設計のため)。
        if (type == 0x90 && d2 > 0) {
            w.hasLastNoteOn    = true;
            w.lastNoteOnStatus = remappedStatus;
            w.lastNoteOnNote   = d1;
            w.lastNoteOnVel    = d2;
        }
    }
    return true;
}

void CFITOM::setSf2ChannelWindow(uint8_t mpu, uint8_t ch, uint8_t fluidsynthChanOr7F)
{
    if (mpu >= MAX_MPUS || ch >= 16) {
        FITOM_LOG_WARN("SF2 channel window: invalid mpu=" << (int)mpu << " ch=" << (int)ch);
        return;
    }

    if (fluidsynthChanOr7F == 0x7F) {
        sf2Windows_[mpu][ch] = Sf2WindowState{};
        FITOM_LOG_INFO("SF2 channel window: mpu=" << (int)mpu << " ch=" << (int)(ch + 1)
            << " released (back to native path)");
        return;
    }

    if (fluidsynthChanOr7F >= 16) {
        FITOM_LOG_WARN("SF2 channel window: invalid fluidsynth_chan=" << (int)fluidsynthChanOr7F);
        return;
    }

    // 同じfluidsynth_chanを複数の(mpu, ch)組へ重複して割り当てることは
    // できない(docs 8.2節)。重複する場合は要求そのものを無視する
    // (暗黙に既存の割り当てを解除するような副作用は起こさない)。
    for (int p = 0; p < MAX_MPUS; ++p) {
        for (int c = 0; c < 16; ++c) {
            if (p == mpu && c == ch) continue;
            if (sf2Windows_[p][c].assigned && sf2Windows_[p][c].fluidsynthChan == fluidsynthChanOr7F) {
                FITOM_LOG_WARN("SF2 channel window: fluidsynth_chan=" << (int)fluidsynthChanOr7F
                    << " is already assigned to mpu=" << p << " ch=" << (int)(c + 1)
                    << ", request ignored");
                return;
            }
        }
    }

    Sf2WindowState fresh;
    fresh.assigned       = true;
    fresh.fluidsynthChan = fluidsynthChanOr7F;
    sf2Windows_[mpu][ch] = fresh;
    FITOM_LOG_INFO("SF2 channel window: mpu=" << (int)mpu << " ch=" << (int)(ch + 1)
        << " -> fluidsynth_chan=" << (int)fluidsynthChanOr7F);
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
    CFITOM* parent, bool clockEnabled, int mpuIndex)
    : channels_(channels), parent_(parent), clockEnabled_(clockEnabled), mpuIndex_(mpuIndex)
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

    // SF2直行パス(docs/sf2-fluidsynth-integration.md参照): 窓に含まれる
    // (mpuIndex_, ch)は、CInstCh/CRhythmChへのディスパッチ・PatchManagerに
    // よる音色解決を一切経由せず、SF2ラッパープラグインへ生MIDIバイト列
    // として直接転送する。内部用MIDIパイプ(mpuIndex_==-1)は対象外。
    if (mpuIndex_ >= 0
        && parent_->routeSf2ChannelMessage(mpuIndex_, ch, status, msgBuf_[1], msgBuf_[2])) {
        return;
    }

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
    // パラメータ番号を選び直したら、保持中のデータエントリー値は破棄する
    // (CFITOM.h の RpnState コメント参照)。
    case 98:  rpn_[ch].reg = (rpn_[ch].reg & 0x3F80) | val; rpn_[ch].isNrpn = true;  selectRpnParam(ch); break;
    case 99:  rpn_[ch].reg = (rpn_[ch].reg & 0x007F) | (static_cast<uint16_t>(val) << 7); rpn_[ch].isNrpn = true; selectRpnParam(ch); break;
    case 100: rpn_[ch].reg = (rpn_[ch].reg & 0x3F80) | val; rpn_[ch].isNrpn = false; selectRpnParam(ch); break;
    case 101: rpn_[ch].reg = (rpn_[ch].reg & 0x007F) | (static_cast<uint16_t>(val) << 7); rpn_[ch].isNrpn = false; selectRpnParam(ch); break;
    case 6:   // Data Entry MSB
        rpn_[ch].msb = val;
        rpn_[ch].msbReceived = true;
        applyDataEntry(ch, midicch);
        break;
    case 38:  // Data Entry LSB
        rpn_[ch].lsb = val;
        // MSB受信済みのときのみ、MSBと合成した14bit値で「上書き」適用する。
        // これが無いと、CC#6→CC#38の通常の送信順でLSBが一切反映されない
        // (2026年8月修正。RPN#1チャンネルファインチューニングのように
        //  14bit全体を使うパラメータで、指定値と実際のピッチが食い違う
        //  原因になっていた)。
        applyDataEntry(ch, midicch);
        break;
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

void MidiProcessor::selectRpnParam(uint8_t ch)
{
    rpn_[ch].msb = 0;
    rpn_[ch].lsb = 0;
    rpn_[ch].msbReceived = false;
}

void MidiProcessor::applyDataEntry(uint8_t ch, IMidiCh* midicch)
{
    // RPN 7F/7F (RPN NULL): 明示的にRPN/NRPNの選択を解除する規格上の
    // 値。この状態でData Entryを受けても何も適用しない
    // (MIDI規格上の必須動作。現状はrpn_[ch].regの初期値も0x3FFFの
    //  ため、未選択状態と区別できるようにするための明示ガード)。
    if (rpn_[ch].reg == 0x3FFF) return;
    // MSB未受信(CC#38単独)では適用しない。
    if (!rpn_[ch].msbReceived) return;
    uint16_t data = (static_cast<uint16_t>(rpn_[ch].msb) << 7) | rpn_[ch].lsb;
    if (rpn_[ch].isNrpn) midicch->setNRPNRegister(rpn_[ch].reg, data);
    else                 midicch->setRPNRegister(rpn_[ch].reg, data);
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
// SysEx(private, manufacturer 00H 48H 01H)によるHwPatch/SwPatch
// パラメータオーバーライド。プロトコル:
//   [3]   sub-cmd (0x01=HwPatch / 0x02=SwPatch)
//   [4]   target-type (0x00=MIDIチャンネル / 0x01=プリセットバンク直接編集)
//   [5..] target-addr (sub-cmd・target-typeにより可変長、下記参照)
//   [layerOffset]   layer (対象ToneLayerインデックス。target-type=0x01では無視)
//   [jsonOffset..]  JSONペイロード(ASCII、オーバーライドしたい
//                   フィールドのみを持つオブジェクト)
//
// target-addr:
//   target-type=0x00(チャンネル、sub-cmd共通): [5]=MIDIチャンネル(0-15)  (1byte)
//   target-type=0x01(バンク) sub-cmd=0x01(HwPatch):
//     [5]=VoicePatchType [6]=HwBankインデックス [7]=HwProg番号            (3byte)
//   target-type=0x01(バンク) sub-cmd=0x02(SwPatch):
//     [5]=SwBankインデックス [6]=SwProg番号                              (2byte)
//     (SwBankはHwBankと異なりチップ族に依存しない単一の番号空間のため
//      VoicePatchTypeを指定する必要がない)
//
// sub-cmd=0x06(レイヤードパッチ)/0x07(ドラムキット)は、上記と異なり
// target-type/layerの枠組みを持たない(バンク直接編集専用、チャンネル
// スコープの一時上書きは既存の NRPN97/NRPN24-28 が担うため不要)。
//   [3]=sub-cmd  [4]=バンク番号  [5]=prog番号  [6..]=JSON
// 詳細はdocs/manuals/midi-message-reference.md 8.1節参照。
void MidiProcessor::processPrivateSysEx()
{
    if (sysexPt_ < 5) {
        FITOM_LOG_DEBUG("SysEx: private message too short (missing sub-cmd/target-type), ignored");
        return;
    }
    const uint8_t subCmd = sysexBuf_[3];

    // sub-cmd=0x04: SF2直行パス チャンネル窓の動的割り当て
    // (docs/manuals/midi-message-reference.md 8.2節、
    //  docs/sf2-fluidsynth-integration.md ⑤節参照)。
    //   F0 00 48 01 04 <mpu> <ch> <fluidsynth_chan|0x7F> F7
    if (subCmd == 0x04) {
        if (sysexPt_ < 7) {
            FITOM_LOG_DEBUG("SysEx: SF2 channel window message too short, ignored");
            return;
        }
        if (parent_) {
            parent_->setSf2ChannelWindow(sysexBuf_[4], sysexBuf_[5], sysexBuf_[6]);
        }
        return;
    }

    // sub-cmd=0x06: レイヤードパッチ(Patch)直接編集。パッチエディタが
    // バンクファイルへの保存と対にして送る、永続化用のバンク確定専用
    // メッセージ(docs/manuals/midi-message-reference.md 8.1節、
    // docs/plugin-midi-pipe.md「後片付け」節参照)。target-type/layerの
    // 枠組みは使わず(このPatch単位ではチャンネルスコープの一時上書きは
    // 既にNRPN97「ToneLayerオーバーライド」が担っているため、バンク直接
    // 編集の1系統のみでよい)、常にバンク直接編集として扱う。
    //   F0 00 48 01 06 <patchBank> <prog> <JSON> F7
    if (subCmd == 0x06) {
        if (sysexPt_ < 6) {
            FITOM_LOG_DEBUG("SysEx: Patch override message too short, ignored");
            return;
        }
        if (!parent_) return;
        const uint8_t patchBankNo = sysexBuf_[4];
        const uint8_t prog        = sysexBuf_[5];
        const std::string jsonText(reinterpret_cast<const char*>(&sysexBuf_[6]), sysexPt_ - 6);

        PatchManager& pm = parent_->getPatchManager();
        PatchBank* bank = pm.findMutablePatchBank(patchBankNo);
        if (!bank) {
            FITOM_LOG_WARN("SysEx: Patch override (bank) target not found: patchBank="
                << (int)patchBankNo);
            return;
        }
        if (prog >= BANK_PROG_SIZE) {
            FITOM_LOG_WARN("SysEx: Patch override (bank) prog out of range: " << (int)prog);
            return;
        }
        Patch& target = bank->patches[prog];
        if (!target.isValid()) {
            // HwPatch/SwPatchのバンク直接編集と同じ方針: 空きスロットから
            // 新規パッチを作る用途ではない(idを設定する手段がこの経路には
            // 無いため)。
            FITOM_LOG_WARN("SysEx: Patch override (bank) target slot is empty (patchBank="
                << (int)patchBankNo << " prog=" << (int)prog << "), ignored");
            return;
        }
        std::string err;
        if (!pm.mergePatchFromJsonText(jsonText, target, &err)) {
            FITOM_LOG_WARN("SysEx: Patch override (bank) JSON parse failed: " << err);
        }
        return;
    }

    // sub-cmd=0x07: ドラムキット(DrumPatch)直接編集。0x06と同じ性質
    // (バンク確定専用、ディスク保存はしない)。ノート単位の微調整も
    // キット全体の一括反映も、JSON側の"notes"(ノート番号キーのオブジェクト)
    // で同じメッセージ形式に収める(docs/manuals/midi-message-reference.md
    // 8.1節参照)。
    //   F0 00 48 01 07 <drumBank> <prog> <JSON> F7
    if (subCmd == 0x07) {
        if (sysexPt_ < 6) {
            FITOM_LOG_DEBUG("SysEx: DrumPatch override message too short, ignored");
            return;
        }
        if (!parent_) return;
        const uint8_t drumBankNo = sysexBuf_[4];
        const uint8_t prog       = sysexBuf_[5];
        const std::string jsonText(reinterpret_cast<const char*>(&sysexBuf_[6]), sysexPt_ - 6);

        PatchManager& pm = parent_->getPatchManager();
        DrumPatchBank* bank = pm.drumRegistry().findMutable(drumBankNo);
        if (!bank) {
            FITOM_LOG_WARN("SysEx: DrumPatch override (bank) target not found: drumBank="
                << (int)drumBankNo);
            return;
        }
        if (prog >= BANK_PROG_SIZE) {
            FITOM_LOG_WARN("SysEx: DrumPatch override (bank) prog out of range: " << (int)prog);
            return;
        }
        DrumPatch& target = bank->patches[prog];
        if (!target.isValid()) {
            FITOM_LOG_WARN("SysEx: DrumPatch override (bank) target slot is empty (drumBank="
                << (int)drumBankNo << " prog=" << (int)prog << "), ignored");
            return;
        }
        std::string err;
        if (!pm.mergeDrumPatchFromJsonText(jsonText, target, &err)) {
            FITOM_LOG_WARN("SysEx: DrumPatch override (bank) JSON parse failed: " << err);
        }
        return;
    }

    if (subCmd != 0x01 && subCmd != 0x02) {
        FITOM_LOG_DEBUG("SysEx: private sub-cmd=0x" << std::hex << (int)subCmd
            << std::dec << " unhandled");
        return;
    }
    const bool isHw = (subCmd == 0x01); // false = SwPatch(sub-cmd 0x02)

    const uint8_t targetType = sysexBuf_[4];
    size_t addrLen;
    if (targetType == 0x00) {
        addrLen = 1; // ch (HwPatch/SwPatch共通)
    } else if (targetType == 0x01) {
        addrLen = isHw ? 3 : 2; // HwPatch: voicePatchType+hwBank+hwProg / SwPatch: swBank+swProg
    } else {
        FITOM_LOG_DEBUG("SysEx: patch override target-type=0x" << std::hex << (int)targetType
            << std::dec << " unhandled");
        return;
    }

    const size_t layerOffset = 5 + addrLen;
    if (sysexPt_ < layerOffset + 1) {
        FITOM_LOG_DEBUG("SysEx: patch override message too short (missing layer byte)");
        return;
    }
    const uint8_t layer = sysexBuf_[layerOffset];
    const size_t jsonOffset = layerOffset + 1;
    const std::string jsonText(reinterpret_cast<const char*>(&sysexBuf_[jsonOffset]),
                                sysexPt_ - jsonOffset);

    if (targetType == 0x00) {
        const uint8_t ch = sysexBuf_[5];
        if (ch >= 16) {
            FITOM_LOG_WARN("SysEx: patch override invalid channel=" << (int)ch);
            return;
        }
        IMidiCh* midicch = channels_[ch].get();
        if (!midicch) return;
        const bool ok = isHw ? midicch->mergeHwPatchOverride(layer, jsonText)
                              : midicch->mergeSwPatchOverride(layer, jsonText);
        if (!ok) {
            FITOM_LOG_WARN("SysEx: " << (isHw ? "HwPatch" : "SwPatch")
                << " override (channel) failed ch=" << (int)ch << " layer=" << (int)layer);
        }
        return;
    }

    // targetType == 0x01: プリセットバンク直接編集
    if (!parent_) return;
    PatchManager& pm = parent_->getPatchManager();

    if (isHw) {
        const uint8_t voicePatchType = sysexBuf_[5];
        const uint8_t hwBank         = sysexBuf_[6];
        const uint8_t hwProg         = sysexBuf_[7];

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
        return;
    }

    // sub-cmd=0x02(SwPatch)、targetType=0x01(バンク直接編集)
    const uint8_t swBank = sysexBuf_[5];
    const uint8_t swProg = sysexBuf_[6];

    SwBank* bank = pm.swRegistry().findMutable(swBank);
    if (!bank) {
        FITOM_LOG_WARN("SysEx: SwPatch override (bank) target not found: swBank=" << (int)swBank);
        return;
    }
    if (swProg >= BANK_PROG_SIZE) {
        FITOM_LOG_WARN("SysEx: SwPatch override (bank) swProg out of range: " << (int)swProg);
        return;
    }
    SwPatch& target = bank->patches[swProg];
    if (!target.isValid()) {
        FITOM_LOG_WARN("SysEx: SwPatch override (bank) target slot is empty (swBank="
            << (int)swBank << " swProg=" << (int)swProg << "), ignored");
        return;
    }
    std::string err;
    if (!pm.mergeSwPatchFromJsonText(jsonText, target, &err)) {
        FITOM_LOG_WARN("SysEx: SwPatch override (bank) JSON parse failed: " << err);
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
