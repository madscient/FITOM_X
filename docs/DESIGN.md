# FITOM リファクタリング設計書 v2

## 基本方針の更新

| 項目 | 方針 |
|---|---|
| Boost 除去範囲 | `BOOST_FOREACH` / `boost::filesystem` / `boost::thread` → std 代替。`boost::log` / `boost::format` / `boost::asio` / `boost::interprocess` は**そのまま残す** |
| ログ出力 | **Boost.Log** を採用。`fitom/Log.h` マクロで全翻訳単位から透過的に使う |
| バックエンド方式 | FM エンジン・HW I/F・MIDI を**すべて DLL プラグイン方式**に統一 |
| Windows MIDI | **Windows MIDI Services** (C++/WinRT)。WMS バックエンド DLL に完全隔離 |
| Linux MIDI | **ALSA シーケンサ** |

---

## Boost 依存の最終整理

```
残す:
  boost::asio          → SPFMController.hpp の依存 (除去不可)
  boost::log           → ロギング (FITOM 全体で採用)
  boost::format        → 書式化 (C++20 std::format 移行は任意)
  boost::interprocess  → named_mutex (プロセス間排他)

std で置き換える:
  BOOST_FOREACH        → 範囲 for
  boost::filesystem    → std::filesystem (C++17)
  boost::thread        → std::thread / std::mutex
  boost::algorithm::string → std::string + <algorithm>
  boost::property_tree → nlohmann/json + 薄い INI ラッパー
```

---

## アーキテクチャ

```
┌───────────────────────────────────────────────────────────────────┐
│                     GUI / アプリケーション層                        │
│           MFC shim / Qt / CLI                                     │
└──────────────────────────────┬────────────────────────────────────┘
                               │ C API (fitom_api.h) ← 将来
┌──────────────────────────────▼────────────────────────────────────┐
│                  fitom_core  (static lib)                         │
│  CFITOM / CMidiInst / FITOMConfig / CSoundDevice / ...           │
│                                                                   │
│  ┌──────────────┐  ┌─────────────────┐  ┌────────────────────┐  │
│  │  IPort       │  │ IMidiPlugin C-API│  │  Boost.Log         │  │
│  │  HWPort      │  │ (DLL 経由で呼ぶ) │  │  (fitom/Log.h)     │  │
│  └──────┬───────┘  └────────┬────────┘  └────────────────────┘  │
└─────────┼───────────────────┼────────────────────────────────────┘
          │ PluginLoader       │ PluginLoader
          │ (dlopen/LoadLibrary)│
  ┌───────▼───────────────┐   ┌───────▼────────────────────────┐
  │  HW バックエンド        │   │  MIDI バックエンド               │
  │  (実機/エミュレータ共通、│   │  fitom_midi_wms.dll (Win)       │
  │   FITOM本体は区別しない)│   │  fitom_midi_winmm.dll (Win)     │
  │  (IHWPlugin)          │   │  fitom_midi_alsa.so (Linux)     │
  │                       │   │  (IMidiPlugin)                  │
  │  FitomIFTest を DLL化  │   └─────────────────────────────────┘
  │  (実機)                │
  │  FitomEmuIF            │
  │  (FmEngine統合エミュレータ、│
  │   RtAudio内蔵)          │
  └───────────────────────┘
```

---

## プラグイン SDK (plugin_sdk/include/fitom/)

### IHWPlugin.h
FitomIFTest の `hw::HWControllerBase` を C 関数にフラット化した API。
FitomIFTest 側でこの API を実装した共有ライブラリを追加でビルドする。
FmEngineベースのエミュレーター統合プラグイン (FitomEmuIF) もこの同じ
API を実装しており、FITOM本体は実機かエミュレータかを一切区別しない。
複数のHWプラグインを同時にロードでき、`HWPluginRegistry`で名前管理する。
既存の `HWControllerBase` クラス階層はそのまま残し、エクスポート関数だけ追加。

### IMidiPlugin.h
WinMM / WMS / ALSA / CoreMIDI を共通インターフェースで抽象化。
MIDI メッセージは MIDI 1.0 バイト列で統一 (WMS の UMP 変換は DLL 内で行う)。

---

## Windows MIDI Services DLL の注意事項

1. **C++/WinRT が必要** → このサブプロジェクトだけ C++20。コア本体は C++17 のまま。
2. **NuGet パッケージが GitHub 配布のみ** → vcpkg では取得できない。手動設定必要。
3. **SDK Runtime の別途インストールが必要** → エンドユーザーに要求される。
4. **64bit のみ** → 32bit ビルドには引き続き WinMM バックエンドを用意するか、
   WMS バックエンドを省略する。
