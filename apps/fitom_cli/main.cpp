// apps/fitom_cli/main.cpp
//
// GUI実装前にコア機能を評価するためのテキストコンソール版CLIツール。
// 実MIDIデバイスからの入力を受け取り、16 MIDIチャンネルの発音状況を
// topコマンド風に定期再描画するモニター画面を表示する。
//
// 使い方:
//   fitom_cli <profile.json> [midi_backend.so/dll] [midi_in_device_name]
//
// MIDIバックエンドを省略した場合、MIDI入力なしでコア機能の初期化のみ
// 確認する (音色バンクロード・デバイス構成の妥当性確認用)。
//
// Prog名・Device名の解決はこのCLI側 (プロファイル/PatchManagerを保持する
// 側) が行う。MIDIレシーバー(IMidiCh)やチップドライバ(ISoundDevice)は
// bankNo/progNo/deviceIndex/devChのようなID・インデックスのみを提供し、
// 名前解決の責務は持たない。

#include "fitom/CFITOM.h"
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

using namespace fitom;

namespace {

std::atomic<bool> g_running{true};
void onSigInt(int) { g_running = false; }

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
// チップ名文字列の解決には CFITOM::getDeviceNameFromId (プロファイル/
// デバイス定義に基づく静的マップ) を使う。
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
// 発音中でなければ空文字。
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

// 画面全体を再描画する (topコマンド風、ANSIエスケープでカーソルホーム+クリア)。
void renderMonitor(CFITOM& fitom, PatchManager& pm, const std::string& midiInName) {
    std::ostringstream out;
    out << "\x1b[H\x1b[2J"; // カーソルホーム + 画面クリア
    out << "MIDI IN[0]: " << midiInName << "\n";
    out << "CH  Bank   Prog                        Note Vol  Device   ch  fnum\n";

    MidiProcessor* proc = fitom.getMidiProcessor(0);
    for (int ch = 0; ch < 16; ++ch) {
        IMidiCh* c = proc ? proc->getChannel(static_cast<uint8_t>(ch)) : nullptr;

        char chLabel = (ch < 10) ? static_cast<char>('0' + ch)
                                  : static_cast<char>('A' + (ch - 10));
        out << chLabel << "   ";

        if (!c) {
            out << "\n";
            continue;
        }

        uint16_t bankNo = c->getBankNo();
        uint8_t  progNo = c->getProgramNo();
        uint8_t  lastNote = c->getLastNote();
        uint8_t  vol = c->getVolume();
        uint8_t  devIdx = c->getLastDeviceIndex();
        uint8_t  devCh  = c->getLastDevCh();

        std::string progName = patchLabel(pm, bankNo, progNo);
        std::ostringstream progCol;
        progCol << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<int>(progNo) << ":" << progName;

        out << std::dec << std::setfill('0') << std::setw(2) << (bankNo >> 8)
            << ":" << std::setw(2) << (bankNo & 0xFF) << "  "
            << std::left << std::setw(28) << std::setfill(' ') << progCol.str()
            << std::right
            << std::setw(4)  << noteToName(lastNote) << " "
            << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(vol) << "  "
            << std::dec << std::left << std::setw(8) << std::setfill(' ')
            << deviceLabel(fitom, devIdx)
            << std::right
            << std::setw(3) << (devCh == 0xFF ? std::string("-") : std::to_string(devCh)) << "  "
            << fnumLabel(fitom, devIdx, devCh)
            << "\n";
    }

    out << std::flush;
    std::fputs(out.str().c_str(), stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "使い方: " << argv[0]
                  << " <profile.json> [midi_backend.so/dll] [midi_in_device_name]\n";
        return 1;
    }
    const std::string profilePath = argv[1];

    auto config   = std::make_unique<FITOMConfig>();
    auto patchMgr = std::make_unique<PatchManager>();
    PatchManager* pmPtr = patchMgr.get(); // init()への所有権移譲前に保持

    if (!config->loadProfile(profilePath, pmPtr)) {
        std::cerr << "プロファイル読み込み失敗: " << profilePath << "\n";
        return 1;
    }

    CFITOM& fitomInst = CFITOM::instance();
    if (fitomInst.init(std::move(config), std::move(patchMgr)) != 0) {
        std::cerr << "FITOM初期化失敗\n";
        return 1;
    }
    fitomInst.startTimerThread();

    // MIDI入力接続 (実デバイス)。バックエンドDLLパスが指定された場合のみ。
    std::shared_ptr<MidiPluginInstance> midiPlugin;
    std::unique_ptr<MidiInPort> midiPort;
    std::string midiInName = "(no MIDI input)";

    if (argc >= 3) {
        try {
            midiPlugin = MidiPluginInstance::load(argv[2]);
            auto ins = midiPlugin->enumerateIn();
            std::string target = (argc >= 4) ? argv[3] : (ins.empty() ? "" : ins[0]);
            if (!target.empty()) {
                MidiProcessor* proc = fitomInst.getMidiProcessor(0);
                midiPort = std::make_unique<MidiInPort>(
                    midiPlugin, target,
                    [proc](const uint8_t* data, size_t len, uint64_t ts) {
                        if (proc) proc->receiveByte(data, len, ts);
                    });
                midiInName = target;
            } else {
                std::cerr << "警告: 利用可能なMIDI入力デバイスがありません\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "MIDIバックエンド読み込み失敗: " << e.what() << "\n";
        }
    }

    std::signal(SIGINT, onSigInt);

    // メインループ: topコマンド風に画面全体を定期再描画する。
    while (g_running) {
        renderMonitor(fitomInst, *pmPtr, midiInName);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    midiPort.reset();
    fitomInst.stopTimerThread();
    fitomInst.exit();

    std::cout << "\n終了しました。\n";
    return 0;
}
