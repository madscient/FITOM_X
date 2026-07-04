# 設定ファイル設計書

## ファイル構成と分離方針

FITOM の設定は **2 種類のファイル** に分離する。

```
fitom.conf.json        ← システム設定（管理者・開発者向け）
profiles/
  default.profile.json ← デフォルトプロファイル
  studio.profile.json  ← ユーザーが作成した別プロファイル
  ...
```

### 分離の基準

| 項目 | fitom.conf.json | *.profile.json |
|---|---|---|
| 対象者 | 管理者・開発者 | エンドユーザー |
| 変更頻度 | 初回セットアップ時のみ | 曲・現場に応じて切り替え |
| GUI 公開 | しない（テキストエディタで編集） | GUI の「プロファイル設定」画面 |
| 内容 | ログレベル・DLL パス・タイマー設定・バッファサイズ | デバイス構成・MIDI マッピング・音源設定 |

---

## fitom.conf.json — システム設定

**ファイル位置**: 実行ファイルと同ディレクトリ（または OS 標準の設定ディレクトリ）

### ログ設定 (`log`)

```json
"log": {
  "level":   "info",
  "file":    "fitom.log",
  "console": true
}
```

| フィールド | デフォルト | 説明 |
|---|---|---|
| `level` | `"info"` | 最小ログレベル。`trace` / `debug` / `info` / `warning` / `error` / `fatal` |
| `file` | `""` | ログファイルパス。空文字でコンソールのみ |
| `console` | `true` | コンソール出力の有無 |

ログの実装は **Boost.Log** を使用する。`fitom/Log.h` のマクロ経由で全翻訳単位から利用する。

### プラグイン設定 (`plugins`)

```json
"plugins": {
  "fm_engine_dir": "engines",
  "hw_plugin":   { "dll": "fitom_hw.dll" },
  "midi_plugin": { "dll": "fitom_midi_wms.dll" }
}
```

| フィールド | 説明 |
|---|---|
| `fm_engine_dir` | FM エンジン DLL の検索ベースディレクトリ |
| `hw_plugin.dll` | HW I/F バックエンド DLL のパス。省略時は HW を使用しない |
| `midi_plugin.dll` | MIDI バックエンド DLL のパス。省略時は MIDI 入力なし |

### オーディオ設定 (`audio`)

オーディオ設定。fitom_fmhwif DLL に転送される（RtAudio は fitom_core に含まれない）。

```json
"audio": {
  "api":            "auto",
  "buffer_frames":  512,
  "output_channels": 2
}
```

| フィールド | デフォルト | 説明 |
|---|---|---|
| `api` | `"auto"` | fitom_fmhwif が使用する RtAudio API 種別（`wasapi` / `alsa` / `core` / `auto`）|
| `buffer_frames` | `512` | オーディオバッファフレーム数。小さいほど低遅延だが CPU 負荷増大 |
| `output_channels` | `2` | 出力チャンネル数（現在 2ch 固定） |

`buffer_frames` はシステム設定に置く理由: レイテンシーチューニングはハードウェア環境に依存し、プロファイルではなく環境固有の値であるため。

### タイマー設定 (`timing`)

```json
"timing": {
  "timer_ms":            1,
  "polling_interval_us": 500
}
```

---

## *.profile.json — プロファイル（ユーザー設定）

**ファイル位置**: `profiles/` ディレクトリ以下（パスは `fitom.conf.json` で設定可能）  
**切り替え方法**: GUI のプロファイル選択、または CLI の `--profile` オプション

### FM エンジン設定 (`fm_engines`)

エミュレーターバックエンド使用時のみ必要。複数の DLL を同時にロードできる。

```json
"fm_engines": [
  { "name": "YMEngine", "dll": "YMEngine.dll", "sample_rate": 48000 },
  { "name": "AYEngine", "dll": "AYEngine.dll", "sample_rate": 48000 }
]
```

- `name` は `FmEngine_GetEngineName()` の戻り値と一致すること（不一致は起動エラー）
- `dll` はファイル名のみ指定した場合、`fitom.conf.json` の `fm_engine_dir` を基点とする
- `sample_rate` は `audio_output.sample_rate` と一致させること（不一致は警告）

### オーディオ出力デバイス設定 (`audio_output`)

エミュレーターバックエンド使用時のみ有効。どのオーディオデバイスに出力するかはプロファイルによって異なるため、ここに置く。

```json
"audio_output": {
  "device":      "Focusrite USB",
  "sample_rate": 48000
}
```

