// fitom/gui/bridge/FITOMBridge.cpp
// GUI ↔ FITOM コア ブリッジ実装(UIフレームワーク非依存)

#include "FITOMBridge.h"
#include "fitom/CFITOM.h"
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/Log.h"
#include "fitom/MidiManager.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdio>
#include <iterator>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <climits>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <climits>
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

// FITOMBridge::MidiState: MIDI入力の実体(PIMPL、詳細はFITOMBridge.hの
// 宣言コメント参照)。
struct FITOMBridge::MidiState {
    std::shared_ptr<fitom::MidiPluginInstance> plugin;
    std::vector<std::unique_ptr<fitom::MidiInPort>> ports;

    // 内部用MIDIパイプ専用(2026年7月新設)。プロファイルのmidi_backend/
    // midi_inputsとは無関係な、独立したライフサイクルを持つため上記の
    // plugin/portsとは別メンバにする(reopenMidiPorts()がplugin/portsを
    // クリアしても、これは影響を受けない)。
    std::shared_ptr<fitom::MidiPluginInstance> internalPipePlugin;
    std::unique_ptr<fitom::MidiInPort>          internalPipePort;
};

FITOMBridge::FITOMBridge() : midiState_(std::make_unique<MidiState>()) {}
FITOMBridge::~FITOMBridge() = default;

namespace {

// 実行ファイル自身のディレクトリを取得する(MIDIバックエンドDLLの
// 相対パス解決に使う。apps/fitom_cli・apps/fitom_guiの同名ヘルパーと
// 同じ実装)。取得できなければカレントディレクトリを返す。
fs::path exeDir()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#elif defined(__APPLE__)
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

// MIDIバックエンドDLLのパスを解決する(apps/fitom_cliのresolveMidiBackendPath
// と同じ規則: プロファイル側で明示指定されていればそれを exeDir() 基準の
// 相対パスとして解決、省略時はプラットフォームごとの既定ファイル名)。
fs::path resolveMidiBackendPath(const std::string& configured)
{
    if (!configured.empty()) {
        fs::path p = configured;
        return p.is_relative() ? (exeDir() / p) : p;
    }
#if defined(_WIN32)
    return exeDir() / "fitom_midi_rtmidi.dll";
#elif defined(__APPLE__)
    return exeDir() / "fitom_midi_rtmidi.dylib";
#elif defined(__linux__)
    return exeDir() / "fitom_midi_rtmidi.so";
#else
    return exeDir() / "fitom_midi_backend";
#endif
}

// 内部用MIDIパイプ(fitom_midi_pipe)専用のDLLパスを解決する。
// resolveMidiBackendPath()と異なり、プロファイルの設定(midi_backend.dll)を
// 一切参照しない。ファイル名も固定(有効/無効はFITOM_BUILD_BACKEND_MIDI_PIPE
// というビルド設定で切り替える設計のため、パス自体を可変にする理由がない)。
fs::path resolveInternalPipeBackendPath()
{
#if defined(_WIN32)
    return exeDir() / "fitom_midi_pipe.dll";
#elif defined(__APPLE__)
    return exeDir() / "fitom_midi_pipe.dylib";
#elif defined(__linux__)
    return exeDir() / "fitom_midi_pipe.so";
#else
    return exeDir() / "fitom_midi_pipe";
#endif
}

// backends/midi_pipe/src/MidiPipe.cppのkDeviceNameと完全一致させる必要がある。
constexpr const char* kInternalPipeDeviceName = "FITOM Internal Pipe";

// MIDI入力のオープン失敗時、原因調査用にバックエンドが実際に列挙する
// ポート名一覧をログへ出す(findPortByNameは完全一致のみで検索するため、
// profile側の指定名がずれていないか/デバイスが接続されているかを
// その場で確認できるようにする)。
void logAvailableMidiInPorts(const fitom::MidiPluginInstance& plugin)
{
    auto names = plugin.enumerateIn();
    if (names.empty()) {
        FITOM_LOG_WARN("FITOMBridge: 利用可能なMIDI入力ポートがありません");
        return;
    }
    std::string list;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) list += ", ";
        list += "\"" + names[i] + "\"";
    }
    FITOM_LOG_WARN("FITOMBridge: 利用可能なMIDI入力ポート: " << list);
}

// ─── 内蔵リズム音源(VOICE_PATCH_BUILTIN_RHYTHM、CC#0=0x70)専用 ──────────
// PatchManager::resolveBuiltinRhythm()と同じ規約: hwBank相当値として
// 対象チップのVoicePatchTypeをそのまま使う(バンク番号ではない、
// 通常のHwBankRegistry/SampleZoneBankRegistryを一切経由しないため
// JSON設定によるバンク一覧が存在しない。2026年7月新設、パッチピッカー
// でAWM同様に表示されない不具合の指摘を受けて追加)。
struct BuiltinRhythmChip {
    uint8_t     chipSel; // hwBank相当値 (resolveBuiltinRhythmのchipSel引数)
    const char* label;
};
constexpr BuiltinRhythmChip kBuiltinRhythmChips[] = {
    { VOICE_PATCH_OPN2, "OPNA" },
    { VOICE_PATCH_OPLL, "OPLL" },
};

// 各チップの内蔵リズムパート名(実機固定、hwProg=パート番号=デバイス
// チャンネル番号の順序に対応。OPN2_new.cpp::COPNARhythm/OPLL_new.cpp::
// COPLLRhythmのレジスタ割り当てコメントと同じ並び)。
constexpr const char* kOpnaRhythmNames[] = {
    "Bass Drum", "Snare Drum", "Top Cymbal", "Hi-Hat", "Tom", "Rim Shot"
};
constexpr const char* kOpllRhythmNames[] = {
    "Hi-Hat", "Top Cymbal", "Tom", "Snare Drum", "Bass Drum"
};

// chipSel(VOICE_PATCH_OPN2/VOICE_PATCH_OPLL)から、対応する内蔵リズム
// パート名テーブルとその要素数を引く。該当なしならnullptr/0を返す。
void getBuiltinRhythmNames(uint8_t chipSel, const char* const*& names, size_t& count)
{
    if (chipSel == VOICE_PATCH_OPN2) {
        names = kOpnaRhythmNames;
        count = std::size(kOpnaRhythmNames);
    } else if (chipSel == VOICE_PATCH_OPLL) {
        names = kOpllRhythmNames;
        count = std::size(kOpllRhythmNames);
    } else {
        names = nullptr;
        count = 0;
    }
}

} // namespace

FITOMBridge& FITOMBridge::instance() {
    static FITOMBridge inst;
    return inst;
}

// ================================================================
//  初期化
// ================================================================

