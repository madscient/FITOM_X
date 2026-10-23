# FITOM_X

**Flexible Interface for Traditional Oscillator Modules X**

複数の FM / PSG / PCM 音源チップを横断的に扱う、マルチチップ音源エンジンと MIDI アプリケーションです。

- 実機のチップもエミュレータも、同じ HW I/F プラグイン(DLL)の C API を通して鳴らす
- 1 つの音色(レイヤードパッチ)で、種類の違う複数のチップを重ねて発音できる
- GUI(`apps/fitom_gui`, Dear ImGui)と CLI(`apps/fitom_cli`)は、同一プロセス内で `fitom_core` を利用する
- Windows / Linux / macOS のクロスプラットフォームビルド
- C++17 / CMake 3.24+

---

## アーキテクチャ概要

```
apps/fitom_gui (Dear ImGui) / apps/fitom_cli
  │  gui/bridge (FITOMBridge)
  ▼
fitom_core (static lib)
  CFITOM / CInstCh / CRhythmCh / PatchManager / FITOMConfig
  ├─ チップドライバ (ISoundDevice: COPN / COPM / COPL3 / CSSG / CSCC ...)
  │    └─ HWPort (IPort) ─── HW I/F DLL (IHWPlugin)
  │                            ├─ FitomHwIF  … 実機 (RE1 / RE4 / SPFM 等)
  │                            ├─ FitomEmuIF … エミュレータ (FM エンジン DLL を内部でロード)
  │                            └─ FitomSf2IF … SoundFont (fluidsynth)
  └─ MidiManager ─── MIDI DLL (IMidiPlugin)
                       ├─ fitom_midi_rtmidi … MIDI 入力 (WinMM / ALSA / CoreMIDI)
                       └─ fitom_midi_pipe   … 内部用 MIDI パイプ (パッチエディタ連携)
```

HW I/F DLL は別リポジトリで開発しています。`fitom_core` は、接続先が実機かエミュレータかを区別しません。

### 音声データフロー

```
MIDI入力
  → CInstCh / CRhythmCh
      → PatchResolver (Patch → ToneLayer[] → HwPatch + SwPatch)
          → ISoundDevice (チップドライバ)
              → HWPort → HW I/F DLL → チップ (実機 / エミュレータ)
```

---

## 対応チップ

プロファイルの `chip` に書く名前と、対応するチップです。

| カテゴリ | `chip` の値 | チップ |
|---|---|---|
| **OPN 系 FM** | `OPN` / `OPNA` / `OPN2` / `OPN2C` / `OPN2L` / `OPN3` / `OPNB` / `OPNBB` | YM2203 / YM2608 / YM2612 / YM3438 / YMF276 / YMF297 / YM2610 / YM2610B |
| **OPM 系 FM** | `OPM` / `OPP` / `OPZ` | YM2151 / YM2164 / YM2414 |
| **OPL 系 FM** | `OPL` / `OPL2` / `Y8950` / `OPL3` / `OPL3_2` / `OPL4` | YM3526 / YM3812 / Y8950 / YMF262 (`OPL3_2` は 2 オペレータで使う構成) / YMF278 (FM 部 + AWM 部) |
| **OPLL 系 FM** | `OPLL` / `OPLL2` / `OPLLP` / `OPLLX` / `VRC7` | YM2413 / YM2420 / YMF281 / YM2423 / VRC7 |
| **PSG 系** | `SSG` / `PSG` / `EPSG` / `DCSG` / `SAA` | YM2149 / AY-3-891x / AY8930 / SN76489 / SAA1099 |
| **SSG 互換部を持つチップ** | `SSGS` / `SSGS2` / `SSGS3` (`YMZ705` / `YMZ732` / `YMZ771` とも書ける) | YMZ705 / YMZ732 / YMZ771 ※ |
| **SCC** | `SCC` / `SCCP` | K051649 / SCC+ |
| **DSG** | `DSG` (`YM2163` とも書ける) | YM2163 ※ |
| **PCM** | `PCMD8` | YMZ280B |
| **Y8960 (MSX 用サウンドカートリッジ)** | `OPLLEX` / `OPL2EX` / `SSGEX` / `SCCEX` / `OPM` | 拡張 OPLL 部 / 拡張 OPL2 部 / SSG 部 / SCC 部 / OPM ※ |
| **SoundFont** | `SF2` | FitomSf2IF 経由の SoundFont 音源 |

