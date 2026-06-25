#pragma once
// fitom/fitom_core.h
// チップドライバ・コアモジュール共通インクルード
// 旧 stdafx.h の代替。プラットフォーム依存コードを含まない。

// ─── 標準ライブラリ ───────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cassert>

// ─── プリミティブ型の互換定義 ────────────────────────────────────────────────
// Windows BOOL / BYTE / WORD / DWORD の代替
// 旧コードとの互換のため残す。新コードでは std 型を使うこと。
#ifndef _WIN32
using BOOL   = bool;
using BYTE   = uint8_t;
using WORD   = uint16_t;
using DWORD  = uint32_t;
#  define FALSE false
#  define TRUE  true
#  define ZeroMemory(d, l) std::memset(d, 0, l)
#endif

// TCHAR は廃止方向だが移行期に残す
// 内部文字列は全て UTF-8 std::string で扱う
#ifndef TCHAR
using TCHAR   = char;
using LPCTSTR = const char*;
using LPTSTR  = char*;
#  define _T(x) x
#  define tcslen  strlen
#  define tcsncpy strncpy
#  define tcscpy  strcpy
#  define tcscmp  strcmp
#  define tcscmpn strncmp
#  define tstring std::string
// StringCchPrintf 互換マクロ (Windows API の代替)
#  include <cstdarg>
inline void StringCchPrintf(char* dst, size_t dstLen, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(dst, dstLen, fmt, args);
    va_end(args);
}
#else
#  include <strsafe.h>
#endif

// ─── FITOM コアヘッダ ────────────────────────────────────────────────────────
#include "fitom/Log.h"
#include "fitom/FITOMdefine.h"
#include "fitom/VoiceData.h"
#include "fitom/VolumeUtils.h"   // CalcLinearLevel / Linear2dB / ROM::VolCurveLin
#include "fitom/FnumUtils.h"     // FnumRegistry