bool FITOMBridge::init(const std::string& systemConfPath,
                        const std::string& profilePath)
{
    if (initialized_) return true;

    // システム設定 (fitom.conf.json) を先に読み込み、その log.* 設定で
    // Boost.Log を初期化する (省略時は従来通りの既定値 "info"を使う)。
    auto config   = std::make_unique<fitom::FITOMConfig>();
    auto patchMgr = std::make_unique<fitom::PatchManager>();

    if (!systemConfPath.empty()) {
        config->loadSystemConf(fs::path(systemConfPath));
    }
    fitom::Log::init(config->getLogLevel("info"),
                      config->getLogFile(""),
                      config->getLogConsole(true));

    FITOM_LOG_INFO("FITOMBridge initializing...");
    if (!profilePath.empty()) {
        config->loadProfile(fs::path(profilePath), patchMgr.get());
        currentProfile_ = profilePath;
    }

    // タイマースレッドの起動はinit()の呼び出し元(apps/fitom_gui等)が
    // 初期化成功後にstartTimerThread()を呼んで行う(2026年7月訂正。
    // 以前はここに「MFCタイマーからonTimer()を呼ぶため不要」という
    // コメントがあったが、現行のfitom_gui(Dear ImGui)はMFCを使っておらず
    // 実態と食い違っていた。詳細はFITOMBridge.hのonTimer()コメント参照)。
    // CFITOM を初期化
    int ret = fitom::CFITOM::instance().init(
        std::move(config), std::move(patchMgr));
    if (ret != 0) {
        FITOM_LOG_ERR("FITOMBridge: CFITOM init failed (" << ret << ")");
        return false;
    }

    // ステータスコールバックを転送
    fitom::CFITOM::instance().setStatusCallback([this](const std::string& msg) {
        if (statusCb_) statusCb_(msg);
    });

    // MIDI入力を開く(apps/fitom_cliのmain.cppと同じ手順: バックエンド
    // DLLをロードし、プロファイルのmidi_inputs[]で名前指定された各ポートを
    // 開いて、対応するMPU(MidiProcessor)のreceiveByte()へ配線する)。
    // 個別のポートのオープン失敗はログのみ出し、致命的エラーとしては
    // 扱わない(他のポートは引き続き使えるようにするため)。実処理は
    // reopenMidiPorts()(GUIのMIDIポート設定ダイアログからの再設定とも
    // 共通、2026年7月新設)に切り出した。
    reopenMidiPorts();
    initInternalMidiPipe();

    initialized_ = true;
    FITOM_LOG_INFO("FITOMBridge initialized");
    return true;
}

// 内部用MIDIパイプ(fitom_midi_pipe)を、プロファイルのmidi_backend/
// midi_inputs設定とは無関係に無条件でオープンする(2026年7月新設)。
// 詳細はFITOMBridge.hの宣言コメント参照。
void FITOMBridge::initInternalMidiPipe()
{
    const fs::path pipeDllPath = resolveInternalPipeBackendPath();
    if (!fs::exists(pipeDllPath)) {
        // FITOM_BUILD_BACKEND_MIDI_PIPE=OFF(既定)でビルドした場合の通常
        // 経路。エラーではないためINFOレベルにとどめる。
        FITOM_LOG_INFO("FITOMBridge: 内部用MIDIパイプDLL未検出のためスキップ ("
            << pipeDllPath.string() << ")");
        return;
    }

    auto* proc = fitom::CFITOM::instance().getInternalPipeProcessor();
    if (!proc) return; // CFITOM::init()が必ず生成するため通常起こりえない

    try {
        midiState_->internalPipePlugin = fitom::MidiPluginInstance::load(pipeDllPath);
        midiState_->internalPipePort = std::make_unique<fitom::MidiInPort>(
            midiState_->internalPipePlugin, kInternalPipeDeviceName,
            [](const uint8_t* data, size_t len, uint64_t ts) {
                // receiveByte()を直接呼ばずCFITOM::receiveInternalPipeByte()
                // 経由にする(processMutex_でロックしてタイマースレッドとの
                // データ競合を防ぐため。詳細はCFITOM.hのコメント参照)。
                fitom::CFITOM::instance().receiveInternalPipeByte(data, len, ts);
            });
        FITOM_LOG_INFO("FITOMBridge: 内部用MIDIパイプを有効化しました");
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("FITOMBridge: 内部用MIDIパイプのオープンに失敗: " << e.what());
        midiState_->internalPipePort.reset();
        midiState_->internalPipePlugin.reset();
    }
}

// Config側の現在のMIDI入力ポート設定(getMidiInputCount()/
// getMidiInputName())に基づき、既存ポートを全て閉じてから開き直す
// (init()とsetMidiInputPorts()の共通処理、2026年7月新設)。
// 設定名が1件も無い場合はバックエンドDLL自体をロードしない
// (init()の従来挙動を維持。オーディオデバイスと違い、MIDIバックエンドの
// 読み込み自体に副作用は無いが、環境によってはDLLが存在しない場合もある
// ため、不要な読み込みは避ける)。
void FITOMBridge::reopenMidiPorts()
{
    auto& fitomInst = fitom::CFITOM::instance();
    auto& cfg = fitomInst.getConfig();

    // 既存ポートを先に閉じる(同名ポートの二重オープンを防ぐため)。
    midiState_->ports.clear();

    const int midiInCount = cfg.getMidiInputCount();
    bool anyPortConfigured = false;
    for (int i = 0; i < midiInCount; ++i) {
        if (!cfg.getMidiInputName(i).empty()) { anyPortConfigured = true; break; }
    }
    if (!anyPortConfigured) {
        midiState_->plugin.reset();
        return;
    }

    if (!midiState_->plugin) {
        const fs::path midiBackendPath = resolveMidiBackendPath(cfg.getMidiBackendDll());
        try {
            midiState_->plugin = fitom::MidiPluginInstance::load(midiBackendPath);
        } catch (const std::exception& e) {
            FITOM_LOG_ERR("FITOMBridge: MIDIバックエンド読み込み失敗 ("
                << midiBackendPath.string() << "): " << e.what());
            return;
        }
    }

    for (int i = 0; i < fitom::CFITOM::getMpuCount(); ++i) {
        fitom::MidiProcessor* proc = fitomInst.getMidiProcessor(static_cast<uint8_t>(i));
        const std::string portName = (i < midiInCount) ? cfg.getMidiInputName(i) : std::string();
        if (!proc || portName.empty()) {
            midiState_->ports.push_back(nullptr);
            continue;
        }
        const uint8_t mpuIndex = static_cast<uint8_t>(i);
        try {
            midiState_->ports.push_back(std::make_unique<fitom::MidiInPort>(
                midiState_->plugin, portName,
                [mpuIndex](const uint8_t* data, size_t len, uint64_t ts) {
                    // receiveByte()を直接呼ばずCFITOM::receiveMpuByte()経由に
                    // する(processMutex_でロックしてタイマースレッドとの
                    // データ競合を防ぐため。詳細はCFITOM.hのコメント参照)。
                    fitom::CFITOM::instance().receiveMpuByte(mpuIndex, data, len, ts);
                }));
        } catch (const std::exception& e) {
            FITOM_LOG_ERR("FITOMBridge: MIDI入力 \"" << portName
                << "\" のオープンに失敗: " << e.what());
            logAvailableMidiInPorts(*midiState_->plugin);
            midiState_->ports.push_back(nullptr);
        }
    }
}

void FITOMBridge::exit() {
    if (!initialized_) return;
    // MIDI入力を先に閉じる(コールバックが半分シャットダウン済みの
    // CFITOMへ飛び込まないようにするため。apps/fitom_cliと同じ順序)。
    midiState_->ports.clear();
    midiState_->plugin.reset();
    midiState_->internalPipePort.reset();
    midiState_->internalPipePlugin.reset();
    fitom::CFITOM::instance().exit();
    initialized_ = false;
    FITOM_LOG_INFO("FITOMBridge exited");
}

