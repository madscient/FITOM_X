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
  "midi_plugin": { "dll": "fitom_midi_wms.dll" }
}
```

| フィールド | 説明 |
|---|---|
| `midi_plugin.dll` | MIDI バックエンド DLL のパス。省略時は MIDI 入力なし |

HW プラグイン (実機・エミュレータ問わず) はプロファイル側の `hw_plugins[]`
(後述) で指定する。実機かエミュレータかを FITOM 本体は一切区別しないため、
システム設定 (`fitom.conf.json`) 側には置かない。

音声出力設定 (サンプルレート・バッファサイズ等) もFITOM_X本体では
一切管理しない。HWプラグイン (FitomEmuIF等) が自身の設定ファイル
(例: FitomEmuIFなら `fmhwif_profile.json`、DLLと同じディレクトリまたは
環境変数`FMHWIF_PROFILE`で指定) を独立して読み込み、音声出力まで
内部で完結させる。詳細は各HWプラグインのドキュメントを参照すること。

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

### HW プラグイン設定 (`hw_plugins`) — 実機/エミュレータ共通、推奨経路

**`devices[].if = "HW"` で使う。実機ハードウェア用プラグインと
エミュレーター統合プラグイン(FitomEmuIF等)は、どちらも同じ `IHWPlugin`
C APIを実装するため、FITOM本体は両者を一切区別しない。**

```json
"hw_plugins": [
  { "name": "FitomEmuIF", "dll": "FitomEmuIF.dll" },
  { "name": "SpfmDriver", "dll": "fitom_hw_spfm.dll" }
]
```

`devices[]` 側は `plugin` フィールドで名前を指定し、それ以外のフィールドは
そのままプラグインの `HWPlugin_Open()` に渡す `params_json` として転送される。
どんなフィールドが必要かはプラグインごとに異なる (下記例参照)。

**FitomEmuIF (FmEngineベースのエミュレーター統合プラグイン) の例:**

```json
{
  "if": "HW", "label": "OPN", "plugin": "FitomEmuIF",
  "type": "FMHWIF", "engine": "YMEngine", "chip": "OPN",
  "index": 0, "pan": 0
}
```

音声出力(RtAudio等)・FmEngineDLLのロードは全て `FitomEmuIF.dll` 内部が
担う。FITOM_X本体のプロファイルには音声出力の設定を一切書く必要がない。
`FitomEmuIF.dll` 自身の設定
(サンプルレート・バッファサイズ・使用するチップ一覧) は、
`FitomEmuIF.dll` と同じディレクトリに置く `fmhwif_profile.json`
(別ファイル、FITOM_X本体のプロファイルとは独立) で管理する。
詳細は [FitomEmuIF](https://github.com/madscient/FitomEmuIF) を参照。

**実機ハードウェア (RE1/SPFM等) の例:**

```json
{
  "if": "HW", "label": "OPM", "plugin": "SpfmDriver",
  "type": "SPFM_TOWER", "slot": 0, "hw_clock": 4000000, "pan": 0
}
```

### デバイス構成 (`devices`)

音源デバイスの一覧。実機ハードウェアとエミュレーターを、同じ`if: "HW"`で混在できる
(FITOM本体は両者を区別しない)。

```json
"devices": [
  {
    "if": "HW", "label": "OPM", "plugin": "FitomEmuIF",
    "type": "FMHWIF", "engine": "YMEngine", "chip": "OPM",
    "index": 0, "pan": 0
  },
  {
    "if":      "HW",
    "label":   "OPNA (SPFM)",
    "plugin":  "SpfmDriver",
    "type":    "SPFM_TOWER",
    "port":    "COM3",
    "slot":    0,
    "hw_clock": 3993600,
    "pan":     0
  }
]
```

`if=HW` 固有フィールド:

| フィールド | 必須 | 説明 |
|---|---|---|
| `plugin` | ○ | `hw_plugins[].name` への参照。実機・エミュレータ問わず同じフィールド |

`plugin`以外のフィールド（`if`/`label`/`plugin`/`extra_slot`/
`rhythm_mode`/`stereo_pair`を除く）は、**そのままJSON文字列化して
プラグインの`HWPlugin_Open()`に渡される**。どんなフィールドが必要かは
プラグインの実装に依存するため、プラグインのドキュメントを参照すること。
FITOM本体はこれらのフィールドの意味を解釈しない（実機かエミュレータかを
区別しないための設計）。

**FitomEmuIF (エミュレーター統合プラグイン) が要求するフィールド例:**

| フィールド | 説明 |
|---|---|
| `type` | `"FMHWIF"` 固定 |
| `engine` | `FitomEmuIF.dll`自身の設定(`fmhwif_profile.json`)に登録されたFmEngine名 |
| `chip` | チップ種別文字列 |
| `index` | チップインスタンス番号 |
| `pan` | 0=Stereo / 1=L / 2=R / 3=Mono |

**実機ハードウェア用プラグインが要求するフィールド例 (プラグイン依存):**

| フィールド | 説明 |
|---|---|
| `type` | `RE1` / `RE4` / `SPFM_TOWER` / `SPFM_LIGHT` 等 |
| `serial` | FT245R/FT2232H のシリアル番号 |
| `port` | COMポートまたは `/dev/ttyUSB*` |
| `slot` | SPFM スロット番号 |
| `hw_clock` | マスタークロック [Hz] |
| `pan` | 0=Stereo / 1=L / 2=R / 3=Mono |

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

`midi_inputs[i]` は `MidiProcessor(i)` に1対1で対応する。複数指定した場合、
それぞれが独立した16 MIDIチャンネル分の状態を持つ（チャンネル1つ1つを
共有するわけではない）。

### MIDI バックエンド (`midi_backend`)

```json
"midi_backend": {
  "dll": "fitom_midi_winmm.dll"
}
```

MIDI入出力を実際に処理するバックエンドDLLを指定する。`hw_plugins[].dll`と
同様、相対パスは実行ファイルのディレクトリを基点とする。

**省略可能**。省略した場合、プラットフォーム既定のファイル名を実行ファイルと
同じディレクトリから探索する。

| プラットフォーム | 既定ファイル名 |
|---|---|
| Windows | `fitom_midi_winmm.dll`（クラシックWinMM API、追加ランタイム不要） |
| Linux | `fitom_midi_alsa.so`（ALSAシーケンサAPI） |

`backends/midi_wms/`（Windows MIDI Services、要WinRTランタイム）を明示的に
使いたい場合のみ、`"dll": "fitom_midi_wms.dll"`のように明示指定する。

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
  "hw_plugins": [
    { "name": "FitomEmuIF", "dll": "FitomEmuIF.dll" }
  ],
  "devices": [
    { "if": "HW", "label": "OPM",  "plugin": "FitomEmuIF", "type": "FMHWIF", "engine": "YMEngine", "chip": "OPM",  "index": 0, "pan": 0 },
    { "if": "HW", "label": "OPL3", "plugin": "FitomEmuIF", "type": "FMHWIF", "engine": "YMEngine", "chip": "OPL3", "index": 0, "pan": 0 },
    { "if": "HW", "label": "SSG",  "plugin": "FitomEmuIF", "type": "FMHWIF", "engine": "AYEngine", "chip": "SSG",  "index": 0, "pan": 0 }
  ],
  "midi_inputs": ["MIDI キーボード"]
}
```

