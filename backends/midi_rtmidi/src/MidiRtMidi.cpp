// backends/midi_rtmidi/src/MidiRtMidi.cpp
// RtMidi (https://github.com/thestk/rtmidi) による
// クロスプラットフォーム(Windows/Linux/macOS)MIDIバックエンドDLL。
// 旧 backends/midi_wms / midi_winmm / midi_alsa の3実装を本ファイル1つに
// 統合する(旧3実装はいずれもSysEx未対応だったが、RtMidiではSysExも
// 通常メッセージと同じAPIで透過的に送受信できる)。

#include <fitom/IMidiPlugin.h>
#include <nlohmann/json.hpp>
#include <RtMidi.h>

#include <string>
#include <vector>
#include <memory>
#include <cstring>

namespace {

// RtMidiIn/RtMidiOut どちらでも使える汎用ポート列挙。
// RtMidiコンストラクタは、対応するMIDI APIが1つも利用できない環境では
// RtMidiErrorを送出しうる。実MIDIデバイスの無い検証環境でも
// EnumerateIn/Outがクラッシュしないよう、例外はここで飲み込んで
// 空配列を返す。
template <typename RtT>
std::vector<std::string> enumeratePortNames() {
    std::vector<std::string> names;
    try {
        RtT rt;
        unsigned int n = rt.getPortCount();
        names.reserve(n);
        for (unsigned int i = 0; i < n; ++i) {
            try {
                names.push_back(rt.getPortName(i));
            } catch (RtMidiError&) {
                // 個別ポート名の取得失敗はスキップ(列挙全体は継続する)
            }
        }
    } catch (RtMidiError&) {
        // MIDIサブシステム自体が使えない環境 → 空配列を返す
    }
    return names;
}

char* dupJsonArray(const std::vector<std::string>& names) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : names) arr.push_back(n);
    std::string s = arr.dump();
    char* buf = new char[s.size() + 1];
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

// device_name と一致するポート番号を検索する。
// ポート番号はデバイスの抜き差しで変わりうるため、Open毎に毎回名前で
// 再検索する(旧WinMM/ALSA実装と同じパターン)。同名デバイスが複数
// 存在する場合は最初に一致したものを採用する(曖昧さは旧実装と同じ
// 制約であり後退ではない)。
template <typename RtT>
bool findPortByName(RtT& rt, const std::string& name, unsigned int& outPort) {
    unsigned int n = rt.getPortCount();
    for (unsigned int i = 0; i < n; ++i) {
        try {
            if (rt.getPortName(i) == name) { outPort = i; return true; }
        } catch (RtMidiError&) {
            continue;
        }
    }
    return false;
}

} // namespace

// ─── MIDI In ─────────────────────────────────────────────────────────────────

struct MidiInDevice {
    std::unique_ptr<RtMidiIn> rt;
    MidiInCallback            callback = nullptr;
    void*                     userData = nullptr;
};

// RtMidiのコールバックシグネチャ: void(double timeStamp,
// std::vector<unsigned char>* message, void* userData)。timeStampは
// 「前回受信イベントからの相対デルタ秒」であり、IMidiPlugin.hが要求する
// 「絶対受信タイムスタンプ(不明なら0)」ではないため変換すると誤った
// 値になる。既存WinMM/ALSA実装も常に0を渡しており後退ではない。
static void rtMidiInTrampoline(double /*timeStamp*/,
                                std::vector<unsigned char>* message,
                                void* userData)
{
    auto* dev = static_cast<MidiInDevice*>(userData);
    if (!dev || !dev->callback || !message || message->empty()) return;
    dev->callback(message->data(), message->size(), 0, dev->userData);
}

// ─── MIDI Out ────────────────────────────────────────────────────────────────

struct MidiOutDevice {
    std::unique_ptr<RtMidiOut> rt;
};

