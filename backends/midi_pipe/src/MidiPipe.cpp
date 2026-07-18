// backends/midi_pipe/src/MidiPipe.cpp
// 内部用MIDIパイプ バックエンド DLL
//
// 外部プロセス(別プロジェクトのパッチエディタ等)から、ローカルのみで
// FITOM_Xへ生MIDIバイト列(SysExを含む)を送るための、クロスプラット
// フォームなMIDIバックエンド。名前付きパイプ(Windows)/UNIXドメイン
// ソケット(Linux/macOS)を使う。
//
// 複数クライアント対応(2026年7月〜): 最大16本(内部パイプMPUの16ch分)
// まで同時接続を受け付ける。接続ごとに専用スレッドで処理し、接続
// 確立直後、読み取りを始める前にFITOM_X側からチャンネル割り当てを
// プライベートSysExで通知する(sub-cmd 0x03、docs/plugin-midi-pipe.md
// 参照)。クライアント側はMIDIチャンネルを自分で選ばず、この通知を
// 待ってから送信を開始する。空きチャンネルが無い場合は接続を即座に
// 切る。
//
// MIDI Outは上記のチャンネル割り当て通知の一往復のみ実装しており、
// それ以外の汎用送信(MidiPlugin_Send/OpenOut)は引き続き未実装
// (このバックエンドの主用途は「外部プロセス→FITOM_X」の受信側)。
//
// デバイス列挙は固定の1エントリ(kDeviceName)のみを返す。実際の接続先
// (パイプ名/ソケットパス)はkPipeName/kSocketPathで固定されており、
// プロファイル側の指定は使わない(接続先を都度探す必要をなくすため)。
// 詳細な仕様はdocs/plugin-midi-pipe.mdを参照。

#include <fitom/IMidiPlugin.h>
#include <nlohmann/json.hpp>

#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <bitset>
#include <algorithm>
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
constexpr int         kMaxClients = 16; // 内部パイプMPUの16ch分

#if defined(_WIN32)
constexpr const char* kPipeName = "\\\\.\\pipe\\FITOM_X_MIDI";
#else
constexpr const char* kSocketPath = "/tmp/fitom_x_midi.sock";
#endif

} // namespace

struct MidiInDevice {
    std::thread        acceptThread;
    std::vector<std::thread> workers; // 接続ごとの処理スレッド
    std::atomic<bool>  running { false };
    MidiInCallback     callback = nullptr;
    void*              userData = nullptr;

    // stateMutex が以下すべてを保護する:
    //   leasedChannels / activeHandles(Win) / activeFds(POSIX)
    std::mutex         stateMutex;
    std::bitset<kMaxClients> leasedChannels;
#if defined(_WIN32)
    HANDLE listenPipe = INVALID_HANDLE_VALUE; // 未使用(互換のため残置)
    std::vector<HANDLE> activeHandles;
#else
    int listenFd = -1;
    std::vector<int> activeFds;
#endif

    // 空きチャンネル(0-kMaxClients-1)を1つ確保する。無ければ-1。
    int acquireChannel() {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (int ch = 0; ch < kMaxClients; ++ch) {
            if (!leasedChannels.test(static_cast<size_t>(ch))) {
                leasedChannels.set(static_cast<size_t>(ch));
                return ch;
            }
        }
        return -1;
    }
    void releaseChannel(int ch) {
        if (ch < 0) return;
        std::lock_guard<std::mutex> lock(stateMutex);
        leasedChannels.reset(static_cast<size_t>(ch));
    }

    ~MidiInDevice() {
        running.store(false);
#if defined(_WIN32)
        // ConnectNamedPipe/ReadFileでブロックしている全スレッドを起こす
        // ため、生存中の全ハンドルを閉じる。以後、各スレッドが自分の
        // ハンドルをclaimHandleForClose()経由でクローズしようとしても
        // (既にここでリストから除去済みのため)falseが返り二重closeは
        // 起きない。
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            for (HANDLE h : activeHandles) CloseHandle(h);
            activeHandles.clear();
        }
#else
        // accept()/read()でブロックしている全スレッドを起こす。
        // shutdown()はfd番号自体は無効にしないため、直後の再accept()
        // によるfd番号の再利用と競合しない。
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            for (int fd : activeFds) shutdown(fd, SHUT_RDWR);
        }
        if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
#endif
        if (acceptThread.joinable()) acceptThread.join();
        // acceptThreadの終了後は新規ワーカーが増えないため、ここから先は
        // workersへの安全な走査・joinができる。
        for (auto& t : workers) if (t.joinable()) t.join();

#if !defined(_WIN32)
        // 上記でclose済みのはずだが、念のための安全網。
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            for (int fd : activeFds) ::close(fd);
            activeFds.clear();
        }
        if (listenFd >= 0) ::close(listenFd);
        ::unlink(kSocketPath);
#endif
    }
};