ADPCM-A / ADPCM-B・OPNA の内蔵リズム音源・AWM 部など、チップに内蔵された部位は、親のチップを指定すると自動的に使えるようになります。OPL 系・OPLL 系の内蔵リズム音源は、ch6-8 をリズム専用にするため、デバイスに `rhythm_mode: true` を指定したときだけ使えます。

※ 実機・エミュレータでの音の確認がまだ済んでいないチップです。

---

## パッチ構造

```
PatchBank (*.patchbank.json)     ← 通常モード (CC#0 = 0) で選ぶ音色
  └── Patch [prog 0..127]
        └── ToneLayer [最大 4]
              ├── voice_patch_type    → 鳴らすチップの種別
              ├── hw_bank / hw_prog   → HwPatch への参照
              └── note_range_lo / note_range_hi / transpose / volume_offset / pan_offset

HwBank (*.hwbank.json)           ← チップ種別ごとに独立したバンク番号
  └── HwPatch [prog 0..127]      (FB / ALG / AMS / PMS / ops[] / ext)
        └── sw_bank / sw_prog    → SwPatch への参照

SwBank (*.swbank.json)
  └── SwPatch [prog 0..127]      (ソフトウェア LFO / ベロシティ感度)
```

- ドラムは `*.drumkit.json` でノート番号ごとに音色を割り当てます
- SCC の波形は `*.sccwave.json` で管理し、HwPatch の `ops[0].WS` で波形番号を選びます
- ADPCM・PCM 系は、波形イメージを `*.pcmbank.json` に、鍵盤・ベロシティの範囲ごとの割り当てを `*.samplezonebank.json` に書きます

各ファイルの書式は、利用者向けマニュアル(下記「ドキュメント」)を参照してください。

---

## MIDI バンクセレクト

CC#0 の値で、音色の選び方が変わります。

