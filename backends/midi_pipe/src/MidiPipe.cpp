// backends/midi_pipe/src/MidiPipe.cpp
// 内部用MIDIパイプ バックエンド DLL
//
// 外部プロセス(別プロジェクトのパッチエディタ等)から、ローカルのみで
// FITOM_Xへ生MIDIバイト列(SysExを含む)を送るための、クロスプラット
// フォームなMIDIバックエンド。名前付きパイプ(Windows)/UNIXドメイン
// ソケット(Linux/macOS)を使う。
//
// MIDI Outは現状未実装(このバックエンドの用途は「外部プロセス→FITOM_X」
// の一方向のみ)。MidiPlugin_OpenOutは常にMIDI_ERR_NOT_FOUNDを返す。
//
// デバイス列挙は固定の1エントリ(kDeviceName)のみを返す。実際の接続先
// (パイプ名/ソケットパス)はkPipeName/kSocketPathで固定されており、
// プロファイル側の指定は使わない(接続先を都度探す必要をなくすため)。
// 詳細な仕様はdocs/plugin-midi-pipe.mdを参照。

#include <fitom/IMidiPlugin.h>
#include <nlohmann/json.hpp>

#include <string>
#include <thread>
#include <atomic>
#include <cstring>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace {

constexpr const char* kDeviceName = "FITOM Internal Pipe";

#if defined(_WIN32)
constexpr const char* kPipeName = "\\\\.\\pipe\\FITOM_X_MIDI";
#else
constexpr const char* kSocketPath = "/tmp/fitom_x_midi.sock";
#endif

} // namespace

struct MidiInDevice {
    std::thread        thread;
    std::atomic<bool>  running { false };
    MidiInCallback     callback = nullptr;
    void*              userData = nullptr;
#if defined(_WIN32)
    HANDLE pipe = INVALID_HANDLE_VALUE;
#else
    int listenFd = -1;
    int clientFd = -1;
#endif

    ~MidiInDevice() {
        running.store(false);
#if defined(_WIN32)
        // ConnectNamedPipe/ReadFileでブロックしているスレッドを起こすため、
        // ハンドルを閉じる(CancelIoExの方が丁寧だが、対象OS世代を広く
        // カバーするため単純にCloseHandleで代替する)。
        if (pipe != INVALID_HANDLE_VALUE) {
            HANDLE h = pipe;
            pipe = INVALID_HANDLE_VALUE;
            CloseHandle(h);
        }
#else
        // accept()/read()でブロックしているスレッドを起こす。
        if (clientFd >= 0) shutdown(clientFd, SHUT_RDWR);
        if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
#endif
        if (thread.joinable()) thread.join();
#if !defined(_WIN32)
        if (clientFd >= 0) close(clientFd);
        if (listenFd >= 0) close(listenFd);
        ::unlink(kSocketPath);
#endif
    }
};

namespace {

#if defined(_WIN32)

// 接続→切断を繰り返す想定(パッチエディタ側が起動/終了するたびに
// 繋ぎ直す)。1本のパイプインスタンスで、接続が切れたら次の接続を
// 待ち直す。
void midiInThread(MidiInDevice* dev)
{
    uint8_t buf[4096];
    while (dev->running.load()) {
        HANDLE h = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,      // 同時接続は1本のみ
            0, 4096,
            0, nullptr);
        if (h == INVALID_HANDLE_VALUE) break;
        dev->pipe = h;

        BOOL connected = ConnectNamedPipe(h, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!dev->running.load()) { CloseHandle(h); break; }

        if (connected) {
            DWORD n = 0;
            while (dev->running.load() &&
                   ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
                dev->callback(buf, n, 0, dev->userData);
            }
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
        dev->pipe = INVALID_HANDLE_VALUE;
    }
}

#else

void midiInThread(MidiInDevice* dev)
{
    uint8_t buf[4096];
    while (dev->running.load()) {
        sockaddr_un clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int fd = ::accept(dev->listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (fd < 0) {
            if (!dev->running.load()) break;
            continue; // shutdown()以外の理由でaccept失敗した場合は単に再試行
        }
        dev->clientFd = fd;

        ssize_t n;
        while (dev->running.load() &&
               (n = ::read(fd, buf, sizeof(buf))) > 0) {
            dev->callback(buf, static_cast<size_t>(n), 0, dev->userData);
        }
        ::close(fd);
        dev->clientFd = -1;
    }
}

#endif

} // namespace

// ─── エクスポート関数 ─────────────────────────────────────────────────────────

extern "C" {

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_GetName() {
    return "InternalPipe";
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateIn() {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back(kDeviceName);
    const std::string s = arr.dump();
    char* buf = new char[s.size() + 1];
    std::strcpy(buf, s.c_str());
    return buf;
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateOut() {
    // MIDI Out(FITOM_X → 外部プロセス方向)は現状未実装。
    nlohmann::json arr = nlohmann::json::array();
    const std::string s = arr.dump();
    char* buf = new char[s.size() + 1];
    std::strcpy(buf, s.c_str());
    return buf;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_FreeString(const char* str) {
    delete[] const_cast<char*>(str);
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenIn(
    const char*    device_name,
    MidiInCallback callback,
    void*          user_data,
    MidiInHandle*  out_handle)
{
    if (!callback || !out_handle) return MIDI_ERR_UNAVAILABLE;
    if (!device_name || std::string(device_name) != kDeviceName) {
        return MIDI_ERR_NOT_FOUND;
    }

    auto* dev = new MidiInDevice();
    dev->callback = callback;
    dev->userData = user_data;

#if !defined(_WIN32)
    ::unlink(kSocketPath); // 前回異常終了時の残骸を消しておく
    dev->listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (dev->listenFd < 0) { delete dev; return MIDI_ERR_OPEN_FAILED; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);
    if (::bind(dev->listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(dev->listenFd, 1) < 0) {
        ::close(dev->listenFd);
        delete dev;
        return MIDI_ERR_OPEN_FAILED;
    }
#endif

    dev->running.store(true);
    dev->thread = std::thread(midiInThread, dev);

    *out_handle = reinterpret_cast<MidiInHandle>(dev);
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseIn(MidiInHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<MidiInDevice*>(handle);
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenOut(
    const char* /*device_name*/, MidiOutHandle* /*out_handle*/)
{
    return MIDI_ERR_NOT_FOUND; // 未実装(このバックエンドは受信専用)
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseOut(MidiOutHandle /*handle*/) {
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_Send(
    MidiOutHandle /*handle*/, const uint8_t* /*data*/, size_t /*len*/, uint64_t /*ts*/)
{
    return MIDI_ERR_NOT_FOUND;
}

} // extern "C"
