// apps/fitom_cli/main.cpp
//
// GUI実装前にコア機能を評価するためのテキストコンソール版CLIツール。
// プロファイルに指定された実MIDIデバイスからの入力を受け取り、
// 各MIDI入力ポートごとに16チャンネルの発音状況をtopコマンド風に
// 定期再描画するモニター画面を表示する。
//
// 使い方:
//   fitom_cli <profile.json>
//
// MIDIバックエンドDLLのパスは profile の "midi_backend.dll" で指定する。
// 省略時はプラットフォーム既定 (fitom_midi_rtmidi.dll/.so/.dylib) を
// 実行ファイルと同じディレクトリから探索する。
// 接続する MIDI 入力デバイス名一覧は profile の "midi_inputs" 配列を使う。
//
// Prog名・Device名の解決はこのCLI側 (プロファイル/PatchManagerを保持する
// 側) が行う。MIDIレシーバー(IMidiCh)やチップドライバ(ISoundDevice)は
// bankNo/progNo/deviceIndex/devChのようなID・インデックスのみを提供し、
// 名前解決の責務は持たない。

#include "fitom/CFITOM.h"
#include "fitom/Log.h"
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/MidiManager.h"
#include "fitom/MidiCh.h"
#include "fitom/ISoundDevice.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <memory>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <climits>
#endif

using namespace fitom;
namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_running{true};
void onSigInt(int) { g_running = false; }

// 実行ファイル自身のディレクトリを取得する (相対パス解決・プラット
// フォーム既定MIDIバックエンドの探索基点に使う)。取得できなければ
// カレントディレクトリを返す。
fs::path exeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#endif
    return fs::current_path();
}

