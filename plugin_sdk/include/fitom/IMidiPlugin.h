#pragma once
// plugin_sdk/include/fitom/IMidiPlugin.h
//
// MIDI バックエンド DLL が実装・エクスポートする C API。
//
// ─── 設計原則 ────────────────────────────────────────────────────────────────
//   WinMM / Windows MIDI Services / ALSA / CoreMIDI 等の差異を完全に隠蔽する。
//   MIDI メッセージは生バイト列 (MIDI 1.0 形式) でやり取りする。
//   Windows MIDI Services の UMP (Universal MIDI Packet) は DLL 側で変換する。
//
// ─── Windows MIDI Services の注意点 ─────────────────────────────────────────
//   WMS SDK は C++/WinRT が必要なため、ビルドは backends/midi_wms/ で完結させ、
//   コア側には一切の WinRT 依存を持ち込まない。
//   WMS バックエンド DLL は 64bit のみ・Runtime 要インストール。
//   Runtime 未インストール環境では HWPlugin_Open が HW_ERR_NOT_FOUND を返す。
//
// ─── コールバック ────────────────────────────────────────────────────────────
//   受信データはコールバック関数で通知する (ポーリングモデルは提供しない)。
//   コールバックは MidiPlugin 内部スレッドから呼ばれるため、
//   コールバック関数はできるだけ短時間で返ること。

#include <cstdint>
#include <stddef.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef FITOM_MIDI_PLUGIN_EXPORTS
#    define FITOM_MIDIP_API __declspec(dllexport)
#  else
#    define FITOM_MIDIP_API __declspec(dllimport)
#  endif
#  define FITOM_MIDIP_CALL __cdecl
#else
#  if defined(FITOM_MIDI_PLUGIN_EXPORTS) && defined(__GNUC__)
#    define FITOM_MIDIP_API __attribute__((visibility("default")))
#  else
#    define FITOM_MIDIP_API
#  endif
#  define FITOM_MIDIP_CALL
#endif

typedef enum MidiResult {
    MIDI_OK              =  0,
    MIDI_ERR_NOT_FOUND   = -1,
    MIDI_ERR_OPEN_FAILED = -2,
    MIDI_ERR_IO          = -3,
    MIDI_ERR_UNAVAILABLE = -4,  // WMS Runtime 未インストール等
} MidiResult;

// MIDI デバイスの不透明ハンドル
struct MidiInOpaque;
struct MidiOutOpaque;
typedef struct MidiInOpaque*  MidiInHandle;
typedef struct MidiOutOpaque* MidiOutHandle;

// 受信コールバック
// data: MIDI 1.0 バイト列 (status + data bytes)
// len : バイト数
// timestamp_ns: 受信タイムスタンプ [ナノ秒]。不明な場合は 0。
// user_data: MidiPlugin_OpenIn に渡した値
typedef void (FITOM_MIDIP_CALL *MidiInCallback)(
    const uint8_t* data, size_t len,
    uint64_t timestamp_ns, void* user_data);

#ifdef __cplusplus
extern "C" {
#endif

// ─── プラグイン情報 ──────────────────────────────────────────────────────────
// "WinMM", "WindowsMIDIServices", "ALSA", "CoreMIDI" 等
FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_GetName();

// ─── デバイス列挙 ────────────────────────────────────────────────────────────
// 利用可能な MIDI In / Out デバイス名の JSON 配列を返す
// 例: ["MIDI キーボード", "Microsoft GS Wavetable Synth"]
// 呼び出し元は MidiPlugin_FreeString で解放する
FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateIn();
FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateOut();
FITOM_MIDIP_API void        FITOM_MIDIP_CALL MidiPlugin_FreeString(const char* str);

// ─── MIDI In ─────────────────────────────────────────────────────────────────
// device_name: MidiPlugin_EnumerateIn() で得た名前
// callback: 受信コールバック (非 null)
// user_data: コールバックに渡す任意のポインタ
FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenIn(
    const char* device_name,
    MidiInCallback callback,
    void* user_data,
    MidiInHandle* out_handle);

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseIn(MidiInHandle handle);

// ─── MIDI Out ────────────────────────────────────────────────────────────────
FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenOut(
    const char* device_name,
    MidiOutHandle* out_handle);

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseOut(MidiOutHandle handle);

// data: MIDI 1.0 バイト列
// timestamp_ns: 送信予約タイムスタンプ [ナノ秒]。0 = 即時送信。
//               WMS 以外のバックエンドでは 0 として扱う。
FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_Send(
    MidiOutHandle handle,
    const uint8_t* data, size_t len,
    uint64_t timestamp_ns);

#ifdef __cplusplus
}
#endif
