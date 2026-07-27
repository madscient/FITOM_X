// tests/test_midi_pipe.cpp
// 内部用MIDIパイプ(backends/midi_pipe)の回帰テスト。
//
// このファイルはFITOM_BUILD_BACKEND_MIDI_PIPE=ON時のみビルド対象に
// 追加される(tests/CMakeLists.txt参照)。
//
// 検証対象: パッチエディタが一度も接続しない状態(=acceptThreadが起動直後
// からConnectNamedPipe()でブロックし続けている状態)でMidiInPortを破棄した
// とき、シャットダウン処理がハングしないこと。2026年7月、内部パイプを
// プロファイル設定と無関係に常時有効化する変更を行った直後、実機で
// FITOM_X終了時にハングする不具合が発覚した(ブロッキングモードのハンドルは
// 別スレッドからのCloseHandle()だけでは同期I/O呼び出し中のスレッドを
// 確実には解放できないというWindowsの既知の制約が原因、
// backends/midi_pipe/src/MidiPipe.cppのCancelSynchronousIo()追加で修正)。

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include "fitom/MidiManager.h"

#include <windows.h>
#include <filesystem>
#include <future>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace {

// fitom_tests.exe自身のディレクトリ(.../tests/<Config>/)から、
// fitom_midi_pipe.dllの想定パス(.../bin/<Config>/fitom_midi_pipe.dll)を
// 組み立てる(tests/CMakeLists.txt・backends/midi_pipe/CMakeLists.txtの
// RUNTIME_OUTPUT_DIRECTORY設定に基づく、ビルドディレクトリのレイアウト)。
fs::path resolveMidiPipeDllPath()
{
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    fs::path exeDir = fs::path(buf).parent_path();       // .../tests/<Config>
    fs::path buildRoot = exeDir.parent_path().parent_path(); // .../<preset>
    return buildRoot / "bin" / exeDir.filename() / "fitom_midi_pipe.dll";
}

} // namespace

TEST_CASE("MidiPipe: closing an idle listening port does not hang", "[midi_pipe]")
{
    const fs::path dllPath = resolveMidiPipeDllPath();
    if (dllPath.empty() || !fs::exists(dllPath)) {
        WARN("fitom_midi_pipe.dll not found (" << dllPath.string() << ") — skipping");
        return;
    }

    auto plugin = fitom::MidiPluginInstance::load(dllPath);
    auto* port = new fitom::MidiInPort(
        plugin, "FITOM Internal Pipe",
        [](const uint8_t*, size_t, uint64_t) {});

    // acceptThreadが確実にConnectNamedPipe()でブロックする時間を与える
    // (パッチエディタが一度も接続しない、最も典型的な待機状態を再現する)。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // portの破棄(~MidiInPort → MidiPlugin_CloseIn → ~MidiInDevice)が
    // ハングしないことを確認する。万一ハングした場合でもテストプロセス
    // 自体は先に進めるよう、破棄は別スレッドに切り離して行い、完了通知を
    // タイムアウト付きで待つ(std::asyncの戻り値futureは破棄時に完了を
    // 待ってブロックしてしまうため使えない。ここではstd::threadを
    // detach()し、promise/futureで完了だけを個別に通知させる)。
    auto donePromise = std::make_shared<std::promise<void>>();
    std::future<void> doneFuture = donePromise->get_future();
    std::thread worker([port, donePromise]() {
        delete port; // ハングした場合、このスレッドは戻らない
        donePromise->set_value();
    });
    worker.detach();

    auto status = doneFuture.wait_for(std::chrono::seconds(5));
    REQUIRE(status == std::future_status::ready);
}

#endif // _WIN32