5. **Runtime 未インストール時** → `MidiPlugin_OpenIn` が `MIDI_ERR_UNAVAILABLE` を返す。
   コアはこれを検知して警告ログを出し、graceful degradation する。

---

## config.json の例

```json
{
  "log": {
    "level": "info",
    "file": "fitom.log"
  },
  "midi_plugin": {
    "dll": "fitom_midi_wms.dll"
  },
  "hw_plugins": [
    { "name": "FitomEmuIF", "dll": "FitomEmuIF.dll" },
    { "name": "SpfmDriver", "dll": "fitom_hw_spfm.dll" }
  ],
  "devices": [
    { "if": "HW", "plugin": "FitomEmuIF", "type": "FMHWIF", "engine": "YMEngine", "chip": "OPNA", "index": 0, "pan": 0 },
    { "if": "HW", "plugin": "FitomEmuIF", "type": "FMHWIF", "engine": "AYEngine", "chip": "PSG",  "index": 0, "pan": 0 },
    { "if": "HW", "plugin": "SpfmDriver", "type": "SPFM_TOWER", "port": "COM3",  "slot": 0, "hw_clock": 3993600 }
  ],
  "midi_inputs": [
    "MIDI キーボード"
  ]
}
```

---

## FitomIFTest 側の追加作業

既存のヘッダオンリーライブラリに加えて、`IHWPlugin.h` の C API を実装した
共有ライブラリ (`fitom_hw`) をビルドするターゲットを追加する。

```cpp
// FitomIFTest/src/HWPluginImpl.cpp に追加する実装パターン

#define FITOM_HW_PLUGIN_EXPORTS
#include <fitom/IHWPlugin.h>
#include "RE1Controller.hpp"
#include "RE4Controller.hpp"
#include "SPFMController.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

struct HWDeviceOpaque {
    std::unique_ptr<hw::HWControllerBase> ctrl;
    int clock;
    int pan;
};

extern "C" {

FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetName() {
    return "FitomIFTest";
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Open(
    const char* params_json, HWHandle* out_handle)
{
    auto j = nlohmann::json::parse(params_json, nullptr, false);
    if (j.is_discarded()) return HW_ERR_INVALID_ARG;

    std::string type = j.value("type", "");
    std::string port = j.value("port", "");
    std::string serial = j.value("serial", "");
    int clock = j.value("clock", 0);
    int pan   = j.value("pan",   0);

    auto* dev = new HWDeviceOpaque();
    dev->clock = clock;
    dev->pan   = pan;

    try {
        if (type == "RE1") {
            auto ctrl = std::make_unique<hw::RE1Controller>();
            if (!serial.empty()) ctrl->openBySerial(serial);
            else ctrl->openByIndex(0);
            dev->ctrl = std::move(ctrl);
        } else if (type == "RE4") {
            auto ctrl = std::make_unique<hw::RE4Controller>();
            if (!serial.empty()) ctrl->openBySerial(serial);
            else ctrl->openByIndex(0);
            dev->ctrl = std::move(ctrl);
        } else if (type == "SPFM_TOWER") {
            dev->ctrl = std::make_unique<hw::SPFMTower>(port);
        } else if (type == "SPFM_LIGHT") {
            dev->ctrl = std::make_unique<hw::SPFMLight>(port);
        } else {
            delete dev; return HW_ERR_INVALID_ARG;
        }
    } catch (...) {
        delete dev; return HW_ERR_OPEN_FAILED;
    }

    *out_handle = reinterpret_cast<HWHandle>(dev);
    return HW_OK;
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Close(HWHandle handle) {
    delete reinterpret_cast<HWDeviceOpaque*>(handle);
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Write(
    HWHandle handle, uint16_t addr, uint8_t data)
{
    auto* dev = reinterpret_cast<HWDeviceOpaque*>(handle);
    uint8_t a_high = static_cast<uint8_t>(addr >> 8);
    uint8_t a_low  = static_cast<uint8_t>(addr & 0xFF);
    dev->ctrl->write(0, a_low, data, a_high);
    return HW_OK;
}

// ... (WriteBlock / Reset / GetClock / GetPanpot / IsOpen は同様)

} // extern "C"
```

---

## 移行フェーズ (更新版)

| フェーズ | 内容 |
|---|---|
| 1 | CMake 化・Boost 削減 (std 置き換え可能な部分のみ) |
| 2 | `plugin_sdk/` 確立・`PluginLoader.h` 実装 |
| 3 | `HWPort` (IHWPlugin 経由、実機/エミュレータ問わず複数DLL対応) |
| 4 | FitomIFTest 側に `fitom_hw.dll` ビルドターゲット追加 |
| 5 | MIDI バックエンド DLL (ALSA → WMS の順) |
| 6 | `fitom_core` 確立・既存チップドライバの型名移行 |
| 7 | GUI 分離 |