| フィールド | 説明 |
|---|---|
| `device` | デバイス名の部分一致文字列。空文字でシステムデフォルト |
| `sample_rate` | 出力サンプルレート [Hz] |

**なぜプロファイルに置くか**: スタジオ環境ではオーディオインターフェースを指定し、  
外出先ではノートPC内蔵スピーカーを使うなど、プロファイルによって変わるため。

### デバイス構成 (`devices`)

音源デバイスの一覧。エミュレーターとハードウェアを混在できる。

```json
"devices": [
  {
    "if":     "FMENGINE",
    "label":  "OPM",
    "engine": "YMEngine",
    "chip":   "OPM",
    "clock":  0,
    "gain_l": 1.0,
    "gain_r": 1.0
  },
  {
    "if":      "HW",
    "label":   "OPNA (SPFM)",
    "type":    "SPFM_TOWER",
    "port":    "COM3",
    "slot":    0,
    "hw_clock": 3993600,
    "pan":     0,
    "sample_rate": 44100
  }
]
```

`if=FMENGINE` 固有フィールド:

| フィールド | 必須 | 説明 |
|---|---|---|
| `engine` | ○ | `fm_engines[].name` への参照 |
| `chip` | ○ | チップ種別文字列（`FmEngine_AddChip` の name 引数） |
| `clock` | - | マスタークロック [Hz]。0 で標準クロック |
| `gain_l` / `gain_r` | - | L/R ゲイン（1.0 = 0dB） |

`if=HW` 固有フィールド:

| フィールド | 必須 | 説明 |
|---|---|---|
| `type` | ○ | `RE1` / `RE4` / `SPFM_TOWER` / `SPFM_LIGHT` |
| `serial` | RE1/RE4 推奨 | FT245R/FT2232H のシリアル番号 |
| `port` | SPFM 必須 | COMポートまたは `/dev/ttyUSB*` |
| `slot` | - | SPFM スロット番号（デフォルト 0） |
| `hw_clock` | - | マスタークロック [Hz]（デフォルト 0） |
| `pan` | - | 0=Stereo / 1=L / 2=R / 3=Mono |
| `sample_rate` | - | サンプルレート [Hz]（デフォルト 44100）。`fitom_hw.dll`/`fitom_fmhwif.dll` のキューイング遅延計算（レイテンシ同期）に使用する |

`if` に関わらず共通のオプションフィールド:

| フィールド | 必須 | 説明 |
|---|---|---|
| `rhythm_mode` | - | `true`でチップ内蔵リズム音源を有効化（デフォルト`false`）。OPLL系（COPLL/COPLL2/COPLLP/COPLLX）で対応済み。OPL系は将来対応予定。特定チップ専用の名前ではなく、内蔵リズム音源を持つチップ全般で共通のフィールド名とする |
| `extra_slot` | - | 2ポートチップ（OPNA/OPN2/OPL3等）用、2番目のSPFMスロット番号 |

### MIDI デバイス (`midi_inputs`, `midi_outputs`)

```json
"midi_inputs":  ["MIDI キーボード", "UM-ONE"],
"midi_outputs": ["External Synth Out"]
```

デバイス名は `MidiPlugin_EnumerateIn()` / `MidiPlugin_EnumerateOut()` が返す文字列と一致させること。  
GUI はこれらの関数を呼んで得たリストから選択できるようにすることが望ましい。

### MIDI チャンネルの既定動作（`channel_map`は廃止）

`channel_map`フィールドは廃止した。既存プロファイルにこのフィールドが
残っていても単純に読み飛ばされるだけで、エラーにはならない。

現在の既定動作（GM規格準拠）:

- **MIDI ch10（0-indexed: ch9）は固定でリズムチャンネル**として扱われる
  （`CRhythmCh`が生成される）。他のチャンネルは全て通常の楽器パート
  （`CInstCh`）になる。この割り当ては設定不可の固定仕様。
- **各チャンネルのポリフォニー数はデバイス依存**で動的に決定される。
  `ProgChange`受信時に解決されたパッチが使用するデバイス（複数レイヤーが
  ある場合はその全て）のうち、最小のチャンネル数を持つデバイスがそのまま
  ポリフォニー数の上限になる。固定設定値は存在しない。
- 起動時、全MIDIチャンネルに対して暗黙的に`ProgChange(bank=0, prog=0)`が
  適用される（GM準拠のデフォルト音色）。これにより、シーケンサーが
  明示的にProgram Changeを送らずいきなりNote Onを送ってきた場合でも、
  即座に発音できる。

