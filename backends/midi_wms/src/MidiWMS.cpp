// backends/midi_wms/src/MidiWMS.cpp
// Windows MIDI Services バックエンド DLL 実装
//
// ─── ビルド要件 ──────────────────────────────────────────────────────────────
//   - Visual Studio 2022 + Windows SDK 10.0.20348+
//   - NuGet: Microsoft.Windows.Devices.Midi2 (GitHub からローカルフィード経由)
//   - NuGet: Microsoft.Windows.CppWinRT
//   - cppwinrt.exe でプロジェクション生成済み
//   - 64bit のみ
//
// ─── 実行時要件 ─────────────────────────────────────────────────────────────
//   Windows MIDI Services Runtime のインストールが必要。
//   未インストール環境では MidiPlugin_OpenIn / OpenOut が MIDI_ERR_UNAVAILABLE を返す。
//
// ─── UMP → MIDI 1.0 変換 ─────────────────────────────────────────────────────
//   WMS は全メッセージを UMP (Universal MIDI Packet) で扱う。
//   MIDI 1.0 デバイスから来る UMP32 (Type 2) を MIDI 1.0 バイト列に変換して
//   コールバックに渡す。コアは MIDI 1.0 バイト列のみ扱う。

// C++/WinRT 初期化 (最初にインクルード)
#include <winrt/base.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Initialization.h>
#include <winrt/Microsoft.Windows.Devices.Midi2.Messages.h>

// WMS SDK の初期化ヘルパー
#include <Microsoft.Windows.Devices.Midi2.Initialization.Bootstrapper.h>

#include <fitom/IMidiPlugin.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <cstring>

using namespace winrt;
using namespace winrt::Microsoft::Windows::Devices::Midi2;
using namespace winrt::Microsoft::Windows::Devices::Midi2::Initialization;

// ─── WMS 初期化状態 ──────────────────────────────────────────────────────────

static bool                   g_wmsAvailable = false;
static std::once_flag         g_initOnce;
static MidiSession            g_session{nullptr};

static void ensureWmsInitialized() {
    std::call_once(g_initOnce, []() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            // SDK Runtime の存在確認
            auto bootstrapper = MidiServicesInitializer::InitializeSdkRuntime();
            if (!bootstrapper) return;
            // セッション生成
            g_session = MidiSession::Create(L"FITOM");
            g_wmsAvailable = (g_session != nullptr);
        } catch (...) {
            g_wmsAvailable = false;
        }
    });
}

// ─── UMP32 → MIDI 1.0 バイト列変換 ──────────────────────────────────────────

static std::vector<uint8_t> umpToMidi1(uint32_t ump32)
{
    // UMP Type 2 (MIDI 1.0 Channel Voice) のみ対応
    // bit31-28: Message Type (2 = MIDI 1.0 CV)
    // bit27-24: Group
    // bit23-16: Status byte
    // bit15- 8: Data byte 1
    // bit 7- 0: Data byte 2
    uint8_t msgType = static_cast<uint8_t>((ump32 >> 28) & 0x0F);
    if (msgType != 0x02 && msgType != 0x01) return {};  // Type 1=System, 2=MIDI1 CV

    uint8_t status = static_cast<uint8_t>((ump32 >> 16) & 0xFF);
    uint8_t d1     = static_cast<uint8_t>((ump32 >>  8) & 0xFF);
    uint8_t d2     = static_cast<uint8_t>((ump32 >>  0) & 0xFF);

    // System messages (Type 1): status のみの場合を考慮
    if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
        return {status, d1};      // 2バイトメッセージ
    }
    if (status == 0xF1 || status == 0xF3) {
        return {status, d1};
    }
    if (status >= 0xF4) {
        return {status};          // 1バイトシステムメッセージ
    }
    return {status, d1, d2};     // 通常の3バイトメッセージ
}

// ─── MIDI In デバイス ─────────────────────────────────────────────────────────

struct MidiInDevice {
    MidiEndpointConnection  connection{nullptr};
    winrt::event_token      messageToken{};
    MidiInCallback          callback = nullptr;
    void*                   userData = nullptr;
};

// ─── エクスポート関数 ─────────────────────────────────────────────────────────