// ================================================================
//  プロファイル切り替え
// ================================================================

bool FITOMBridge::loadProfile(const std::string& path)
{
    auto& fitom = fitom::CFITOM::instance();
    fitom.allNoteOff();

    auto& cfg = fitom.getConfig();
    bool ok = cfg.loadProfile(fs::path(path), &fitom.getPatchManager());
    if (ok) {
        currentProfile_ = path;
        FITOM_LOG_INFO("Profile loaded: " << path);
    }
    return ok;
}

std::string FITOMBridge::currentProfilePath() const {
    return currentProfile_;
}

bool FITOMBridge::saveCurrentProfile()
{
    if (!initialized_ || currentProfile_.empty()) return false;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    bool ok = cfg.saveProfile(fs::path(currentProfile_));
    if (ok) {
        FITOM_LOG_INFO("Profile saved: " << currentProfile_);
    } else {
        FITOM_LOG_ERR("FITOMBridge: プロファイルの保存に失敗: " << currentProfile_);
    }
    return ok;
}

// ================================================================
//  デバイス情報
// ================================================================

std::vector<FITOMDeviceInfo> FITOMBridge::getDevices() const
{
    std::vector<FITOMDeviceInfo> result;
    if (!initialized_) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    int n = cfg.getDeviceCount();
    for (int i = 0; i < n; ++i) {
        FITOMDeviceInfo info;
        info.index       = i;
        info.label       = cfg.getDeviceLabel(i);
        info.deviceType  = cfg.getDeviceType(i);
        // 実機/エミュレータの区別は、HWプラグイン(IHWPlugin)経由に
        // 一元化された現行アーキテクチャでは、この階層からは判定でき
        // ない(どちらであるかはロードされたHWプラグインDLLの内部
        // 実装に委ねられている)。旧`FmEnginePort`型による判定は
        // 同型が廃止されたため撤去した。判定手段が必要になった場合は
        // IHWPlugin側にケイパビリティ問い合わせAPIを追加検討する。
        info.isEmulator  = false;

        auto* dev = cfg.getDevice(i);
        if (dev) {
            info.descriptor = dev->getDescriptor();
            info.chCount    = dev->getChCount();
        } else {
            info.descriptor = "N/A";
            info.chCount    = 0;
        }
        result.push_back(info);
    }
    return result;
}

std::vector<FITOMChipInfo> FITOMBridge::getHwChips() const
{
    std::vector<FITOMChipInfo> result;
    if (!initialized_) return result;

    auto& core = fitom::CFITOM::instance();
    int n = core.getPhysicalChipCount();
    for (int i = 0; i < n; ++i) {
        const auto* info = core.getPhysicalChipInfo(i);
        if (!info) continue;
        FITOMChipInfo c;
        c.index        = i;
        c.label        = info->label;
        c.physicalName = info->physicalName;
        result.push_back(c);
    }
    return result;
}

std::vector<uint8_t> FITOMBridge::getHwChipRegisterDump(int chipIndex) const
{
    if (!initialized_) return {};
    return fitom::CFITOM::instance().getPhysicalChipRegisterDump(chipIndex);
}

std::vector<FITOMLevelBand> FITOMBridge::getPhysicalLevelBands() const
{
    std::vector<FITOMLevelBand> result;
    if (!initialized_) return result;

    auto& core = fitom::CFITOM::instance();
    int n = core.getPhysicalChipCount();
    for (int i = 0; i < n; ++i) {
        const auto* info = core.getPhysicalChipInfo(i);
        // subDevicesが空(SF2直行パス用ラッパー等、論理デバイスを持たない
        // エントリ)は内訳を取得できないため対象外。
        if (!info || info->subDevices.empty()) continue;

        // 物理チップ単位のビューなので、リニアステレオ化ペアもL側・R側を
        // それぞれ独立したバンドとして並べる(ラベルの「(stereo pair)」で
        // 区別できる。右ペインのレジスタダンプ一覧とも一対一で対応する)。
        FITOMLevelBand band;
        band.label = info->physicalName.empty() ? info->label : info->physicalName;

        auto states = core.getPhysicalChipChannelStates(i);
        size_t stateIdx = 0;
        for (const auto& sub : info->subDevices) {
            std::string prefix = fitom::CFITOM::getSubDeviceChannelPrefix(sub.deviceType);
            for (uint8_t ch = 0; ch < sub.chCount && stateIdx < states.size(); ++ch, ++stateIdx) {
                FITOMLevelChannel c;
                c.name     = prefix + std::to_string(static_cast<int>(ch) + 1);
                c.sounding  = states[stateIdx].sounding;
                c.velocity  = states[stateIdx].velocity;
                c.enabled   = states[stateIdx].enabled;
                c.noteOnSeq = states[stateIdx].noteOnSeq;
                c.stereo    = states[stateIdx].stereo;
                c.gainL     = states[stateIdx].gainL;
                c.gainR     = states[stateIdx].gainR;
                band.channels.push_back(std::move(c));
            }
        }
        result.push_back(std::move(band));
    }
    return result;
}

std::vector<FITOMLevelBand> FITOMBridge::getLogicalLevelBands() const
{
    std::vector<FITOMLevelBand> result;
    if (!initialized_) return result;

    auto& core = fitom::CFITOM::instance();
    int n = core.getDeviceCount();
    for (int i = 0; i < n; ++i) {
        auto* dev = core.getDevice(i);
        if (!dev) continue;
        auto states = core.getLogicalDeviceChannelStates(i);
        if (states.empty()) continue;

        FITOMLevelBand band;
        band.label = core.getConfig().getDeviceLabel(i);

        std::string prefix = fitom::CFITOM::getSubDeviceChannelPrefix(dev->getDeviceType());
        for (size_t ch = 0; ch < states.size(); ++ch) {
            FITOMLevelChannel c;
            c.name     = prefix + std::to_string(static_cast<int>(ch) + 1);
            c.sounding  = states[ch].sounding;
            c.velocity  = states[ch].velocity;
            c.enabled   = states[ch].enabled;
            c.noteOnSeq = states[ch].noteOnSeq;
            c.stereo    = states[ch].stereo;
            c.gainL     = states[ch].gainL;
            c.gainR     = states[ch].gainR;
            band.channels.push_back(std::move(c));
        }
        result.push_back(std::move(band));
    }
    return result;
}

std::vector<FITOMMidiInfo> FITOMBridge::getMidiInputs() const
{
    std::vector<FITOMMidiInfo> result;
    if (!initialized_) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    const int midiInCount = cfg.getMidiInputCount();
    // MPU(getMpuCount())分を常に返す。未設定のMPUはnameが空文字列になる
    // (2026年7月、MIDIポート設定ダイアログでMPUごとの割り当てを一覧
    // できるようにするため。以前はcfg.getMidiInputCount()件のみ返して
    // いた)。
    for (int i = 0; i < fitom::CFITOM::getMpuCount(); ++i) {
        FITOMMidiInfo info;
        info.index = i;
        info.name  = (i < midiInCount) ? cfg.getMidiInputName(i) : std::string();
        result.push_back(info);
    }
    return result;
}