---

## ファイル例

### fitom.conf.json（開発者環境）

```json
{
  "log": {
    "level": "debug",
    "file":  "fitom.log",
    "console": true
  },
  "plugins": {
    "fm_engine_dir": "engines",
    "hw_plugin":   { "dll": "fitom_hw.dll" },
    "midi_plugin": { "dll": "fitom_midi_wms.dll" }
  },
  "audio": {
    "api":           "wasapi",
    "buffer_frames": 256
  },
  "timing": {
    "timer_ms":            1,
    "polling_interval_us": 500
  }
}
```

### profiles/emulator_only.profile.json

```json
{
  "profile_name": "エミュレーターのみ (OPM + OPL3 + SSG)",
  "fm_engines": [
    { "name": "YMEngine", "dll": "YMEngine.dll", "sample_rate": 48000 },
    { "name": "AYEngine", "dll": "AYEngine.dll", "sample_rate": 48000 }
  ],
  "audio_output": {
    "device": "",
    "sample_rate": 48000
  },
  "devices": [
    { "if": "FMENGINE", "label": "OPM",  "engine": "YMEngine", "chip": "OPM",  "clock": 0 },
    { "if": "FMENGINE", "label": "OPL3", "engine": "YMEngine", "chip": "OPL3", "clock": 0 },
    { "if": "FMENGINE", "label": "SSG",  "engine": "AYEngine", "chip": "SSG",  "clock": 0 }
  ],
  "midi_inputs": ["MIDI キーボード"]
}
```

### profiles/studio.profile.json

```json
{
  "profile_name": "自宅スタジオ (SPFM Tower + OPN2エミュ)",
  "fm_engines": [
    { "name": "YMEngine", "dll": "YMEngine.dll", "sample_rate": 48000 }
  ],
  "audio_output": {
    "device": "Focusrite USB",
    "sample_rate": 48000
  },
  "devices": [
    {
      "if": "HW", "label": "OPNA #1",
      "type": "SPFM_TOWER", "port": "COM3", "slot": 0,
      "hw_clock": 3993600, "pan": 0
    },
    {
      "if": "HW", "label": "OPM",
      "type": "SPFM_TOWER", "port": "COM3", "slot": 1,
      "hw_clock": 3579545, "pan": 0
    },
    {
      "if": "FMENGINE", "label": "OPN2 (エミュ)",
      "engine": "YMEngine", "chip": "OPN2",
      "clock": 7670454, "gain_l": 0.8, "gain_r": 0.8
    }
  ],
  "midi_inputs": ["UM-ONE"]
}
```

---

## 起動時の読み込み順序

```
1. fitom.conf.json を読み込む
   └─ ログ初期化 (Boost.Log)
   └─ プラグイン DLL パスを確定する
   └─ audio.api / audio.buffer_frames を確定する

2. プロファイルを読み込む (--profile オプション or デフォルト)
   └─ fm_engines[] の DLL を FmEngineRegistry に登録する
   └─ audio_output.device / sample_rate を確定する
   └─ devices[] から IPort を生成する (FmEnginePort or HWPort)
   └─ midi_inputs を開く (MidiPlugin_OpenIn)

3. fitom_fmhwif DLL が内部で RtAudio ストリームを開く
   └─ audio_output.device で検索、sample_rate と buffer_frames を設定する
   └─ fm_engines[] の sample_rate との一致を確認し、不一致なら警告ログ

4. MIDI タイマーを起動する
```

---

## バリデーション規則

| チェック | エラー / 警告 |
|---|---|
| `fm_engines[].name` と `FmEngine_GetEngineName()` の不一致 | **起動エラー** |
| `audio_output.sample_rate` と `fm_engines[].sample_rate` の不一致 | **警告ログ**（動作は継続） |
| `devices[].engine` が `fm_engines[]` に存在しない | **起動エラー** |
| `devices[].chip` がエンジンの非対応チップ | **起動エラー** |
| `midi_inputs[]` のデバイスが見つからない | **警告ログ**（他のデバイスで継続） |
| `hw_plugin.dll` が見つからない（HW デバイスあり） | **起動エラー** |
| `midi_plugin.dll` が見つからない | **警告ログ**（MIDI 入力なしで継続） |
| WMS Runtime 未インストール（WMS DLL 使用時） | **警告ログ**（MIDI 入力なしで継続） |
