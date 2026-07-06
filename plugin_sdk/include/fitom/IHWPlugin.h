#pragma once
// plugin_sdk/include/fitom/IHWPlugin.h
//
// ハードウェア I/F バックエンド DLL が実装・エクスポートする C API。
//
// ─── 設計原則 ────────────────────────────────────────────────────────────────
//   hw::HWControllerBase の write / reset / isOpen を C 関数にフラット化。
//   DLL は不透明ハンドル (HWHandle) を管理し、FITOM コアはその値を保持するだけ。
//   FitomIFTest 側でこの C API を実装した共有ライブラリ (fitom_hw.dll / .so) を
//   ビルドすることで FITOM コアと分離できる。
//
// ─── デバイス列挙 ────────────────────────────────────────────────────────────
//   HWPlugin_Enumerate で接続済みデバイスの JSON 文字列を返す。
//   フォーマット:
//     [
//       { "type": "RE1", "serial": "ABCD1234", "index": 0 },
//       { "type": "SPFM_TOWER", "port": "COM3", "index": 0 }
//     ]
//
// ─── アドレス変換規則 ─────────────────────────────────────────────────────────
//   HWPlugin_Write(handle, addr, data):
//     addr 上位 8bit → a_high (SPFM 拡張アドレス)
//     addr 下位 8bit → addr (& ADDR_MASK)
//   これは IPort::write() と同じ慣習。

#include <cstdint>
#include <stddef.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef FITOM_HW_PLUGIN_EXPORTS
#    define FITOM_HWP_API __declspec(dllexport)
#  else
#    define FITOM_HWP_API __declspec(dllimport)
#  endif
#  define FITOM_HWP_CALL __cdecl
#else
#  if defined(FITOM_HW_PLUGIN_EXPORTS) && defined(__GNUC__)
#    define FITOM_HWP_API __attribute__((visibility("default")))
#  else
#    define FITOM_HWP_API
#  endif
#  define FITOM_HWP_CALL
#endif

typedef enum HWResult {
    HW_OK              =  0,
    HW_ERR_NOT_FOUND   = -1,
    HW_ERR_OPEN_FAILED = -2,
    HW_ERR_IO          = -3,
    HW_ERR_INVALID_ARG = -4,
} HWResult;

struct HWDeviceOpaque;
typedef struct HWDeviceOpaque* HWHandle;

#ifdef __cplusplus
extern "C" {
#endif

// ─── プラグイン情報 ──────────────────────────────────────────────────────────
// プラグイン名を返す ("FitomIFTest", "DummyHW" 等)
FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetName();

// ─── プロファイル設定 (Optional) ────────────────────────────────────────────
// アプリケーションが明示的にプロファイルのファイルパスを設定する。
// HWPlugin_Open()より前に呼ぶこと。
//
// [オプション関数] 多くのプラグインは、環境変数(プラグインのドキュメント
// に従う。例: FitomEmuIFならFMHWIF_PROFILE) 経由でDLLロード時点(静的
// シングルトン初期化)に自身のプロファイルを自動読み込みする設計になって
// いる。この関数は、それに加えて(または代わりに)、ホスト側が明示的な
// API呼び出しでプロファイルを指定できるようにするためのもの。
//
// 未実装のプラグイン(旧バージョン)ではこのシンボル自体が存在しない。
// ホスト側は動的ロード時にシンボルの有無を確認し(GetProcAddress/dlsym
// がnullptrを返す)、存在しない場合は環境変数経由の設定にフォールバック
// すること。
//
// 実装側の注意: DLLロード時点で既に静的初期化によりプロファイルを
// 読み込み済みの設計であっても、この関数が呼ばれた場合は指定された
// パスで再読み込み・再初期化すること (呼び出しタイミングに依存しない
// 動作を保証する)。
//
// 戻り値: 成功時 HW_OK、失敗時エラーコード(HW_ERR_NOT_FOUND等)。
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_SetProfile(const char* path);

// ─── デバイス列挙 ────────────────────────────────────────────────────────────
// 接続デバイスを JSON 文字列で返す (呼び出し元は HWPlugin_FreeString で解放)
// 失敗時は nullptr
FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_Enumerate();
FITOM_HWP_API void        FITOM_HWP_CALL HWPlugin_FreeString(const char* str);

// ─── デバイス開閉 ────────────────────────────────────────────────────────────
// params_json: { "type":"RE1", "serial":"ABCD1234", "slot":0, "clock":3579545, "pan":0 }
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Open(
    const char* params_json, HWHandle* out_handle);
FITOM_HWP_API void     FITOM_HWP_CALL HWPlugin_Close(HWHandle handle);

// ─── I/O ─────────────────────────────────────────────────────────────────────
// addr 上位 8bit = a_high (SPFM 拡張アドレス)、下位 8bit = レジスタアドレス
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Write(
    HWHandle handle, uint16_t addr, uint8_t data);

// バースト書き込み (startAddr は下位 8bit のみ有効)
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_WriteBlock(
    HWHandle handle, uint8_t startAddr, const uint8_t* data, size_t len);

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Reset(HWHandle handle, unsigned int pulse_us);

// ─── メタ情報 ────────────────────────────────────────────────────────────────
FITOM_HWP_API int  FITOM_HWP_CALL HWPlugin_GetClock(HWHandle handle);
FITOM_HWP_API int  FITOM_HWP_CALL HWPlugin_GetPanpot(HWHandle handle);
FITOM_HWP_API bool FITOM_HWP_CALL HWPlugin_IsOpen(HWHandle handle);

// ─── レイテンシ同期 ──────────────────────────────────────────────────────────
// HWPlugin_GetLatencySamples:
//   このデバイスが write() から実際の発音まで要するサンプル数を返す。
//   物理チップ (SPFM 等) は 0 を返す。
//   FM エンジン内蔵 hwif は (buffer_frames) を返す。
//   FITOM コアはこの値を全デバイス間で収集し最大値を基準レイテンシとする。
FITOM_HWP_API uint32_t FITOM_HWP_CALL HWPlugin_GetLatencySamples(HWHandle handle);

// HWPlugin_SetDelaySamples:
//   FITOM コアが全デバイスの基準レイテンシを設定する。
//   物理チップはこの値だけ write() をキューイングして遅らせる。
//   FM エンジン内蔵 hwif は自身のレイテンシと一致するため何もしなくてよい。
//   delay_samples == 0 の場合は遅延なし (単デバイス構成向け)。
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SetDelaySamples(
    HWHandle handle, uint32_t delay_samples);

#ifdef __cplusplus
}
#endif