extern "C" {

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_GetName() {
    return "WindowsMIDIServices";
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateIn() {
    ensureWmsInitialized();
    if (!g_wmsAvailable) return nullptr;

    try {
        auto endpoints = MidiEndpointDeviceInformation::FindAll(
            MidiEndpointDeviceInformationSortOrder::Name,
            MidiEndpointDeviceInformationFilters::AllStandardEndpoints);

        nlohmann::json arr = nlohmann::json::array();
        for (auto& ep : endpoints) {
            arr.push_back(winrt::to_string(ep.Name()));
        }
        char* buf = new char[arr.dump().size() + 1];
        strcpy(buf, arr.dump().c_str());
        return buf;
    } catch (...) {
        return nullptr;
    }
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateOut() {
    // WMS は双方向エンドポイントのため In と同一
    return MidiPlugin_EnumerateIn();
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_FreeString(const char* str) {
    delete[] str;
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenIn(
    const char*    device_name,
    MidiInCallback callback,
    void*          user_data,
    MidiInHandle*  out_handle)
{
    ensureWmsInitialized();
    if (!g_wmsAvailable) return MIDI_ERR_UNAVAILABLE;
    if (!callback || !out_handle) return MIDI_ERR_UNAVAILABLE;

    try {
        // デバイス名でエンドポイントを検索
        auto endpoints = MidiEndpointDeviceInformation::FindAll(
            MidiEndpointDeviceInformationSortOrder::Name,
            MidiEndpointDeviceInformationFilters::AllStandardEndpoints);

        winrt::hstring targetId;
        for (auto& ep : endpoints) {
            if (winrt::to_string(ep.Name()) == device_name) {
                targetId = ep.EndpointDeviceId();
                break;
            }
        }
        if (targetId.empty()) return MIDI_ERR_NOT_FOUND;

        auto* dev = new MidiInDevice();
        dev->callback = callback;
        dev->userData = user_data;

        // 接続を開く
        dev->connection = g_session.CreateEndpointConnection(targetId);
        if (!dev->connection) { delete dev; return MIDI_ERR_OPEN_FAILED; }

        // メッセージ受信イベント登録
        dev->messageToken = dev->connection.MessageReceived(
            [dev](MidiEndpointConnection const&, MidiMessageReceivedEventArgs const& args) {
                auto msg = args.GetMessagePacket();
                // UMP32 として取り出し MIDI 1.0 に変換
                if (auto ump32 = msg.try_as<MidiMessage32>()) {
                    auto bytes = umpToMidi1(ump32.Word0());
                    if (!bytes.empty()) {
                        uint64_t ts = static_cast<uint64_t>(args.Timestamp());
                        dev->callback(bytes.data(), bytes.size(), ts, dev->userData);
                    }
                }
            });

        dev->connection.Open();
        *out_handle = reinterpret_cast<MidiInHandle>(dev);
        return MIDI_OK;
    } catch (...) {
        return MIDI_ERR_OPEN_FAILED;
    }
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseIn(MidiInHandle handle) {
    if (!handle) return;
    auto* dev = reinterpret_cast<MidiInDevice*>(handle);
    if (dev->connection) {
        dev->connection.MessageReceived(dev->messageToken);
        g_session.DisconnectEndpointConnection(dev->connection.ConnectionId());
    }
    delete dev;
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenOut(
    const char*     device_name,
    MidiOutHandle*  out_handle)
{
    // WMS は双方向のため OpenIn と同じエンドポイントを使う
    // ここでは送信専用ラッパーとして同じ接続を再利用する簡略実装
    ensureWmsInitialized();
    if (!g_wmsAvailable || !out_handle) return MIDI_ERR_UNAVAILABLE;

    MidiInHandle inHandle = nullptr;
    auto r = MidiPlugin_OpenIn(device_name, [](auto,auto,auto,auto){}, nullptr, &inHandle);
    if (r != MIDI_OK) return r;
    *out_handle = reinterpret_cast<MidiOutHandle>(inHandle);
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseOut(MidiOutHandle handle) {
    MidiPlugin_CloseIn(reinterpret_cast<MidiInHandle>(handle));
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_Send(
    MidiOutHandle  handle,
    const uint8_t* data, size_t len,
    uint64_t       timestamp_ns)
{
    if (!handle || !data || len == 0) return MIDI_ERR_INVALID_ARG;
    auto* dev = reinterpret_cast<MidiInDevice*>(reinterpret_cast<MidiInHandle>(handle));
    if (!dev->connection) return MIDI_ERR_IO;

    try {
        // MIDI 1.0 バイト列 → UMP32 に変換して送信
        // 最大 3 バイト → UMP Type 2 (MIDI 1.0 CV)
        if (len < 1 || len > 3) return MIDI_ERR_INVALID_ARG;

        uint8_t status = data[0];
        uint8_t d1 = (len > 1) ? data[1] : 0;
        uint8_t d2 = (len > 2) ? data[2] : 0;

        uint32_t ump = (0x20000000u)
                     | (static_cast<uint32_t>(status) << 16)
                     | (static_cast<uint32_t>(d1)     <<  8)
                     | (static_cast<uint32_t>(d2));

        auto msg = MidiMessage32(
            static_cast<winrt::Windows::Foundation::TimeSpan>(
                std::chrono::nanoseconds(timestamp_ns)),
            ump);

        dev->connection.SendSingleMessagePacket(msg);
        return MIDI_OK;
    } catch (...) {
        return MIDI_ERR_IO;
    }
}

} // extern "C"