std::vector<std::string> FITOMBridge::getAvailableMidiInputPorts()
{
    if (!initialized_) return {};

    if (!midiState_->plugin) {
        auto& cfg = fitom::CFITOM::instance().getConfig();
        const fs::path midiBackendPath = resolveMidiBackendPath(cfg.getMidiBackendDll());
        try {
            midiState_->plugin = fitom::MidiPluginInstance::load(midiBackendPath);
        } catch (const std::exception& e) {
            FITOM_LOG_ERR("FITOMBridge: MIDIバックエンド読み込み失敗 ("
                << midiBackendPath.string() << "): " << e.what());
            return {};
        }
    }
    return midiState_->plugin->enumerateIn();
}

std::vector<std::string> FITOMBridge::getMidiInputPortAssignments() const
{
    std::vector<std::string> result;
    if (!initialized_) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    const int midiInCount = cfg.getMidiInputCount();
    for (int i = 0; i < fitom::CFITOM::getMpuCount(); ++i) {
        result.push_back((i < midiInCount) ? cfg.getMidiInputName(i) : std::string());
    }
    return result;
}

void FITOMBridge::setMidiInputPorts(const std::vector<std::string>& names)
{
    if (!initialized_) return;

    fitom::CFITOM::instance().getConfig().setMidiInputNames(names);
    reopenMidiPorts();
}

// ================================================================
//  MIDIモニター
// ================================================================

namespace {

// MIDIノート番号("C4"等)への変換。中央ハ(ノート60)を"C4"とする
// (Roland/GM系の慣例。Yamaha系の"C3"表記とは1オクターブずれる場合が
// あるが、GUI表示用の一貫した基準として採用する)。
std::string midiNoteName(uint8_t note)
{
    static const char* names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = static_cast<int>(note) / 12 - 1;
    int idx    = static_cast<int>(note) % 12;
    return std::string(names[idx]) + std::to_string(octave);
}

} // namespace

int FITOMBridge::getMpuCount() const
{
    return fitom::CFITOM::getMpuCount();
}

std::vector<FITOMChannelMonitor> FITOMBridge::getChannelMonitors(int mpuIndex) const
{
    std::vector<FITOMChannelMonitor> result;
    if (!initialized_) return result;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return result;

    auto& fitomInst = fitom::CFITOM::instance();
    auto* processor = fitomInst.getMidiProcessor(static_cast<uint8_t>(mpuIndex));
    if (!processor) return result;

    auto& pm = fitomInst.getPatchManager();

    for (int ch = 0; ch < 16; ++ch) {
        fitom::IMidiCh* midich = processor->getChannel(static_cast<uint8_t>(ch));
        FITOMChannelMonitor mon;
        mon.ch = ch;

        // SF2直行パス(docs/sf2-fluidsynth-integration.md参照、2026年8月新設):
        // 窓に含まれる(mpu,ch)はMidiProcessor::processMessage()がCInstCh/
        // CRhythmChへのディスパッチ自体を行わないため、以下のmidich由来の
        // 解決(bankNo/progNo/deviceName等)はいずれも意味を持たない。
        // CFITOM::sf2Windows_の表示専用キャッシュから、Bank/Prog列には
        // ファイル名+phdrで解決したプリセット名を、Fnumber列には実際に
        // fluidsynthへ送出したNote Onの生バイト列を組み立てて差し替える。
        const auto sf2 = fitomInst.getSf2ChannelWindowInfo(
            static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch));
        if (sf2.assigned) {
            mon.isSf2Windowed     = true;
            mon.sf2FluidsynthChan = sf2.fluidsynthChan;

            if (sf2.bankResolved) {
                const auto& reg   = fitomInst.getConfig().getSf2BankRegistry();
                const auto& files = reg.soundfontFiles();
                if (sf2.soundfontIndex >= 0
                    && static_cast<size_t>(sf2.soundfontIndex) < files.size()) {
                    mon.bankName = fs::path(files[sf2.soundfontIndex]).filename().string();
                }
                mon.bankNo = sf2.sf2Bank;
                if (sf2.lastProg >= 0) {
                    mon.progNo = sf2.lastProg;
                    std::string presetName;
                    if (reg.resolvePresetNameByIndex(sf2.soundfontIndex, sf2.sf2Bank,
                            static_cast<uint8_t>(sf2.lastProg), presetName)) {
                        mon.progName = presetName;
                    }
                }
            }

            mon.deviceName  = "SF2 fluidsynth ch" + std::to_string(static_cast<int>(sf2.fluidsynthChan));
            mon.deviceIndex = -1;

            if (sf2.hasLastNoteOn) {
                mon.sounding = true;
                mon.lastNote = sf2.lastNoteOnNote;
                mon.velocity = sf2.lastNoteOnVel;
                mon.noteName = midiNoteName(mon.lastNote);
                char hexBuf[16];
                std::snprintf(hexBuf, sizeof(hexBuf), "%02X %02X %02X",
                    sf2.lastNoteOnStatus, sf2.lastNoteOnNote, sf2.lastNoteOnVel);
                mon.sf2NoteOnHex = hexBuf;
            }

            result.push_back(mon);
            continue;
        }

        if (!midich) { result.push_back(mon); continue; }

        mon.isRhythm = midich->isRhythm();
        mon.bankNo   = midich->getBankNo();
        mon.progNo   = midich->getProgramNo();
        mon.volume   = midich->getVolume();
        mon.expression = midich->getExpression();
        mon.panpot   = midich->getPanpot();

        // バンク名・パッチ名の解決。レイヤードモード(PatchBank)・
        // 直接デバイス選択モード(HwBankRegistry)・リズムチャンネル
        // (DrumPatchBank)とで参照先が異なる。解決できない場合は空文字の
        // ままにし、GUI側で数値表示にフォールバックする。
        const uint8_t bankSelMSB = midich->getBankSelMSB();
        if (mon.isRhythm) {
            const auto* dp = pm.drumRegistry().resolve(mon.bankNo, mon.progNo);
            if (dp) mon.progName = dp->name;
        } else if (bankSelMSB == 0) {
            const auto* bank = pm.findPatchBank(mon.bankNo);
            if (bank) {
                mon.bankName = bank->name;
                const auto& p = bank->get(mon.progNo);
                if (p.isValid()) mon.progName = p.name;
            }
        } else if (bankSelMSB <= 0x6F) {
            // 直接デバイス選択モード(2026年7月修正: 以前はここが未対応で
            // 常に数値フォールバック表示になっていた)。サンプルベース
            // 音源系(ADPCM-B/ADPCM-A/PCMD8/AWM)はHwBankRegistryではなく
            // SampleZoneBankRegistryを参照する(getHwBankList()等と同じ理由、
            // 2026年7月追加修正)。
            if (isSampleBasedVoicePatchType(bankSelMSB)) {
                const auto* sampleBank = pm.sampleRegistry().find(bankSelMSB, mon.bankNo);
                if (sampleBank) {
                    mon.bankName = sampleBank->name;
                    const auto& sp = sampleBank->get(mon.progNo);
                    if (sp.isValid()) mon.progName = sp.name;
                }
            } else if (mon.bankNo == 0 && pm.getOpllRomPatches(bankSelMSB) != nullptr) {
                // OPLL系ROM音色(バンク0固定、getHwBankList()/getHwBankPatches()
                // と同じ理由、2026年7月追加修正)。JSON定義のバンクを一切
                // 経由しないため、HwBankRegistryには現れない。
                // 【注意】実際の解決(resolveOpllRomVoice)はCC#0の値
                // (bankSelMSB)ではなくhwProg(mon.progNo)自身の上位3bitで
                // チップ種別を決定するため、名前解決もgetOpllRomPatchByProg()
                // でmon.progNoから直接行う(getOpllRomPatches(bankSelMSB)は
                // ここでは「OPLL系バンク0か」の判定にのみ使う)。
                mon.bankName = "ROM Preset";
                if (mon.progNo >= 0 && mon.progNo <= 0xFF) {
                    const fitom::HwPatch* p = pm.getOpllRomPatchByProg(static_cast<uint8_t>(mon.progNo));
                    if (p) mon.progName = p->name;
                }
            } else {
                const auto group = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(bankSelMSB);
                const auto* hwBank = pm.hwRegistry().find(group, mon.bankNo);
                if (hwBank) {
                    mon.bankName = hwBank->name;
                    const auto& hp = hwBank->get(mon.progNo);
                    if (hp.isValid()) mon.progName = hp.name;
                }
            }
        } else if (bankSelMSB == VOICE_PATCH_BUILTIN_RHYTHM) {
            // 内蔵リズム音源(2026年7月追加修正、getHwBankList()/
            // getHwBankPatches()と同じ理由)。mon.bankNoがchipSel
            // (VOICE_PATCH_OPN2/VOICE_PATCH_OPLL)、mon.progNoが
            // パート番号(=デバイスチャンネル番号)を表す。
            for (const auto& c : kBuiltinRhythmChips) {
                if (c.chipSel == mon.bankNo) { mon.bankName = c.label; break; }
            }
            const char* const* names = nullptr;
            size_t count = 0;
            getBuiltinRhythmNames(static_cast<uint8_t>(mon.bankNo), names, count);
            if (names && mon.progNo >= 0 && static_cast<size_t>(mon.progNo) < count) {
                mon.progName = names[mon.progNo];
            }
        }

        // 発音状態: このチャンネルが最後に使ったデバイス/物理chの
        // ChStateを見て、現在も鳴っているか(Running/Releasing)を判定する。
        // 複数レイヤーがある場合も、直近に使われた1系統のみを表示する
        // (「後発優先」の方針をここでも踏襲する)。
        uint8_t devIdx = midich->getLastDeviceIndex();
        uint8_t devCh  = midich->getLastDevCh();
        mon.deviceIndex = (devIdx == 0xFF) ? -1 : static_cast<int>(devIdx);
        if (devIdx != 0xFF && devCh != 0xFF) {
            // ISoundDeviceの実体はCFITOMが所有する(Config::devices_[i].device
            // は生成されるだけで一度も代入されない。設計上の分離は
            // CFITOM::initDevices()のコメント参照)。fitomInst.getDevice()を
            // 使うこと(cfg.getDevice()は常にnullptrを返す)。
            auto* dev = fitomInst.getDevice(devIdx);
            if (dev) {
                mon.deviceName = dev->getDescriptor();
                const auto* cs = dev->getChState(devCh);
                if (cs && cs->isActive()) {
                    mon.sounding  = true;
                    mon.lastNote  = cs->lastNote;
                    mon.velocity  = cs->velocity;
                    mon.fnumBlock = cs->lastFnum.block;
                    mon.fnum      = cs->lastFnum.fnum;
                    if (mon.lastNote != 0xFF) mon.noteName = midiNoteName(mon.lastNote);
                }
            }
        }

        // キーボードビュー用: 現在発音中の全ノート(ポリフォニー)。
        // 上のsounding判定(直近使用デバイスの単一ChStateのみを見る)とは
        // 独立に、チャンネル側が把握している発音中ノート集合をそのまま
        // 使う(soundingがfalseでも、他のノートが発音中の場合がある)。
        for (const auto& an : midich->getActiveNotes()) {
            mon.activeNotes.push_back({ an.note, an.velocity });
        }

        result.push_back(mon);
    }
    return result;
}