namespace {

// チャンネル割り当て通知(FITOM_X → クライアント、接続直後に1回のみ)。
// F0 00 48 01 03 <ch> F7 (マニュファクチャラID 00 48 01 は既存の
// HwPatch/SwPatchパラメータオーバーライドSysExと共通。sub-cmd 0x01/0x02
// は使用済みのため 0x03 を新設。詳細はdocs/plugin-midi-pipe.md参照)
void buildChannelAnnounce(uint8_t (&out)[7], int ch)
{
    out[0] = 0xF0; out[1] = 0x00; out[2] = 0x48; out[3] = 0x01;
    out[4] = 0x03; out[5] = static_cast<uint8_t>(ch); out[6] = 0xF7;
}

#if defined(_WIN32)

// hがactiveHandles内にまだ存在すれば除去してtrueを返す(=このスレッドが
// CloseHandleする責任を持つ)。既に無ければfalse(デストラクタ側が
// 既に処理済み/処理中のため、二重closeを避けて何もしない)。
bool claimHandleForClose(MidiInDevice* dev, HANDLE h)
{
    std::lock_guard<std::mutex> lock(dev->stateMutex);
    auto it = std::find(dev->activeHandles.begin(), dev->activeHandles.end(), h);
    if (it == dev->activeHandles.end()) return false;
    dev->activeHandles.erase(it);
    return true;
}

void connectionWorker(MidiInDevice* dev, HANDLE h, int ch)
{
    uint8_t announce[7];
    buildChannelAnnounce(announce, ch);
    DWORD written = 0;
    WriteFile(h, announce, sizeof(announce), &written, nullptr);

    uint8_t buf[4096];
    DWORD n = 0;
    while (dev->running.load() &&
           ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        dev->callback(buf, n, 0, dev->userData);
    }

    if (claimHandleForClose(dev, h)) {
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    dev->releaseChannel(ch);
}

// 新しいパイプインスタンスを作って接続を待つ→繋がったらチャンネルを
// 確保してワーカースレッドへ引き渡す→次のインスタンスへ、を繰り返す。
void acceptThreadFunc(MidiInDevice* dev)
{
    while (dev->running.load()) {
        HANDLE h = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_DUPLEX, // チャンネル割り当て通知の書き込みに必要
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, // 実際の上限はkMaxClients(下記)
            4096, 4096,
            0, nullptr);
        if (h == INVALID_HANDLE_VALUE) break;

        {
            std::lock_guard<std::mutex> lock(dev->stateMutex);
            dev->activeHandles.push_back(h);
        }

        BOOL connected = ConnectNamedPipe(h, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!dev->running.load() || !connected) {
            if (claimHandleForClose(dev, h)) {
                CloseHandle(h);
            }
            if (!dev->running.load()) break;
            continue;
        }

        int ch = dev->acquireChannel();
        if (ch < 0) {
            // 空きチャンネルなし(kMaxClients本すでに接続中): 即座に切る
            if (claimHandleForClose(dev, h)) {
                DisconnectNamedPipe(h);
                CloseHandle(h);
            }
            continue;
        }

        dev->workers.emplace_back(connectionWorker, dev, h, ch);
    }
}

#else // POSIX

bool claimFdForClose(MidiInDevice* dev, int fd)
{
    std::lock_guard<std::mutex> lock(dev->stateMutex);
    auto it = std::find(dev->activeFds.begin(), dev->activeFds.end(), fd);
    if (it == dev->activeFds.end()) return false;
    dev->activeFds.erase(it);
    return true;
}

void connectionWorker(MidiInDevice* dev, int fd, int ch)
{
    uint8_t announce[7];
    buildChannelAnnounce(announce, ch);
    ::write(fd, announce, sizeof(announce));

    uint8_t buf[4096];
    ssize_t n;
    while (dev->running.load() &&
           (n = ::read(fd, buf, sizeof(buf))) > 0) {
        dev->callback(buf, static_cast<size_t>(n), 0, dev->userData);
    }

    if (claimFdForClose(dev, fd)) {
        ::close(fd);
    }
    dev->releaseChannel(ch);
}

void midiInThread(MidiInDevice* dev)
{
    while (dev->running.load()) {
        sockaddr_un clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int fd = ::accept(dev->listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (fd < 0) {
            if (!dev->running.load()) break;
            continue; // shutdown()以外の理由でaccept失敗した場合は単に再試行
        }
        if (!dev->running.load()) { ::close(fd); break; }

        int ch = dev->acquireChannel();
        if (ch < 0) {
            ::close(fd); // 空きチャンネルなし(kMaxClients本すでに接続中)
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(dev->stateMutex);
            dev->activeFds.push_back(fd);
        }
        dev->workers.emplace_back(connectionWorker, dev, fd, ch);
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
    // MIDI Out(汎用送信)は現状未実装。チャンネル割り当て通知のみ、
    // 接続受け入れ側(MidiPlugin_OpenIn)が内部で直接書き込む。
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
        ::listen(dev->listenFd, kMaxClients) < 0) {
        ::close(dev->listenFd);
        delete dev;
        return MIDI_ERR_OPEN_FAILED;
    }
#endif

    dev->running.store(true);
#if defined(_WIN32)
    dev->acceptThread = std::thread(acceptThreadFunc, dev);
#else
    dev->acceptThread = std::thread(midiInThread, dev);
#endif

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
