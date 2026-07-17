# FITOM_X

**Flexible Interface for Traditional Oscillator Modules X**

Windows 専用 MFC アプリだった旧 FITOM をフルリアーキテクチャしたプロジェクト。

- バックエンド（FM エンジン / HW I/F / MIDI）を DLL プラグインに分離
- GUI(`apps/fitom_gui`, Dear ImGui)は `gui/bridge`(FITOMBridge)経由で同一プロセス内から fitom_core を利用
- Windows / Linux / macOS のクロスプラットフォームビルド対応
- C++17 / CMake 3.20+ / vcpkg

---

## アーキテクチャ概要

```
┌────────────────────────────────────────────────────────────────┐
│              GUI / アプリケーション層 (同一プロセス)               │
│         apps/fitom_gui (Dear ImGui) / apps/fitom_cli            │
└────────────────────────────┬───────────────────────────────────┘
                             │ gui/bridge (FITOMBridge)
┌────────────────────────────▼───────────────────────────────────┐
│                   fitom_core  (static lib)                     │
│  CFITOM / CInstCh / CRhythmCh / PatchManager / Config ...     │
│                                                                │
│  IPort ─── FmEnginePort ─── FM エンジン DLL (IFmEnginePlugin) │
│        └── HWPort       ─── HW I/F DLL     (IHWPlugin)        │
│  MidiManager            ─── MIDI DLL        (IMidiPlugin)     │
└────────────────────────────────────────────────────────────────┘
         │                         │
  ┌──────▼──────┐         ┌────────▼────────┐
  │ FM Engine   │         │  MIDI Backend    │
  │ YMEngine.dll│         │  fitom_midi_wms  │
  │ AYEngine.dll│         │  fitom_midi_alsa │
  └─────────────┘         └─────────────────┘
```

### 音声データフロー

```
MIDI入力
  → CInstCh / CRhythmCh
      → PatchResolver (Patch → ToneLayer[] → HwPatch)
          → ISoundDevice (COPN / COPM / COPL3 / CSCC ...)
              → IPort → FmEnginePort / HWPort → チップ
```

---

## 対応チップ

| カテゴリ | チップ / 型番 | ドライバクラス |
|---|---|---|
| **OPN 系 FM** | YM2203 (OPN) / YM2608 (OPNA) / YM2612 (OPN2) / YM3438 (OPN2C) / YMF288 (OPN3L) | `COPN` / `COPNA` / `COPN2` |
| **OPM 系 FM** | YM2151 (OPM) / YM2164 (OPP) / YM2414 (OPZ) | `COPM` / `COPP` / `COPZ` |
| **OPL 系 FM** | YM3526 (OPL) / YM3812 (OPL2) / Y8950 / YMF262 (OPL3) | `COPL` / `COPL2` / `COPL3` |
| **OPLL 系 FM** | YM2413 (OPLL) / VRC7 (OPLLP) | `COPLL` / `COPLLP` |
| **PSG / SSG** | YM2149 / AY-3-8910 (SSG/PSG) / SN76489 (DCSG) | `CSSG` / `CDCSG` |
| **SCC** | K051649 (SCC) / SCC+ | `CSCC` |
| **ADPCM** | YM2608内蔵 ADPCM-B (YMDeltaT) / YMZ280B (PCMD8) | `CYmDelta` / `CAdPcmZ280` |

複数デバイスを 1 MIDI チャンネルに束ねるマルチレイヤー発音と、`SplitPort` による 2 ポートチップ（OPNA / OPN2 / OPL3）の透過的なアドレス振り分けをサポートします。

---

## パッチ構造

```
PatchBank (*.patchbank.json)
  └── Patch [prog 0..127]
        ├── ToneLayer [0..3]        ← 最大4チップ同時発音
        │   ├── device_index        → profile の devices[] を指す
        │   ├── hw_bank / hw_prog   → HwPatch への参照
        │   └── note_range / transpose / volume_offset / pan_offset
        └── sw_bank / sw_prog       → SwPatch (ベロシティ感度 / ソフト LFO)

HwBank (*.hwbank.json)    ← チップ族ごとに独立
  └── HwPatch [prog 0..127]
        ├── FmHwVoice  (FB / ALG / AMS / PMS)
        ├── FmHwOp [4] (AR/DR/TL/KSR/MUL/DT1/WS ...)
        └── FmChipExt  (OPZ 拡張: REV / EGS / DT3)

SwBank (*.swbank.json)    ← チップ族共通
  └── SwPatch [prog 0..127]
        ├── FmSwVoice  (ソフト LFO / ビブラート)
        └── FmSwOp [4] (ベロシティ感度 / トレモロ)
```