### profiles/studio.profile.json

```json
{
  "profile_name": "自宅スタジオ (SPFM Tower + OPN2エミュ)",
  "hw_plugins": [
    { "name": "FitomEmuIF",  "dll": "FitomEmuIF.dll" },
    { "name": "SpfmDriver",  "dll": "fitom_hw_spfm.dll" }
  ],
  "devices": [
    {
      "if": "HW", "label": "OPNA #1", "plugin": "SpfmDriver",
      "type": "SPFM_TOWER", "port": "COM3", "slot": 0,
      "hw_clock": 3993600, "pan": 0
    },
    {
      "if": "HW", "label": "OPM", "plugin": "SpfmDriver",
      "type": "SPFM_TOWER", "port": "COM3", "slot": 1,
      "hw_clock": 3579545, "pan": 0
    },
    {
      "if": "HW", "label": "OPN2 (エミュ)", "plugin": "FitomEmuIF",
      "type": "FMHWIF", "engine": "YMEngine", "chip": "OPN2",
      "index": 0, "pan": 0
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

2. プロファイルを読み込む (--profile オプション or デフォルト)
   └─ hw_plugins[] の DLL を HWPluginRegistry に登録する
   └─ devices[] から IPort を生成する (HWPort)
   └─ midi_inputs を開く (MidiPlugin_OpenIn)

3. HW プラグイン (FitomEmuIF等) が自身の設定 (fmhwif_profile.json等) に
   基づき、必要であれば内部で音声出力を開始する
   (FITOM_X本体はこの処理に一切関与しない)

4. MIDI タイマーを起動する
```

---

## バリデーション規則

| チェック | エラー / 警告 |
|---|---|
| `devices[].plugin` が `hw_plugins[]` に存在しない | **起動エラー** |
| `midi_inputs[]` のデバイスが見つからない | **警告ログ**（他のデバイスで継続） |
| `hw_plugin.dll` が見つからない（HW デバイスあり） | **起動エラー** |
| `midi_plugin.dll` が見つからない | **警告ログ**（MIDI 入力なしで継続） |
| WMS Runtime 未インストール（WMS DLL 使用時） | **警告ログ**（MIDI 入力なしで継続） |
