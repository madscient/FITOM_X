#pragma once
// plugin_sdk/include/fitom/IFmEnginePlugin.h
//
// FM エンジン (エミュレーター) バックエンド DLL が実装・エクスポートする C API。
//
// ─── 設計原則 ────────────────────────────────────────────────────────────────
//   既存 FmEngineApi.h をそのままラップする。
//   FmEngine_Create / Destroy / AddChip / Write / Generate 等は変更しない。
//   FITOM コアは DLL を実行時ロード (LoadLibrary / dlopen) するため、
//   ビルド時のリンク依存はヘッダのみ。
//
// ─── 複数 DLL の同時使用 ────────────────────────────────────────────────────
//   config.json に複数の FmEngine エントリを記述することで、
//   YMEngine.dll (OPN/OPM 系) と AYEngine.dll (PSG 系) を同時にロードできる。
//   FmEngineLoader がエンジン名 → DLL パスのマッピングを管理する。
//
// ─── 既存 FmEngineApi.h との関係 ─────────────────────────────────────────────
//   このヘッダは FmEngineApi.h のシンボルを再利用する (重複定義なし)。
//   FmEngineApi.h を include した後にこのヘッダを include すること。

#include <cstdint>

// ---- エクスポート属性 (FmEngineApi.h と同じ規則) ---------------------------
#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef FITOM_FMENGINE_PLUGIN_EXPORTS
#    define FITOM_FMEP_API __declspec(dllexport)
#  else
#    define FITOM_FMEP_API __declspec(dllimport)
#  endif
#  define FITOM_FMEP_CALL __cdecl
#else
#  if defined(FITOM_FMENGINE_PLUGIN_EXPORTS) && defined(__GNUC__)
#    define FITOM_FMEP_API __attribute__((visibility("default")))
#  else
#    define FITOM_FMEP_API
#  endif
#  define FITOM_FMEP_CALL
#endif

// ---- FITOM が追加で期待するエクスポート関数 ---------------------------------
// 既存 FmEngineApi.h の関数群 (FmEngine_Create 等) に加えて、
// FITOM コアは以下の 1 関数を追加で要求する。
// FmEngineApi.h 側で変更がない実装は、この関数を追加するだけで対応できる。

#ifdef __cplusplus
extern "C" {
#endif

// エンジンの識別名を返す ("YMEngine", "AYEngine" 等)
// FmEngineLoader がエンジンを区別するために使用する。
FITOM_FMEP_API const char* FITOM_FMEP_CALL FmEngine_GetEngineName();

#ifdef __cplusplus
}
#endif