FITOMChannelSettings FITOMBridge::getChannelSettings(int mpuIndex, int ch) const
{
    FITOMChannelSettings settings;
    if (!initialized_) return settings;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return settings;
    if (ch < 0 || ch >= 16) return settings;

    auto& fitomInst = fitom::CFITOM::instance();

    // SF2直行パス(2026年8月新設): 窓に含まれる(mpu,ch)はCInstCh/CRhythmCh
    // へのディスパッチ自体が行われないため(getChannelMonitors()と同じ
    // 理由)、midich由来の解決を一切行わない。Volume/Expression/Panpot/
    // Monoは読み戻す手段が無い(fluidsynth側の現在値をFITOM_Xは把握
    // していない)ため既定値のままにする(スライダー自体は引き続き有効で、
    // 操作すればCC#7/#10/#11/#126/#127がそのままfluidsynthへ転送される)。
    const auto sf2 = fitomInst.getSf2ChannelWindowInfo(
        static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch));
    if (sf2.assigned) {
        settings.isSf2Windowed     = true;
        settings.sf2FluidsynthChan = sf2.fluidsynthChan;
        settings.sf2Bank           = (sf2.lastCc32Bank >= 0) ? sf2.lastCc32Bank : 0;
        settings.sf2Prog           = (sf2.lastProg >= 0) ? sf2.lastProg : 0;
        return settings;
    }

    auto* processor = fitomInst.getMidiProcessor(static_cast<uint8_t>(mpuIndex));
    if (!processor) return settings;

    fitom::IMidiCh* midich = processor->getChannel(static_cast<uint8_t>(ch));
    if (!midich) return settings;

    settings.volume     = midich->getVolume();
    settings.expression = midich->getExpression();
    settings.panpot     = midich->getPanpot();
    settings.isRhythm   = midich->isRhythm();
    settings.monoMode   = midich->isMonoMode();
    settings.bankSelMSB = midich->getBankSelMSB();
    settings.bankNo     = midich->getBankNo();
    settings.progNo     = midich->getProgramNo();
    return settings;
}

// ─── SF2直行パス (docs/sf2-fluidsynth-integration.md参照、2026年8月新設) ───

void FITOMBridge::setSf2ChannelWindow(int mpuIndex, int ch, int fluidsynthChanOr7F)
{
    if (!initialized_) return;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return;
    if (ch < 0 || ch >= 16) return;
    if (fluidsynthChanOr7F < 0 || fluidsynthChanOr7F > 0x7F) return;

    fitom::CFITOM::instance().setSf2ChannelWindow(
        static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch),
        static_cast<uint8_t>(fluidsynthChanOr7F));
}

std::vector<FITOMSf2WindowAssignment> FITOMBridge::getAssignedSf2Windows() const
{
    std::vector<FITOMSf2WindowAssignment> result;
    if (!initialized_) return result;

    for (const auto& a : fitom::CFITOM::instance().listAssignedSf2Windows()) {
        result.push_back(FITOMSf2WindowAssignment{
            static_cast<int>(a.mpu), static_cast<int>(a.ch), static_cast<int>(a.fluidsynthChan)});
    }
    return result;
}