// MIDIバックエンドDLLの実際のパスを解決する。
// profile の midi_backend.dll が指定されていればそれを使う (相対パスは
// 実行ファイルのディレクトリを基点とする)。未指定ならプラットフォーム
// 既定のファイル名を実行ファイルと同じディレクトリから探す。
fs::path resolveMidiBackendPath(const std::string& configured) {
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

// MIDIノート番号 → "C4"のような音名表記 (MIDI note 60 = C4)。
std::string noteToName(uint8_t note) {
    if (note >= 128) return "";
    static const char* kNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = static_cast<int>(note) / 12 - 1;
    int idx    = static_cast<int>(note) % 12;
    std::ostringstream oss;
    oss << kNames[idx] << octave;
    return oss.str();
}

// デバイス種別名[インデックス] 表記 ("OPN[0]" 等)。
std::string deviceLabel(CFITOM& fitom, uint8_t deviceIndex) {
    if (deviceIndex == 0xFF) return "-";
    ISoundDevice* dev = fitom.getDevice(deviceIndex);
    if (!dev) return "-";
    std::string name = CFITOM::getDeviceNameFromId(dev->getDeviceType());
    std::ostringstream oss;
    oss << name << "[" << static_cast<int>(deviceIndex) << "]";
    return oss.str();
}

// "bank:prog" から音色名を解決する。見つからなければ空文字。
std::string patchLabel(PatchManager& pm, uint16_t bankNo, uint8_t progNo) {
    const PatchBank* bank = pm.findPatchBank(bankNo);
    if (!bank) return "";
    const Patch& p = bank->get(progNo);
    if (p.name[0] == '\0') return "";
    return std::string(p.name);
}

// 指定チャンネルの現在のfnumを16進4桁で取得する ("025A" 等)。
std::string fnumLabel(CFITOM& fitom, uint8_t deviceIndex, uint8_t devCh) {
    if (deviceIndex == 0xFF || devCh == 0xFF) return "";
    ISoundDevice* dev = fitom.getDevice(deviceIndex);
    if (!dev) return "";
    const ChState* st = dev->getChState(devCh);
    if (!st) return "";
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
        << st->lastFnum.fnum;
    return oss.str();
}

// 1つのMIDI入力ポート分 (16チャンネル) のテーブルを描画する。
// headerSuffix: ヘッダー行に付加する診断情報 (受信バイト数・オープン状態等)。
void renderPort(std::ostringstream& out, CFITOM& fitom, PatchManager& pm,
                 uint8_t portIndex, const std::string& midiInName,
                 const std::string& headerSuffix) {
    out << "MIDI IN[" << static_cast<int>(portIndex) << "]: " << midiInName
        << headerSuffix << "\n";
    out << "CH  Bank   Prog                        Note Vol  Device   ch  fnum\n";

    MidiProcessor* proc = fitom.getMidiProcessor(portIndex);
    for (int ch = 0; ch < 16; ++ch) {
        IMidiCh* c = proc ? proc->getChannel(static_cast<uint8_t>(ch)) : nullptr;

        char chLabel = (ch < 10) ? static_cast<char>('0' + ch)
                                  : static_cast<char>('A' + (ch - 10));
        out << chLabel << "   ";

        if (!c) { out << "\n"; continue; }

        uint16_t bankNo   = c->getBankNo();
        uint8_t  progNo   = c->getProgramNo();
        uint8_t  lastNote = c->getLastNote();
        uint8_t  vol      = c->getVolume();
        uint8_t  devIdx   = c->getLastDeviceIndex();
        uint8_t  devCh    = c->getLastDevCh();

        std::string progName = patchLabel(pm, bankNo, progNo);
        std::ostringstream progCol;
        progCol << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<int>(progNo) << ":" << progName;

        out << std::dec << std::setfill('0') << std::setw(2) << (bankNo >> 8)
            << ":" << std::setw(2) << (bankNo & 0xFF) << "  "
            << std::left << std::setw(28) << std::setfill(' ') << progCol.str()
            << std::right
            << std::setw(4) << noteToName(lastNote) << " "
            << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(vol) << "  "
            << std::dec << std::left << std::setw(8) << std::setfill(' ')
            << deviceLabel(fitom, devIdx)
            << std::right
            << std::setw(3) << (devCh == 0xFF ? std::string("-") : std::to_string(devCh)) << "  "
            << fnumLabel(fitom, devIdx, devCh)
            << "\n";
    }
}

// 画面全体を再描画する (topコマンド風、ANSIエスケープでカーソルホーム+クリア)。
// MIDI入力ポートが複数ある場合は縦に並べて全て表示する。
// byteCounts: ポートごとの累計受信バイト数 (診断用、バックエンドが実際に
// データを受信できているかを一目で確認するため)。openFailed: オープンに
// 失敗したポートかどうか (失敗時はヘッダーにその旨を表示し続ける、
// 従来はエラーメッセージが次の再描画で即座に消えてしまい気づけなかった)。
void renderMonitor(CFITOM& fitom, PatchManager& pm,
                    const std::vector<std::string>& midiInNames,
                    const std::vector<std::shared_ptr<std::atomic<uint64_t>>>& byteCounts,
                    const std::vector<bool>& openFailed) {
    std::ostringstream out;
    out << "\x1b[H\x1b[2J"; // カーソルホーム + 画面クリア

    if (midiInNames.empty()) {
        renderPort(out, fitom, pm, 0, "(no MIDI input)", "");
    } else {
        for (size_t i = 0; i < midiInNames.size(); ++i) {
            if (i > 0) out << "\n";
            std::ostringstream suffix;
            if (openFailed[i]) {
                suffix << "  [オープン失敗]";
            } else {
                suffix << "  (受信バイト数: " << byteCounts[i]->load() << ")";
            }
            renderPort(out, fitom, pm, static_cast<uint8_t>(i), midiInNames[i], suffix.str());
        }
    }

    out << std::flush;
    std::fputs(out.str().c_str(), stdout);
    std::fflush(stdout);
}

// MIDI入力のオープン失敗時、原因調査用にバックエンドが実際に列挙する
// ポート名一覧を出す(findPortByNameは完全一致のみで検索するため、
// profile側の指定名がずれていないか/デバイスが接続されているかを
// その場で確認できるようにする)。
void printAvailableMidiInPorts(const MidiPluginInstance& plugin) {
    auto names = plugin.enumerateIn();
    if (names.empty()) {
        std::cerr << "利用可能なMIDI入力ポートがありません\n";
        return;
    }
    std::cerr << "利用可能なMIDI入力ポート: ";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) std::cerr << ", ";
        std::cerr << "\"" << names[i] << "\"";
    }
    std::cerr << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "使い方: " << argv[0] << " <profile.json>\n";
        return 1;
    }
    const std::string profilePath = argv[1];

    auto config   = std::make_unique<FITOMConfig>();
    auto patchMgr = std::make_unique<PatchManager>();
    PatchManager* pmPtr = patchMgr.get(); // init()への所有権移譲前に保持

    // fitom.conf.json (実行ファイルと同じディレクトリにあれば読み込む。
    // 省略可能なシステム設定のため、無くても継続する) の log.* で
    // 既定のログ設定を上書きできるようにする。既定はファイルに出力する
    // (画面は100ms間隔で全消去されるため、コンソールへの警告ログは
    // すぐに消えて見えなくなってしまう。ファイルに残しておけば、
    // 原因調査時に後から確認できる)。
    fs::path sysConfPath = exeDir() / "fitom.conf.json";
    if (fs::exists(sysConfPath)) config->loadSystemConf(sysConfPath);
    Log::init(config->getLogLevel("debug"),
              config->getLogFile("fitom_cli.log"),
              config->getLogConsole(true));

    if (!config->loadProfile(profilePath, pmPtr)) {
        std::cerr << "プロファイル読み込み失敗: " << profilePath << "\n";
        return 1;
    }

    // MIDI接続に必要な情報は、所有権が CFITOM::init() に移る前に控えておく。
    std::vector<std::string> midiInNames;
    for (int i = 0; i < config->getMidiInputCount(); ++i) {
        midiInNames.push_back(config->getMidiInputName(i));
    }
    fs::path midiBackendPath = resolveMidiBackendPath(config->getMidiBackendDll());

    CFITOM& fitomInst = CFITOM::instance();
    if (fitomInst.init(std::move(config), std::move(patchMgr)) != 0) {
        std::cerr << "FITOM初期化失敗\n";
        return 1;
    }
    fitomInst.startTimerThread();
    // 音声出力は HW プラグイン (FitomEmuIF.dll 等、IHWPlugin実装DLL) が
    // 内部で担う。CLIツールはレジスタ書き込みまでを担い、実際の音声
    // ストリーミング(RtAudio等)には関与しない。

    // MIDI入力接続 (実デバイス)。profile の midi_inputs[] の数だけ、
    // 対応する MidiProcessor(0..N-1) にそれぞれ接続する。
    // byteCounts/openFailed は診断表示用 (バックエンドが実際にデータを
    // 受信できているか、オープン自体に失敗していないかを画面上で
    // 常に確認できるようにするため)。
    std::shared_ptr<MidiPluginInstance> midiPlugin;
    std::vector<std::unique_ptr<MidiInPort>> midiPorts;
    std::vector<std::shared_ptr<std::atomic<uint64_t>>> byteCounts;
    std::vector<bool> openFailed;

    for (size_t i = 0; i < midiInNames.size(); ++i) {
        byteCounts.push_back(std::make_shared<std::atomic<uint64_t>>(0));
        openFailed.push_back(false);
    }

    if (!midiInNames.empty()) {
        try {
            midiPlugin = MidiPluginInstance::load(midiBackendPath);
            for (size_t i = 0; i < midiInNames.size(); ++i) {
                MidiProcessor* proc = fitomInst.getMidiProcessor(static_cast<uint8_t>(i));
                if (!proc) { openFailed[i] = true; continue; }
                auto counter = byteCounts[i];
                try {
                    midiPorts.push_back(std::make_unique<MidiInPort>(
                        midiPlugin, midiInNames[i],
                        [proc, counter](const uint8_t* data, size_t len, uint64_t ts) {
                            counter->fetch_add(len, std::memory_order_relaxed);
                            proc->receiveByte(data, len, ts);
                        }));
                } catch (const std::exception& e) {
                    std::cerr << "MIDI入力 \"" << midiInNames[i]
                              << "\" のオープンに失敗: " << e.what() << "\n";
                    printAvailableMidiInPorts(*midiPlugin);
                    midiPorts.push_back(nullptr);
                    openFailed[i] = true;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "MIDIバックエンド読み込み失敗 (" << midiBackendPath.string()
                      << "): " << e.what() << "\n";
            for (size_t i = 0; i < midiInNames.size(); ++i) openFailed[i] = true;
        }
    }

    std::signal(SIGINT, onSigInt);

    // メインループ: topコマンド風に画面全体を定期再描画する。
    while (g_running) {
        renderMonitor(fitomInst, *pmPtr, midiInNames, byteCounts, openFailed);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    midiPorts.clear();
    fitomInst.stopTimerThread();
    fitomInst.exit();

    std::cout << "\n終了しました。\n";
    return 0;
}
