// backends/midi_winmm/src/MidiWinMM.cpp
// クラシック WinMM API (winmm.lib) による薄いMIDIバックエンドDLL
//
// C++/WinRTを必要とする backends/midi_wms/ (Windows MIDI Services) とは
// 別物。ランタイムの追加インストールが不要で、Windows全バージョンで
// そのまま動作する、最小限の薄いラッパー実装。

#include <fitom/IMidiPlugin.h>
#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <string>
#include <vector>
#include <cstring>
#include <mutex>

#pragma comment(lib, "winmm.lib")

namespace {

// UINT (デバイスID) → デバイス名。列挙のたびに再取得する
// (デバイスの抜き差しで並び順が変わりうるため、都度取得が安全)。
std::vector<std::string> enumerateInNames() {
    std::vector<std::string> names;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            names.emplace_back(caps.szPname);
        }
    }
    return names;
}

std::vector<std::string> enumerateOutNames() {
    std::vector<std::string> names;
    UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSA caps{};
        if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            names.emplace_back(caps.szPname);
        }
    }
    return names;
}

// MIDIメッセージのステータスバイトから、そのメッセージの全体バイト数を返す
// (1〜3バイト)。リアルタイムメッセージ(0xF8以上)は1バイト、
// Program Change(0xC0)/Channel Pressure(0xD0)は2バイト、それ以外は3バイト。
size_t messageLength(uint8_t status) {
    if (status >= 0xF8) return 1;
    uint8_t hi = status & 0xF0;
    if (hi == 0xC0 || hi == 0xD0) return 2;
    return 3;
}

char* dupJsonArray(const std::vector<std::string>& names) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : names) arr.push_back(n);
    std::string s = arr.dump();
    char* buf = new char[s.size() + 1];
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

} // namespace

// ─── MIDI In ─────────────────────────────────────────────────────────────────

struct MidiInDevice {
    HMIDIIN        handle   = nullptr;
    MidiInCallback callback = nullptr;
    void*          userData = nullptr;
};

static void CALLBACK midiInProc(HMIDIIN /*hMidiIn*/, UINT wMsg,
                                 DWORD_PTR dwInstance, DWORD_PTR dwParam1,
                                 DWORD_PTR /*dwParam2*/)
{
    if (wMsg != MIM_DATA) return; // SysEx(MIM_LONGDATA)は今回未対応
    auto* dev = reinterpret_cast<MidiInDevice*>(dwInstance);
    if (!dev || !dev->callback) return;

    DWORD packed = static_cast<DWORD>(dwParam1);
    uint8_t status = static_cast<uint8_t>(packed & 0xFF);
    uint8_t buf[3] = {
        status,
        static_cast<uint8_t>((packed >> 8) & 0xFF),
        static_cast<uint8_t>((packed >> 16) & 0xFF)
    };
    size_t len = messageLength(status);
    dev->callback(buf, len, 0, dev->userData);
}

// ─── MIDI Out ────────────────────────────────────────────────────────────────

struct MidiOutDevice {
    HMIDIOUT handle = nullptr;
};

extern "C" {

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_GetName() {
    return "WinMM";
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateIn() {
    return dupJsonArray(enumerateInNames());
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateOut() {
    return dupJsonArray(enumerateOutNames());
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

    // 名前一致でデバイスIDを検索
    UINT n = midiInGetNumDevs();
    UINT targetId = static_cast<UINT>(-1);
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        if (std::string(caps.szPname) == device_name) { targetId = i; break; }
    }
    if (targetId == static_cast<UINT>(-1)) return MIDI_ERR_NOT_FOUND;

    auto* dev = new MidiInDevice();
    dev->callback = callback;
    dev->userData = user_data;

    MMRESULT mr = midiInOpen(&dev->handle, targetId,
        reinterpret_cast<DWORD_PTR>(&midiInProc),
        reinterpret_cast<DWORD_PTR>(dev),
        CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) {
        delete dev;
        return MIDI_ERR_OPEN_FAILED;
    }
    midiInStart(dev->handle);

    *out_handle = reinterpret_cast<MidiInHandle>(dev);
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseIn(MidiInHandle handle) {
    if (!handle) return;
    auto* dev = reinterpret_cast<MidiInDevice*>(handle);
    if (dev->handle) {
        midiInStop(dev->handle);
        midiInClose(dev->handle);
    }
    delete dev;
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenOut(
    const char* device_name, MidiOutHandle* out_handle)
{
    if (!device_name || !out_handle) return MIDI_ERR_UNAVAILABLE;

    UINT n = midiOutGetNumDevs();
    UINT targetId = static_cast<UINT>(-1);
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSA caps{};
        if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        if (std::string(caps.szPname) == device_name) { targetId = i; break; }
    }
    if (targetId == static_cast<UINT>(-1)) return MIDI_ERR_NOT_FOUND;

    auto* dev = new MidiOutDevice();
    MMRESULT mr = midiOutOpen(&dev->handle, targetId, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        delete dev;
        return MIDI_ERR_OPEN_FAILED;
    }

    *out_handle = reinterpret_cast<MidiOutHandle>(dev);
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseOut(MidiOutHandle handle) {
    if (!handle) return;
    auto* dev = reinterpret_cast<MidiOutDevice*>(handle);
    if (dev->handle) midiOutClose(dev->handle);
    delete dev;
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_Send(
    MidiOutHandle handle, const uint8_t* data, size_t len, uint64_t /*timestamp_ns*/)
{
    // WinMMのmidiOutShortMsgは3バイト以内の通常メッセージのみ対応。
    // SysEx(0xF0開始)はmidiOutLongMsgが必要だが今回は未対応。
    if (!handle || !data || len == 0 || len > 3) return MIDI_ERR_IO;
    auto* dev = reinterpret_cast<MidiOutDevice*>(handle);

    DWORD packed = data[0];
    if (len > 1) packed |= (static_cast<DWORD>(data[1]) << 8);
    if (len > 2) packed |= (static_cast<DWORD>(data[2]) << 16);

    return (midiOutShortMsg(dev->handle, packed) == MMSYSERR_NOERROR)
        ? MIDI_OK : MIDI_ERR_IO;
}

} // extern "C"