std::vector<FITOMBankInfo> FITOMBridge::getSf2BankList() const
{
    std::vector<FITOMBankInfo> result;
    if (!initialized_) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    const auto& reg = cfg.getSf2BankRegistry();
    const auto& files = reg.soundfontFiles();
    for (const auto& b : reg.listBanks()) {
        FITOMBankInfo info;
        info.bankNo = b.bank;
        if (b.soundfontIndex >= 0 && static_cast<size_t>(b.soundfontIndex) < files.size()) {
            info.name = fs::path(files[b.soundfontIndex]).filename().string()
                + " (bank " + std::to_string(b.sf2Bank) + ")";
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<FITOMPatchInfo> FITOMBridge::getSf2BankPatches(int bank) const
{
    std::vector<FITOMPatchInfo> result;
    if (!initialized_) return result;
    if (bank < 0 || bank > 127) return result;

    auto& cfg = fitom::CFITOM::instance().getConfig();
    for (const auto& p : cfg.getSf2BankRegistry().listPresetsInBank(static_cast<uint8_t>(bank))) {
        FITOMPatchInfo info;
        info.bank       = bank;
        info.prog       = p.preset;
        info.name       = p.name;
        info.layerCount = 0; // SF2プリセットにFITOM_X側のToneLayer概念は無い
        result.push_back(std::move(info));
    }
    return result;
}

// ================================================================
//  MIDI送信 (CH設定ダイアログのOKで使う)
// ================================================================

void FITOMBridge::sendControlChange(int mpuIndex, int ch, uint8_t cc, uint8_t val)
{
    if (!initialized_) return;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return;
    if (ch < 0 || ch >= 16) return;
    fitom::CFITOM::instance().sendChannelControlChange(
        static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch), cc, val);
}

void FITOMBridge::sendProgramChange(int mpuIndex, int ch, uint8_t prog)
{
    if (!initialized_) return;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return;
    if (ch < 0 || ch >= 16) return;
    fitom::CFITOM::instance().sendChannelProgramChange(
        static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch), prog);
}

void FITOMBridge::sendNoteOn(int mpuIndex, int ch, uint8_t note, uint8_t vel)
{
    if (!initialized_) return;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return;
    if (ch < 0 || ch >= 16) return;
    fitom::CFITOM::instance().sendChannelNoteOn(
        static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch), note, vel);
}

void FITOMBridge::sendNoteOff(int mpuIndex, int ch, uint8_t note)
{
    if (!initialized_) return;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return;
    if (ch < 0 || ch >= 16) return;
    fitom::CFITOM::instance().sendChannelNoteOff(
        static_cast<uint8_t>(mpuIndex), static_cast<uint8_t>(ch), note);
}

bool FITOMBridge::resolveChannelHwPatch(int mpuIndex, int ch, std::string& outKind,
                                         std::string& outBankFile, int& outProgNo) const
{
    if (!initialized_) return false;
    if (mpuIndex < 0 || mpuIndex >= fitom::CFITOM::getMpuCount()) return false;
    if (ch < 0 || ch >= 16) return false;

    auto& fitomInst = fitom::CFITOM::instance();
    auto* processor = fitomInst.getMidiProcessor(static_cast<uint8_t>(mpuIndex));
    if (!processor) return false;

    fitom::IMidiCh* midich = processor->getChannel(static_cast<uint8_t>(ch));
    if (!midich) return false;

    auto& pm  = fitomInst.getPatchManager();
    auto& cfg = fitomInst.getConfig();

    // リズムチャンネルはCC#0/#32を無視し、ノート単位で複数のHwPatchを
    // 使いうる(単一の「現在のパッチ」という概念が無い)ため、個々の
    // HwPatchではなくドラムキット自体(kind="drum"、FITOM_patch_editor側
    // D-040)をキオスク対象にする。bank-fileだけでキットが一意に決まり、
    // prog引数はエディタ側で「プロファイルのdrum_banks[].progと一致するか」
    // の整合性チェックにのみ使われる(単一ファイル=単一キットのため、
    // 探すためのキーではない)。
    if (midich->isRhythm()) {
        const auto* dp = pm.drumRegistry().resolve(midich->getBankNo(), midich->getProgramNo());
        if (!dp || dp->filename.empty()) return false;

        outKind     = "drum";
        outBankFile = dp->filename;
        outProgNo   = static_cast<int>(midich->getProgramNo());
        return true;
    }

    // 発音履歴(ChState/ノートオン)には依存しない。CInstCh::progChange()が
    // 実際に参照しているのと同じ値(このチャンネルの「現在のCC#0/#32/
    // プログラムチェンジ値」)をその場で読む。progChangeを一度も受信して
    // いないチャンネルでも、これらのメンバは初期値(0)のまま公開されるため、
    // 「未受信なら初期値(PatchBank0/Prog0)扱い」を自然に満たす。
    const uint8_t bankSelMSB = midich->getBankSelMSB();
    const uint8_t bankNoLsb  = static_cast<uint8_t>(midich->getBankNo());
    const uint8_t progNo     = midich->getProgramNo();

    if (bankSelMSB == 0) {
        // 通常モード: レイヤードパッチ(PatchBank)そのものをキオスク対象に
        // する(FITOM_patch_editor側 kind="layered")。以前はここで
        // pm.resolve()により先頭ToneLayerのHwPatchまで解決していたため、
        // 「レイヤードパッチ経由だった」という情報が失われ、常に
        // デバイスパッチ編集画面が開いてしまう不具合があった(D-039)。
        // bankNoLsb = PatchBank番号、progNo = そのバンク内のPatch番号を
        // そのまま使う(解決は行わず存在確認のみ)。
        const fitom::PatchBank* bank = pm.findPatchBank(static_cast<int>(bankNoLsb));
        if (!bank || bank->filename.empty()) return false;
        if (!bank->get(static_cast<int>(progNo)).isValid()) return false;

        outKind     = "layered";
        outBankFile = bank->filename;
        outProgNo   = static_cast<int>(progNo);
        return true;
    }
    if (bankSelMSB <= 0x6F) {
        // 直接モード: bankSelMSB自体がVoicePatchType、bankNoLsb = HwBank
        // インデックス、progNo = HwProgそのもの(FITOM_patch_editor側
        // kind="device")。
        fitom::Patch directStorage; // resolveDirect()内でのみ参照される
        fitom::ResolvedPatch resolved =
            pm.resolveDirect(bankSelMSB, bankNoLsb, progNo, cfg, directStorage);
        if (!resolved.isValid() || resolved.layerCount <= 0) return false;
        const fitom::ResolvedLayer& rl = resolved.layers[0];
        if (!rl.hwPatch) return false; // AWM等HwBankを使わない音色、または解決失敗

        // HwPatch::id = (bank_no << 16) | prog_no (PatchData.h参照)
        const int bankNo = static_cast<int>(rl.hwPatch->id >> 16);
        const int prog   = static_cast<int>(rl.hwPatch->id & 0xFFFFu);

        // deviceType はプロファイルのdevices[]メタデータ(FITOMConfig側)から
        // 得る。ISoundDeviceの実体(CFITOM::getDevice())は問わない
        // (resolveTriple()自身がこのメタデータだけでVoiceGroup照合している
        // ため、実体の有無に依存させる必要が無い。実機HWプラグイン未接続の
        // 環境でも解決できるようにするため意図的にこちらを使う)。
        const uint32_t  deviceType = cfg.getDeviceType(rl.deviceIndex);
        const uint8_t   vpt        = fitom::FITOMConfig::deviceTypeToVoicePatchType(deviceType);
        const uint32_t  group      = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(vpt);

        const fitom::HwBank* hwBank = pm.hwRegistry().find(group, bankNo);
        if (!hwBank || hwBank->filename.empty()) return false;

        outKind     = "device";
        outBankFile = hwBank->filename;
        outProgNo   = prog;
        return true;
    }
    return false; // 0x70(内蔵リズム専用バンク)等、本機能の対象外
}