| CC#0 | モード | CC#32 / プログラムチェンジ |
|---|---|---|
| 0 | 通常モード | パッチバンク番号 / パッチ番号 |
| 1–111 | 直接デバイス選択(CC#0 がチップの種別) | そのチップの HwBank 番号 / 音色番号 |
| 112 | 内蔵リズム音源 | 対象チップ / 内蔵楽器の番号 |
| 120 / 121 | リズムチャンネル / メロディチャンネルへ切り替え | — |
| 127 | 無音バンク | 参照しない |

詳細は [MIDI メッセージ リファレンスマニュアル](docs/manuals/midi-message-reference.md) の 2.2 節を参照してください。

---

## ディレクトリ構成

```
FITOM_X/
├── apps/
│   ├── fitom_cli/          # CLI アプリケーション
│   └── fitom_gui/          # Dear ImGui GUI アプリケーション
├── backends/
│   ├── hw_if/              # HW I/F バックエンドを外部ソースから取り込む口
│   ├── midi_pipe/          # 内部用 MIDI パイプ DLL (パッチエディタ連携)
│   └── midi_rtmidi/        # RtMidi MIDI バックエンド DLL (Windows/Linux/macOS共通)
├── banks/                  # バンクファイルの書式例
├── config/
│   ├── fitom.conf.json     # システム設定の例
│   └── profiles/           # プロファイルの例
├── config_schema/          # 設定ファイル・バンクファイルの JSON Schema
├── core/
│   ├── include/fitom/      # 公開ヘッダ
│   └── src/                # 実装
├── docs/
│   ├── manuals/            # 利用者向けマニュアル
│   ├── design/             # 本体の内部設計
│   ├── interfaces/         # プラグイン・外部ツールとの契約
│   ├── proposals/          # 未実装の設計検討
│   ├── worklog/            # 変更・修正・調査の経緯
│   └── STATUS.md           # 実装状況
├── gui/
│   └── bridge/             # GUI ブリッジ（UIフレームワーク非依存）
├── legacy/                 # 旧実装（ビルド対象外、参照用に保管）
├── plugin_sdk/
│   └── include/fitom/      # DLL プラグイン C API
│       ├── IHWPlugin.h
│       └── IMidiPlugin.h
├── spec/                   # 外部ツール向けの機械可読仕様 (chip-capabilities.json)
├── tests/                  # Catch2 テスト
├── third_party/            # git submodule (nlohmann-json / rtmidi / imgui / glfw)
├── CMakeLists.txt
├── CMakePresets.json
└── vcpkg.json
```

---

## ビルド要件

| 項目 | 要件 |
|---|---|
| C++ 標準 | C++17 |
| CMake | 3.24 以上 |
| Boost | 1.71 以上 (log / log_setup / thread。format はヘッダーのみ使用) |
| nlohmann/json | git submodule (`third_party/nlohmann-json`) |
| RtMidi | git submodule (`third_party/rtmidi`) |
| Dear ImGui / GLFW | git submodule (`third_party/imgui` / `third_party/glfw`)。GUI をビルドするときのみ |

---

## ビルド手順

### 共通（初回）

```bash
git submodule update --init --recursive
```

Boost は各プラットフォームの手段で用意します。

- Ubuntu/Debian: `apt install libboost-dev libboost-log-dev libboost-thread-dev`
- Windows: 公式バイナリ配布、MSYS2、または vcpkg。vcpkg を使う場合は、configure 時に
  vcpkg のツールチェーン(`-DCMAKE_TOOLCHAIN_FILE=<vcpkg のルート>/scripts/buildsystems/vcpkg.cmake`)を
  指定すると、`vcpkg.json` に従って Boost が取得されます(`windows-vs2022-x64-vcpkg` /
  `windows-vs2026-x64-vcpkg` プリセットはこれを指定済み)

### Linux

```bash
cmake --preset linux-ninja
cmake --build --preset linux-release
ctest --preset linux-test
```

### Windows (Visual Studio 2022 / 2026)

```bash
cmake --preset windows-vs2022-x64        # VS2026 は windows-vs2026-x64
cmake --build --preset windows-release   # VS2026 は windows-vs2026-release
ctest --preset windows-test              # VS2026 は windows-vs2026-test
```

GUI (`apps/fitom_gui`) も含めてビルドする場合:

```bash
cmake --preset windows-vs2026-x64-gui-imgui
cmake --build build/windows-vs2026-x64-gui-imgui --config Release
```

### macOS

```bash
cmake --preset macos-ninja
cmake --build --preset macos-release
```

---

## ビルドオプション

| オプション | デフォルト | 説明 |
|---|---|---|
| `FITOM_BUILD_CLI` | ON | `fitom_cli`(テキストコンソールモニター) |
| `FITOM_BUILD_BACKEND_HWIF` | ON | HW I/F バックエンド。`HWIF_SOURCE_DIR` に外部ソース(FitomHwIF)を指定したときだけビルドされる |
| `FITOM_BUILD_BACKEND_MIDI_RTMIDI` | ON | RtMidi MIDI バックエンド DLL（Windows/Linux/macOS共通）|
| `FITOM_BUILD_BACKEND_MIDI_PIPE` | OFF | 内部用MIDIパイプ DLL(パッチエディタ連携)。各プリセットでは ON |
| `FITOM_GUI_IMGUI` | OFF | Dear ImGui GUI（`gui/bridge` + `apps/fitom_gui`）|
| `FITOM_GUI_QT` | OFF | Qt6 GUI（未実装）|
| `FITOM_BUILD_TESTS` | ON | Catch2 テスト |

---

## 設定ファイル

### `fitom.conf.json` — システム設定

ログの設定と、プロファイルを探すディレクトリを記述します(省略可能)。

```json
{
  "log": { "level": "info", "file": "fitom.log", "console": true },
  "profiles": { "root": "profiles" }
}
```

`fitom_gui` を引数なしで起動すると、`profiles.root` 配下の `*.profile.json` が一覧され、選んで起動できます。

### `*.profile.json` — ユーザープロファイル

使う HW I/F プラグインとチップ、MIDI 入力、バンクファイルを記述します。`fitom.conf.json` とは独立して切り替えられます。

```json
{
  "profile_name": "エミュレーター OPN",
  "hw_plugins": [
    { "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
      "profile": "fmhwif_profile.json", "auto_devices": true }
  ],
  "midi_inputs": [ "MIDI キーボード" ],
  "banks": {
    "hw_banks":    [{ "group": "OPN", "bank": 0, "file": "banks/opn.hwbank.json" }],
    "sw_banks":    [{ "bank": 0, "file": "banks/default.swbank.json" }],
    "patch_banks": [{ "bank": 0, "file": "banks/general.patchbank.json" }],
    "drum_banks":  []
  }
}
```

- `hw_plugins[].auto_devices` を `true` にすると、プラグインが持つチップがそのまま使えます。チップを個別に指定する場合は `devices[]` に書きます
- `midi_inputs` の並びが MPU 番号(最大 4)に対応します
- `banks` には、外部のバンク定義ファイルへのパスを文字列で書くこともできます
- `banks.*[].file` の相対パスは、プロファイルファイル自身が置かれているディレクトリを起点に解決されます(起動時のカレントディレクトリは無関係)

すべての項目は `config_schema/profile.schema.json` と [docs/manuals/profile-reference.md](docs/manuals/profile-reference.md) を参照してください。

---

## プラグイン SDK

`plugin_sdk/include/fitom/` の2つのヘッダが DLL プラグインの C API を定義します。

**`IHWPlugin.h`** — HW I/F DLL。チップへのレジスタ書き込みを `HWPlugin_Write` / `HWPlugin_WriteBlock` としてフラット化した C API です。渡すレジスタアドレスは常に実チップのものです。実機用もエミュレータ用もこの同じ API を実装します。チップの出力ごとの音量(部位ごとのゲイン)を調整する関数は任意の実装です。詳細は [docs/interfaces/plugin-hwif.md](docs/interfaces/plugin-hwif.md)。

**`IMidiPlugin.h`** — MIDI バックエンド DLL。MIDI 1.0 のバイト列で統一したインターフェースです。詳細は [docs/interfaces/plugin-midi.md](docs/interfaces/plugin-midi.md)、内部用 MIDI パイプは [docs/interfaces/plugin-midi-pipe.md](docs/interfaces/plugin-midi-pipe.md)。

---

## ドキュメント

### 利用者向け (`docs/manuals/`)

- [midi-message-reference.md](docs/manuals/midi-message-reference.md) — MIDI メッセージ仕様(CC / RPN / NRPN / SysEx)
- [midi-implementation-chart.md](docs/manuals/midi-implementation-chart.md) — MIDI インプリメンテーション・チャート
- [profile-reference.md](docs/manuals/profile-reference.md) — プロファイル(`*.profile.json`)と `fitom.conf.json` の書き方
- [bank-reference.md](docs/manuals/bank-reference.md) — バンクファイル(音色・ドラムキット・PCM など)の書き方
- [hwpatch-reference.md](docs/manuals/hwpatch-reference.md) — HwPatch(音色合成パラメータ)
- [swpatch-reference.md](docs/manuals/swpatch-reference.md) — SwPatch(演奏特性パラメータ)
- [layered-patch-reference.md](docs/manuals/layered-patch-reference.md) — レイヤードパッチ

### プラグイン・外部ツールの開発者向け (`docs/interfaces/`)

- [plugin-hwif.md](docs/interfaces/plugin-hwif.md) — HW I/F プラグインの契約
- [plugin-midi.md](docs/interfaces/plugin-midi.md) — MIDI バックエンドプラグインの契約
- [plugin-midi-pipe.md](docs/interfaces/plugin-midi-pipe.md) — 内部用 MIDI パイプ(パッチエディタ連携)
- [spec/README.md](spec/README.md) — 外部ツール向けの機械可読仕様

### FITOM_X 本体の開発者向け

- [docs/STATUS.md](docs/STATUS.md) — 実装状況・未確認事項・既知の未対応
- [docs/design/chip-drivers.md](docs/design/chip-drivers.md) — チップドライバの共通の仕組み
- [docs/design/chip-notes.md](docs/design/chip-notes.md) — チップごとの実装の注意
- [docs/design/patch-resolution.md](docs/design/patch-resolution.md) — パッチ・バンクの解決
- [docs/design/voice-processor.md](docs/design/voice-processor.md) — 音色データと VoiceProcessor
- [docs/design/midi-processing.md](docs/design/midi-processing.md) — MIDI 処理の内部設計
- [docs/design/config-loading.md](docs/design/config-loading.md) — 設定の読み込みと書き戻し
- [docs/design/sf2-direct-path.md](docs/design/sf2-direct-path.md) — SoundFont 音源への直行パス
- [docs/design/terminology.md](docs/design/terminology.md) — 用語
- [docs/proposals/](docs/proposals/) — 未実装の設計検討(VGM 記録、OPM/OPZ のハードウェア LFO)
- [docs/worklog/](docs/worklog/) — 変更・修正・調査の経緯(月ごと)

---

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照。