ドラムチャンネルは `DrumPatch` (`.drumbank.json`) でノート番号ごとに通常パッチを参照します。ドラムノートも `ToneLayer` のマルチレイヤーが有効で、キックを OPN と SSG で同時発音するといった構成が可能です。

SCC 波形テーブルは `*.sccwave.json` で管理し、`FmHwOp::WS` フィールドで波形番号を選択します。ADPCM / PCM は `*.pcmbank.json` で adpcm_packer の出力を参照し、同様に `WS` フィールドでサンプルを選択します。

---

## MIDI バンクセレクト

| MIDI CC | 意味 |
|---|---|
| CC#0 (MSB) | HwBank 番号（チップ族に対応する HwPatch バンク）|
| CC#32 (LSB) | SwBank 番号（ソフトパラメータバンク）|
| Program Change | Patch 番号 |

ドラムチャンネルでは CC#0 がドラムバンク番号になります。

---

## ディレクトリ構成

```
FITOM_X/
├── apps/
│   ├── fitom_cli/          # CLI アプリケーション
│   └── fitom_gui/          # Dear ImGui GUI アプリケーション
├── backends/
│   ├── midi_alsa/          # ALSA MIDI バックエンド DLL (Linux)
│   └── midi_wms/           # Windows MIDI Services バックエンド DLL
├── config/
│   ├── fitom.conf.json     # システム設定（ログ・プラグインパス・バッファサイズ）
│   └── profiles/           # ユーザープロファイルサンプル
│       ├── emulator_only.profile.json
│       └── studio.profile.json
├── config_schema/          # JSON Schema ファイル群
│   ├── fitom.conf.schema.json
│   ├── profile.schema.json
│   ├── hwbank.schema.json
│   ├── swbank.schema.json
│   ├── patchbank.schema.json
│   ├── drumbank.schema.json
│   ├── sccwave.schema.json
│   └── pcmbank.schema.json
├── core/
│   ├── include/fitom/      # 公開ヘッダ
│   └── src/                # 実装（新設計: *_new.cpp）
├── docs/                   # 設計ドキュメント
├── gui/
│   └── bridge/             # GUI ブリッジ（UIフレームワーク非依存、apps/fitom_gui 等から利用）
├── legacy/                 # 旧実装（ビルド対象外、参照用に保管）
│   ├── include/fitom/
│   └── src/
├── plugin_sdk/
│   └── include/fitom/      # DLL プラグイン C API
│       ├── IFmEnginePlugin.h
│       ├── IHWPlugin.h
│       └── IMidiPlugin.h
├── tests/                  # Catch2 テスト
├── CMakeLists.txt
├── CMakePresets.json
└── vcpkg.json
```

---

## ビルド要件

| 項目 | 要件 |
|---|---|
| C++ 標準 | C++17 |
| CMake | 3.20 以上 |
| パッケージマネージャ | vcpkg (manifest モード) |
| Boost | 1.71 以上 (asio / thread / log / format / interprocess) |
| nlohmann/json | vcpkg 経由 |
| RtAudio | `extern/rtaudio/` に submodule として配置（任意）|
| libftdi1 | HW I/F バックエンド使用時（vcpkg 経由）|

Windows MIDI Services バックエンドのみ C++20 と WMS SDK が別途必要です。

---

## ビルド手順

### 共通（初回）

```bash
# 依存ライブラリのインストール
vcpkg install

# または CMake が vcpkg toolchain を認識している場合は不要
# (CMakePresets.json が VCPKG_ROOT 環境変数を参照)
```

### Linux

```bash
cmake --preset linux-ninja
cmake --build --preset linux-release

# テスト
ctest --preset linux-test
```

### Windows (Visual Studio 2022)

```bash
cmake --preset windows-vs2022-x64
cmake --build --preset windows-release
```

### macOS

```bash
cmake --preset macos-ninja
cmake --build --preset macos-release
```

`VCPKG_ROOT` 環境変数に vcpkg のルートディレクトリを設定しておく必要があります。

---

## ビルドオプション