extern "C" {

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_GetName() {
    return "RtMidi";
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateIn() {
    return dupJsonArray(enumeratePortNames<RtMidiIn>());
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateOut() {
    return dupJsonArray(enumeratePortNames<RtMidiOut>());
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
    if (!device_name || !callback || !out_handle) return MIDI_ERR_UNAVAILABLE;

    auto dev = std::make_unique<MidiInDevice>();
    try {
        dev->rt = std::make_unique<RtMidiIn>();
    } catch (RtMidiError&) {
        return MIDI_ERR_UNAVAILABLE; // MIDIサブシステム自体が使えない
    }

    unsigned int port = 0;
    if (!findPortByName(*dev->rt, device_name, port)) return MIDI_ERR_NOT_FOUND;

    dev->callback = callback;
    dev->userData = user_data;

    try {
        dev->rt->openPort(port, "FITOM In");
        dev->rt->setCallback(&rtMidiInTrampoline, dev.get());
        // SysExは通す(false)。Timing Clock / Active Sensingは無視する
        // (既存WinMM/ALSA実装がこれらのリアルタイムメッセージに対応
        //  していないのと同じ挙動であり、後退ではない)。
        dev->rt->ignoreTypes(/*midiSysex=*/false,
                              /*midiTime=*/true,
                              /*midiSense=*/true);
    } catch (RtMidiError&) {
        return MIDI_ERR_OPEN_FAILED;
    }

    *out_handle = reinterpret_cast<MidiInHandle>(dev.release());
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseIn(MidiInHandle handle) {
    if (!handle) return;
    auto* dev = reinterpret_cast<MidiInDevice*>(handle);
    if (dev->rt) {
        // 明示的にコールバック解除→ポート解放してから破棄する
        // (解放中のdevをコールバックが参照し続ける競合の芽を断つ)。
        try { dev->rt->cancelCallback(); } catch (RtMidiError&) {}
        try { dev->rt->closePort(); }      catch (RtMidiError&) {}
    }
    delete dev;
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenOut(
    const char* device_name, MidiOutHandle* out_handle)
{
    if (!device_name || !out_handle) return MIDI_ERR_UNAVAILABLE;

    auto dev = std::make_unique<MidiOutDevice>();
    try {
        dev->rt = std::make_unique<RtMidiOut>();
    } catch (RtMidiError&) {
        return MIDI_ERR_UNAVAILABLE;
    }

    unsigned int port = 0;
    if (!findPortByName(*dev->rt, device_name, port)) return MIDI_ERR_NOT_FOUND;

    try {
        dev->rt->openPort(port, "FITOM Out");
    } catch (RtMidiError&) {
        return MIDI_ERR_OPEN_FAILED;
    }

    *out_handle = reinterpret_cast<MidiOutHandle>(dev.release());
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseOut(MidiOutHandle handle) {
    if (!handle) return;
    auto* dev = reinterpret_cast<MidiOutDevice*>(handle);
    if (dev->rt) {
        try { dev->rt->closePort(); } catch (RtMidiError&) {}
    }
    delete dev;
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_Send(
    MidiOutHandle handle, const uint8_t* data, size_t len, uint64_t /*timestamp_ns*/)
{
    // RtMidiのsendMessage()は即時送信のみでスケジュール送信機構を
    // 持たない。timestamp_ns は無視して即時送信する(旧WMS実装の
    // 「スケジュール送信」もタイムスタンプをメタデータに埋め込むだけで
    // 実質即時送信だったため後退ではない)。
    if (!handle || !data || len == 0) return MIDI_ERR_IO;
    auto* dev = reinterpret_cast<MidiOutDevice*>(handle);
    if (!dev->rt) return MIDI_ERR_IO;

    try {
        dev->rt->sendMessage(data, len); // SysEx(可変長)も同一APIで送信可能
    } catch (RtMidiError&) {
        return MIDI_ERR_IO;
    }
    return MIDI_OK;
}

} // extern "C"