// ================================================================
//  パッチ一覧
// ================================================================

std::vector<FITOMPatchInfo> FITOMBridge::getPatches(int bankNo) const
{
    std::vector<FITOMPatchInfo> result;
    if (!initialized_) return result;

    auto& pm = fitom::CFITOM::instance().getPatchManager();
    const auto* bank = pm.findPatchBank(bankNo);
    if (!bank) return result;

    for (int i = 0; i < fitom::BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        FITOMPatchInfo info;
        info.bank       = bankNo;
        info.prog       = i;
        info.name       = p.name;
        info.layerCount = p.activeLayerCount();
        result.push_back(info);
    }
    return result;
}

std::vector<std::string> FITOMBridge::getPatchBankNames() const
{
    // TODO: PatchManager に bank 名列挙を追加
    return {};
}

// ================================================================
//  パッチピッカー用階層列挙
// ================================================================

std::vector<FITOMBankInfo> FITOMBridge::getPatchBankList() const
{
    std::vector<FITOMBankInfo> result;
    if (!initialized_) return result;

    auto& pm = fitom::CFITOM::instance().getPatchManager();
    for (int bankNo : pm.listPatchBankNumbers()) {
        FITOMBankInfo info;
        info.bankNo = bankNo;
        const auto* bank = pm.findPatchBank(bankNo);
        if (bank) info.name = bank->name;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<FITOMBankInfo> FITOMBridge::getHwBankList(uint8_t voicePatchType) const
{
    std::vector<FITOMBankInfo> result;
    if (!initialized_) return result;

    auto& pm = fitom::CFITOM::instance().getPatchManager();

    // サンプルベース音源系(ADPCM-B/ADPCM-A/PCMD8/AWM)はHwBankRegistryでは
    // なくSampleZoneBankRegistryを参照する(PatchManager::resolveDirect()の
    // isSampleBasedVoicePatchType分岐と同じ、2026年7月修正。以前はここが
    // 常にhwRegistry()を見ており、対象チップのバンク一覧が常に空になる
    // 不具合があった)。
    if (isSampleBasedVoicePatchType(voicePatchType)) {
        for (int bankNo : pm.sampleRegistry().listBankNumbers(voicePatchType)) {
            FITOMBankInfo info;
            info.bankNo = bankNo;
            const auto* bank = pm.sampleRegistry().find(voicePatchType, bankNo);
            if (bank) info.name = bank->name;
            result.push_back(std::move(info));
        }
        return result;
    }

    // 内蔵リズム音源(VOICE_PATCH_BUILTIN_RHYTHM、2026年7月追加修正)。
    // 通常のHwBankRegistryを一切経由しないため、対象チップ(OPNA/OPLL)を
    // 固定の選択肢として列挙する(JSON設定によるバンク一覧が存在しない)。
    if (voicePatchType == VOICE_PATCH_BUILTIN_RHYTHM) {
        for (const auto& c : kBuiltinRhythmChips) {
            FITOMBankInfo info;
            info.bankNo = c.chipSel;
            info.name   = c.label;
            result.push_back(std::move(info));
        }
        return result;
    }

    // OPLL系ROM音色(バンク0固定、2026年7月追加修正)。バンク0はJSON定義
    // 不可の予約領域のため、通常のhwRegistry検索とは別に合成して先頭へ
    // 追加する(hwRegistry側にバンク0が誤って登録されていても、
    // resolveTriple()側で常に無視される[docs/patch-structure-design.md
    // 参照]のと同じ優先順位に揃えるため、hwRegistry側のバンク0は
    // スキップする)。
    const bool hasOpllRom = (pm.getOpllRomPatches(voicePatchType) != nullptr);
    if (hasOpllRom) {
        FITOMBankInfo info;
        info.bankNo = 0;
        info.name   = "ROM Preset";
        result.push_back(std::move(info));
    }

    // 列挙はVoiceGroup(OPM/OPZ/OPZ2等、複数チップで共有)単位で行う一方、
    // PatchManager::resolveTriple()はバンクのvoicePatchTypeとの厳密一致を
    // 要求する。そのため、ここに現れても選ぶと解決に失敗するバンク
    // (別チップとして登録されているもの)が混ざりうる。挙動は変えずに、
    // その不整合が起きているバンクをログに出しておく。
    const auto group = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    std::string unresolvable;
    for (int bankNo : pm.hwRegistry().listBankNumbers(group)) {
        if (hasOpllRom && bankNo == 0) continue;
        FITOMBankInfo info;
        info.bankNo = bankNo;
        const auto* bank = pm.hwRegistry().find(group, bankNo);
        if (bank) {
            info.name = bank->name;
            if (bank->voicePatchType != voicePatchType) {
                if (!unresolvable.empty()) unresolvable += ", ";
                unresolvable += "CC#32=" + std::to_string(bankNo)
                    + " \"" + bank->name + "\" ("
                    + fitom::FITOMConfig::voicePatchTypeToString(bank->voicePatchType) + ")";
            }
        }
        result.push_back(std::move(info));
    }
    if (!unresolvable.empty()) {
        FITOM_LOG_WARN("getHwBankList: chip="
            << fitom::FITOMConfig::voicePatchTypeToString(voicePatchType)
            << " (CC#0=" << (int)voicePatchType << ") の一覧に、別チップとして"
            << "登録されているため選んでも解決に失敗するバンクが含まれる: "
            << unresolvable);
    }
    return result;
}

std::vector<FITOMPatchInfo> FITOMBridge::getHwBankPatches(uint8_t voicePatchType, int hwBank) const
{
    std::vector<FITOMPatchInfo> result;
    if (!initialized_) return result;

    auto& pm = fitom::CFITOM::instance().getPatchManager();

    // サンプルベース音源系: getHwBankList()と同じ理由でSampleZoneBankRegistry
    // を参照する。
    if (isSampleBasedVoicePatchType(voicePatchType)) {
        const auto* bank = pm.sampleRegistry().find(voicePatchType, hwBank);
        if (!bank) return result;
        for (int prog = 0; prog < fitom::BANK_PROG_SIZE; ++prog) {
            const auto& p = bank->get(prog);
            if (!p.isValid()) continue;
            FITOMPatchInfo info;
            info.bank       = hwBank;
            info.prog       = prog;
            info.name       = p.name;
            info.layerCount = 1; // 直接デバイス選択モードは常に単層
            result.push_back(std::move(info));
        }
        return result;
    }

    // 内蔵リズム音源: getHwBankList()と同じ理由。hwBankがchipSel
    // (VOICE_PATCH_OPN2/VOICE_PATCH_OPLL)、progが対象チップのパート番号
    // (=デバイスチャンネル番号)を表す。
    if (voicePatchType == VOICE_PATCH_BUILTIN_RHYTHM) {
        const char* const* names = nullptr;
        size_t count = 0;
        getBuiltinRhythmNames(static_cast<uint8_t>(hwBank), names, count);
        for (size_t i = 0; i < count; ++i) {
            FITOMPatchInfo info;
            info.bank       = hwBank;
            info.prog       = static_cast<int>(i);
            info.name       = names[i];
            info.layerCount = 1;
            result.push_back(std::move(info));
        }
        return result;
    }

    // OPLL系ROM音色(バンク0固定): getHwBankList()と同じ理由。
    if (hwBank == 0) {
        const auto* patches = pm.getOpllRomPatches(voicePatchType);
        if (patches) {
            for (size_t i = 0; i < patches->size(); ++i) {
                if (i == 0) continue; // 下位4bit=0は無音として意図的に予約(resolveOpllRomVoice参照)
                const auto& p = (*patches)[i];
                if (!p.isValid()) continue;
                FITOMPatchInfo info;
                info.bank       = hwBank;
                // 【重要】progは配列添字(=instIndexのみ)ではなくp.id
                // (variantSel<<4|instIndex)を使うこと。実際の発音解決
                // (PatchManager::resolveOpllRomVoice)はこのProgram Change
                // 値自体からvariantSelを再デコードするため、生の添字を
                // 送るとOPLL以外のカテゴリ(OPLLX/OPLLP/VRC7、variantSel
                // >0)で誤ったチップの音色が鳴ってしまう。
                info.prog       = static_cast<int>(p.id);
                info.name       = p.name;
                info.layerCount = 1;
                result.push_back(std::move(info));
            }
            return result;
        }
    }

    const auto group = fitom::FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    const auto* bank = pm.hwRegistry().find(group, hwBank);
    if (!bank) return result;

    for (int prog = 0; prog < fitom::BANK_PROG_SIZE; ++prog) {
        const auto& p = bank->get(prog);
        if (!p.isValid()) continue;
        FITOMPatchInfo info;
        info.bank       = hwBank;
        info.prog       = prog;
        info.name       = p.name;
        info.layerCount = 1; // 直接デバイス選択モードは常に単層
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<FITOMPatchInfo> FITOMBridge::getDrumPatches() const
{
    std::vector<FITOMPatchInfo> result;
    if (!initialized_) return result;

    auto& pm = fitom::CFITOM::instance().getPatchManager();
    for (int prog = 0; prog < fitom::BANK_PROG_SIZE; ++prog) {
        const auto* dp = pm.drumRegistry().resolve(0, prog);
        if (!dp) continue;
        FITOMPatchInfo info;
        info.bank       = 0;
        info.prog       = prog;
        info.name       = dp->name;
        info.layerCount = 0; // ドラムキットには「レイヤー数」の概念が無い
        result.push_back(std::move(info));
    }
    return result;
}

// ================================================================
//  音色エディタ連携
// ================================================================

std::string FITOMBridge::getHwPatchJson(int bankNo, int prog) const
{
    if (!initialized_) return "{}";
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    // TODO: HwBankRegistry から HwPatch を取得して JSON 変換
    return "{}";
}

bool FITOMBridge::setHwPatchJson(int bankNo, int prog, const std::string& js)
{
    if (!initialized_) return false;
    try {
        json j = json::parse(js);
        // TODO: json → HwPatch 変換 → HwBankRegistry::set()
        return true;
    } catch (...) { return false; }
}

std::string FITOMBridge::getSwPatchJson(int bankNo, int prog) const
{
    return "{}"; // TODO
}

bool FITOMBridge::setSwPatchJson(int bankNo, int prog, const std::string& js)
{
    return false; // TODO
}

// ================================================================
//  マスターボリューム / ピッチ
// ================================================================

void    FITOMBridge::setMasterVolume(uint8_t vol) {
    if (initialized_) fitom::CFITOM::instance().setMasterVolume(vol);
}
uint8_t FITOMBridge::getMasterVolume() const {
    return initialized_ ? fitom::CFITOM::instance().getMasterVolume() : 100;
}

void   FITOMBridge::setMasterPitch(double hz) {
    // CFITOM::setMasterPitch()経由にする(FnumRegistry直叩きだと、
    // Config側(config_->masterPitch_、プロファイル書き戻しの対象)の更新と
    // 発音中チャンネルへのF-number即時反映[onMasterPitchChanged]が
    // どちらも行われなかったバグを修正、2026年7月)。
    if (initialized_) fitom::CFITOM::instance().setMasterPitch(hz);
}
double FITOMBridge::getMasterPitch() const {
    return initialized_ ? fitom::CFITOM::instance().getMasterPitch() : 440.0;
}

// ================================================================
//  直接操作
// ================================================================

void FITOMBridge::allNoteOff()   { if (initialized_) fitom::CFITOM::instance().allNoteOff(); }
void FITOMBridge::resetAllCtrl() { if (initialized_) fitom::CFITOM::instance().resetAllCtrl(); }

// ================================================================
//  タイマーコールバック (MFC WM_TIMER から毎 1ms 呼ぶ)
// ================================================================

void FITOMBridge::onTimer(uint32_t tick) {
    if (initialized_) fitom::CFITOM::instance().timerCallback(tick);
}

void FITOMBridge::startTimerThread(uint32_t intervalMs) {
    if (initialized_) fitom::CFITOM::instance().startTimerThread(intervalMs);
}

void FITOMBridge::stopTimerThread() {
    if (initialized_) fitom::CFITOM::instance().stopTimerThread();
}

// ================================================================
//  オーディオデバイス列挙
// ================================================================

std::vector<std::string> FITOMBridge::enumerateAudioDevices() const {
    // オーディオデバイス列挙は fitom_fmhwif DLL が担当。
    // fitom_core は RtAudio に依存しないため、ここでは空リストを返す。
    // GUI はプロファイルの audio_api 設定で API を選択する。
    return {};
}

// ================================================================
//  バンクファイル I/O
// ================================================================

bool FITOMBridge::loadHwBankFile(const std::string& path, int bankNo) {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.loadHwBankJson(fs::path(path), 0, bankNo);
}

bool FITOMBridge::saveHwBankFile(const std::string& path, int bankNo) const {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.saveHwBankJson(fs::path(path), 0, bankNo);
}

bool FITOMBridge::loadPatchBankFile(const std::string& path, int bankNo) {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.loadPatchBankJson(fs::path(path), bankNo);
}

bool FITOMBridge::savePatchBankFile(const std::string& path, int bankNo) const {
    if (!initialized_) return false;
    auto& pm = fitom::CFITOM::instance().getPatchManager();
    return pm.savePatchBankJson(fs::path(path), bankNo);
}

void FITOMBridge::setStatusCallback(StatusCb cb) {
    statusCb_ = std::move(cb);
}