| オプション | デフォルト | 説明 |
|---|---|---|
| `FITOM_BUILD_BACKEND_FMENGINE` | ON | FM エンジンバックエンド DLL |
| `FITOM_BUILD_BACKEND_HWIF` | ON | HW I/F バックエンド DLL |
| `FITOM_BUILD_BACKEND_MIDI_WMS` | OFF | Windows MIDI Services バックエンド（Windows のみ）|
| `FITOM_BUILD_BACKEND_MIDI_ALSA` | OFF | ALSA MIDI バックエンド（Linux のみ）|
| `FITOM_GUI_IMGUI` | OFF | Dear ImGui GUI（`gui/bridge` + `apps/fitom_gui`）|
| `FITOM_GUI_QT` | OFF | Qt6 GUI（実装予定）|
| `FITOM_BUILD_TESTS` | ON | Catch2 テスト |

---

## 設定ファイル

### `fitom.conf.json` — システム設定（管理者向け）

ログレベル、プラグイン DLL のパス、RtAudio バッファサイズなど、環境依存の設定を記述します。

```json
{
  "log":     { "level": "info", "file": "fitom.log" },
  "plugins": {
    "fm_engine_dir": "engines",
    "hw_plugin":    { "dll": "fitom_hw.dll" },
    "midi_plugin":  { "dll": "fitom_midi_wms.dll" }
  },
  "audio":   { "api": "auto", "buffer_frames": 512 },
  "timing":  { "timer_ms": 1 }
}
```

### `*.profile.json` — ユーザープロファイル

使用デバイス、MIDI 入力、チャンネルマップ、バンクファイルのパスを記述します。`fitom.conf.json` とは独立して切り替え可能です。

```json
{
  "devices": [
    { "if": "FMENGINE", "label": "OPM", "engine": "YMEngine", "chip": "OPM" },
    { "if": "HW", "label": "OPNA", "type": "SPFM_TOWER", "port": "COM3",
      "slot": 0, "extra_slot": 1 }
  ],
  "channel_map": [
    { "midi_ch": 1, "device_index": 0, "poly": 8 },
    { "midi_ch": 10, "device_index": 1, "poly": 1 }
  ],
  "banks": {
    "hw_banks":    [{ "group": "OPM", "bank": 0, "file": "banks/hw/opm/default.hwbank.json" }],
    "patch_banks": [{ "bank": 0, "file": "banks/patches/general.patchbank.json" }],
    "drum_banks":  [{ "bank": 0, "file": "banks/drums/gm_drums.drumbank.json" }]
  }
}
```

`banks.*[].file` の相対パスは、プロファイルファイル自身が置かれている
ディレクトリを起点に解決されます(起動時のカレントディレクトリは無関係)。

---

## プラグイン SDK

`plugin_sdk/include/fitom/` の3つのヘッダが DLL プラグインの C API を定義します。

**`IFmEnginePlugin.h`** — FM エンジン（エミュレーター）DLL。既存の `FmEngineApi.h` との互換を保ちつつ `FmEngine_GetEngineName()` を1関数追加するだけで対応できます。

**`IHWPlugin.h`** — HW I/F DLL。SPFM 等の物理デバイスへのアクセスを `HWPlugin_Write` / `HWPlugin_Read` としてフラット化した C API です。アドレスの上位8bit が a_high（ポート番号）として扱われ、`SplitPort` 経由で 2 ポートチップのアドレス振り分けを自動的に行います。

**`IMidiPlugin.h`** — MIDI バックエンド DLL。WMS / ALSA / CoreMIDI を共通インターフェースで抽象化し、MIDI 1.0 バイト列で統一します。

---

## ドキュメント

`docs/` ディレクトリに設計ドキュメントが揃っています。

- `DESIGN.md` — アーキテクチャ全体像と設計方針
- `voice-data-design.md` — HwPatch / SwPatch / FmHwOp の設計
- `patch-structure-design.md` — パッチ・バンク構造
- `config-design.md` — 設定ファイル (`fitom.conf.json` / `*.profile.json`) の設計
- `chip-driver-migration.md` — 旧チップドライバから新設計への移行ガイド
- `plugin-fmengine.md` / `plugin-hwif.md` / `plugin-midi.md` — 各プラグイン SDK の詳細
- `STATUS.md` — 実装完了状況と残タスク

---

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照。
