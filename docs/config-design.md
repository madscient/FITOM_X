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
| 内容 | ログ設定のみ(現状) | デバイス構成・MIDI マッピング・音源設定 |

---

## fitom.conf.json — システム設定

**ファイル位置**: 実行ファイルと同ディレクトリ。省略可能(無ければ各アプリの
既定値で動作する。`fitom_cli`: level=debug/file=fitom_cli.log、
`fitom_gui`: level=info/file=なし。いずれも console=true が既定)。

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
`fitom_cli`/`gui/bridge`(`FITOMBridge::init`)は、実行ファイルと同じ
ディレクトリに`fitom.conf.json`が存在すれば読み込み、`log.*`の値で
`Log::init()`を呼ぶ(2026年7月〜。読み込み自体は`FITOMConfig::
loadSystemConf()`、値の取り出しは`getLogLevel()`/`getLogFile()`/
`getLogConsole()`)。

### プラグイン設定・タイマー設定は廃止(2026年7月)

以前はここに`plugins.midi_plugin.dll`(MIDIバックエンドDLL指定)と
`timing.timer_ms`/`polling_interval_us`のセクションがあったが、実装が
存在しない設定項目だったため`fitom.conf.schema.json`から削除した。

- MIDIバックエンドDLLの指定は、プロファイル側の`midi_backend.dll`に
  一本化されている(こちらが実際に読まれる経路。`fitom.conf.json`側の
  同名フィールドは重複かつパースされるだけで一度も参照されていなかった)
- `timer_ms`: `CFITOM::startTimerThread()`は引数でティック間隔を
  受け取れるが、ポルタメント速度テーブル(`kPortaSpeedTable`、
  `MidiCh.cpp`)やソフトLFOのレート換算(`rateToTicks()`)は「1ティック
  =1ms」を前提に較正されているため、安全に可変化できない
- `polling_interval_us`: HWポーリングの責務はHWプラグイン
  (`fitom_fmhwif.dll`等)側に移管済み(`RtAudio削除`、`plugin-hwif.md`
  参照)であり、FITOM_X本体にポーリングスレッド自体が存在しない

HW プラグイン (実機・エミュレータ問わず) はプロファイル側の `hw_plugins[]`
(後述) で指定する。実機かエミュレータかを FITOM 本体は一切区別しないため、
システム設定 (`fitom.conf.json`) 側には置かない。

音声出力設定 (サンプルレート・バッファサイズ等) もFITOM_X本体では
一切管理しない。HWプラグイン (FitomEmuIF等) が自身の設定ファイル
(例: FitomEmuIFなら `fmhwif_profile.json`、DLLと同じディレクトリまたは
環境変数`FMHWIF_PROFILE`で指定) を独立して読み込み、音声出力まで
内部で完結させる。詳細は各HWプラグインのドキュメントを参照すること。

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
  { "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
    "profile": "fmhwif_profile.json" },
  { "name": "FitomHwIF", "dll": "fitom_hw.dll",
    "profile": "fitom_hw_profile.json" }
]
```

| フィールド | 必須 | 説明 |
|---|---|---|
| `name` | ○ | `devices[].plugin` から参照される識別名 |
| `dll` | ○ | IHWPlugin実装DLLのパス |
| `profile` | - | プラグイン自身の設定ファイルへのパス (下記参照) |

**`profile` について**: FITOM_XはDLLロード直後、他の全関数(Enumerate/Open等)
より前に`HWPlugin_Init(profile)`を必ず呼ぶ(`IHWPlugin.h`参照。全プラグイン
必須実装のAPI)。`profile`を省略した場合はnullptrを渡し、プラグイン自身の
デフォルト探索ルール(DLLと同じディレクトリの既定ファイル名を探す等)に
従わせる。

**FITOM_X本体は`profile`が指すファイルの内容を一切解釈・検証しない**
(エミュレータか実機かを区別しない設計原則を保つため)。ファイル内容が
不正な場合、`HWPlugin_Init()`自体が`HW_ERR_INVALID_ARG`を返し、
**そのプラグインは登録されない**(`hw_plugins[].name`で参照しても
見つからない扱いになり、そのプラグインを使う`devices[]`は全て
デバイス生成失敗としてエラーログが出る)。プロファイルの内容と
`devices[]`側の記述(`engine`/`chip`/`index`等)が整合しない場合は、
`HWPlugin_Init()`自体は成功するが、後続の`HWPlugin_Open()`が
`HW_ERR_NOT_FOUND`を返す。

これにより、FITOM_Xのプロファイルを切り替えると、対応するプラグイン側の
設定ファイルも連動して切り替わり、再現性が保たれる。

`devices[]` 側は `plugin` フィールドで名前を指定し、それ以外のフィールドは
そのままプラグインの `HWPlugin_Open()` に渡す `params_json` として転送される。
どんなフィールドが必要かはプラグインごとに異なる (下記例参照)。

### `devices[]` の自動生成 (`hw_plugins[].auto_devices`) — 推奨・標準の使い方

**チップ接続情報の管理は実機・エミュレータ問わずHWプラグイン側に
完全に委譲する。** 実機であっても「どのスロットにどのチップが挿さって
いるか」はハードウェア的に自動検出できないため、実機用プラグイン
(FitomHwIF)もエミュレーター統合プラグイン(FitomEmuIF)と同様に、
自分自身の設定ファイルでチップ構成を管理し、`HWPlugin_Enumerate()`
はその設定内容をそのまま返す(ライブなハードウェア検出ではなく、
設定の反映)。**この動作はFitomHwIF・FitomEmuIF両方の実装で確認済み。**

`hw_plugins[]`側に`"auto_devices": true`を指定すると、
`HWPlugin_Enumerate()` が返すJSON配列の各要素をそのまま`devices[]`
エントリとして使う。これにより、FITOM_X側のプロファイルで
チップ構成情報を重複して記述する必要がなくなり、一次情報は各プラグイン
自身の設定ファイルに一元化される。

```json
"hw_plugins": [
  {
    "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
    "profile": "fmhwif_profile.json",
    "auto_devices": true
  },
  {
    "name": "FitomHwIF", "dll": "fitom_hw.dll",
    "profile": "fitom_hw_profile.json",
    "auto_devices": true
  }
]
```
`devices[]` セクション自体は省略できる（通常はこれが標準形）。

**動作の仕組み**: `HWPlugin_Enumerate()`が返すエントリ(例:
`{"type":"RE1","serial":"ABCD1234","index":0,"slot":0,"chip":"OPN","clock":3993600,"pan":0}`)
には、FITOM_Xが`resolveChipDeviceId()`で使う`chip`情報と、
`HWPlugin_Open()`がスロット特定に使う`type`/`serial`(または`port`)/
`index`/`slot`の両方が含まれる。このエントリ全体をそのまま
`params_json`として`HWPlugin_Open()`に渡すため、`chip`/`clock`のような
プラグイン側は無視する余分な情報が含まれていても問題ない
(各プラグインの`HWPlugin_Open`実装は必要なフィールドのみ読み、
残りは単に無視する)。

**前提**:
- 実際に動作するかは、使用するHWプラグインが`HWPlugin_Enumerate()`を
  正しく実装しているかどうかに依存する。未実装 (`nullptr`返却)の
  古いプラグインの場合、この機能は使えず、従来通り明示的な
  `devices[]`記述が必要 (下記参照)。
- `devices[]`側に同じ`plugin`を指定した追加エントリを手動で書くことも
  でき、自動生成分と併用できる。

**明示的な`devices[]`記述が必要な場合の例 (auto_devices未対応の古い
プラグイン、またはプロファイルに無いスロットを開く場合):**

FitomEmuIF向け:
```json
{
  "if": "HW", "label": "OPN", "plugin": "FitomEmuIF",
  "type": "FMHWIF", "engine": "YMEngine", "chip": "OPN",
  "index": 0, "pan": 0
}
```

FitomHwIF向け (`chip`/`clock`はプロファイル`fitom_hw_profile.json`側で
既に確定しているため`HWPlugin_Open`には不要。`type`+`slot`が必須):
```json
{
  "if": "HW", "label": "OPNA (RE1)", "plugin": "FitomHwIF",
  "type": "RE1", "serial": "ABCD1234", "slot": 0, "pan": 0
}
```

音声出力(RtAudio等)・FmEngineDLLのロードは全て `FitomEmuIF.dll` 内部が
担う。FITOM_X本体のプロファイルには音声出力の設定を一切書く必要がない。
`FitomEmuIF.dll` 自身の設定
(サンプルレート・バッファサイズ・使用するチップ一覧) は、
`FitomEmuIF.dll` と同じディレクトリに置く `fmhwif_profile.json`
(別ファイル、FITOM_X本体のプロファイルとは独立) で管理する。
詳細は [FitomEmuIF](https://github.com/madscient/FitomEmuIF) を参照。

**実機ハードウェア (RE1/SPFM等、FitomHwIF) の例:**

```json
{
  "if": "HW", "label": "OPM", "plugin": "FitomHwIF",
  "type": "SPFM_TOWER", "port": "COM4", "slot": 0, "pan": 0
}
```
(`chip`/`clock`は`fitom_hw_profile.json`側で確定済みのため`HWPlugin_Open`には
不要。`type`+`port`(またはserial/index)+`slot`が必須)

### デバイス構成 (`devices`)

音源デバイスの一覧。実機ハードウェアとエミュレーターを、同じ`if: "HW"`で混在できる
(FITOM本体は両者を区別しない)。**通常は`auto_devices`(前述)を使うため、
`devices[]`を明示的に記述する機会は限定的**(auto_devices未対応の古い
プラグインを使う場合、または特定スロットのみ追加で開きたい場合等)。

```json
"devices": [
  {
    "if": "HW", "label": "OPM", "plugin": "FitomEmuIF",
    "type": "FMHWIF", "engine": "YMEngine", "chip": "OPM",
    "index": 0, "pan": 0
  },
  {
    "if":     "HW",
    "label":  "OPNA (SPFM)",
    "plugin": "FitomHwIF",
    "type":   "SPFM_TOWER",
    "port":   "COM3",
    "slot":   0,
    "pan":    0
  }
]
```

`if=HW` 固有フィールド:

| フィールド | 必須 | 説明 |
|---|---|---|
| `plugin` | ○ | `hw_plugins[].name` への参照。実機・エミュレータ問わず同じフィールド |

**`if` と `type` の役割の違いに注意**: `if`はFITOM_X内部の分岐キーであり
(現在は`"HW"`のみ)、`params_json`には転送されない。一方`type`は
**FITOM_X側では一切解釈されず**、`params_json`にそのまま転送される、
完全にプラグイン依存の値である(例: FitomEmuIFは`"FMHWIF"`を要求する)。
`type`の妥当な値・必要性はプラグインのドキュメントに従うこと。

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

**FitomHwIF (実機ハードウェア用プラグイン) が要求するフィールド例:**

| フィールド | 必須 | 説明 |
|---|---|---|
| `type` | ○ | `RE1` / `RE4` / `SPFM_TOWER` / `SPFM_LIGHT` |
| `serial` | RE1/RE4: `index`未指定時に必須 | FT245R/FT2232H のシリアル番号 |
| `port` | SPFM系: 必須 | COMポートまたは `/dev/ttyUSB*` |
| `index` | `serial`/`port`未指定時 | 同種インターフェースの通し番号 |
| `slot` | ○ | チップのスロット番号 |
| `pan` | - | 上書き用。省略時はプロファイル(`fitom_hw_profile.json`)の値を使う |

**注意**: `chip`/`clock`は`HWPlugin_Open()`には不要 (FitomHwIF自身の
`fitom_hw_profile.json`側で確定済みのため)。`auto_devices`使用時は
`HWPlugin_Enumerate()`の返り値に`chip`/`clock`も含まれるが、これは
FITOM_X側が`resolveChipDeviceId()`に使うための情報であり、そのまま
`params_json`として転送されても実機プラグイン側では単に無視される。

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
  "dll": "fitom_midi_rtmidi.dll"
}
```

MIDI入出力を実際に処理するバックエンドDLLを指定する。`hw_plugins[].dll`と
同様、相対パスは実行ファイルのディレクトリを基点とする。

**省略可能**。省略した場合、プラットフォーム既定のファイル名を実行ファイルと
同じディレクトリから探索する。

| プラットフォーム | 既定ファイル名 |
|---|---|
| Windows | `fitom_midi_rtmidi.dll`（RtMidi、WinMM API経由、追加ランタイム不要） |
| Linux | `fitom_midi_rtmidi.so`（RtMidi、ALSAシーケンサAPI経由） |
| macOS | `fitom_midi_rtmidi.dylib`（RtMidi、CoreMIDI経由） |

`backends/midi_rtmidi/`が[RtMidi](https://github.com/thestk/rtmidi)を使い
Windows/Linux/macOSを1つのDLLソースでカバーする（2026年7月、旧
Windows MIDI Services/WinMM/ALSAの3個別実装から統合）。

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
  }
}
```

(HWプラグインDLL・MIDIバックエンドDLL・音声出力設定は、いずれもプロファイル
側(`hw_plugins[]`/`midi_backend.dll`)またはHWプラグイン自身の設定ファイルで
指定する。`fitom.conf.json`はログ設定のみを扱う。)

### profiles/emulator_only.profile.json

```json
{
  "profile_name": "エミュレーターのみ (OPM + OPL3 + SSG)",
  "hw_plugins": [
    {
      "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
      "profile": "fmhwif_profile.json",
      "auto_devices": true
    }
  ],
  "midi_inputs": ["MIDI キーボード"]
}
```
(OPM/OPL3/SSGのチップ構成は`fmhwif_profile.json`側で定義する)

### profiles/studio.profile.json

```json
{
  "profile_name": "自宅スタジオ (SPFM/RE1/RE4 実機 + OPN2エミュ)",
  "hw_plugins": [
    {
      "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
      "profile": "fmhwif_profile.json",
      "auto_devices": true
    },
    {
      "name": "FitomHwIF", "dll": "fitom_hw.dll",
      "profile": "fitom_hw_profile.json",
      "auto_devices": true
    }
  ],
  "midi_inputs": ["UM-ONE"]
}
```
(実機のスロット構成・OPN2エミュのチップ構成は、それぞれ`fitom_hw_profile.json`・
`fmhwif_profile.json`側で定義する。FITOM_X側のプロファイルは「どのプラグインを
使うか」のみを記述し、実際のチップ接続情報は一切持たない)

---

## 起動時の読み込み順序

```
1. fitom.conf.json を読み込む (実行ファイルと同ディレクトリに無ければ
   スキップし、各アプリの既定ログ設定のまま続行する)
   └─ ログ初期化 (Boost.Log、log.level/file/console を反映)

2. プロファイルを読み込む (--profile オプション or デフォルト)
   └─ hw_plugins[] の DLL を HWPluginRegistry に登録する
   └─ devices[] から IPort を生成する (HWPort)
   └─ midi_backend.dll (省略時はプラットフォーム既定) を解決する
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
| `hw_plugins[].dll` が見つからない（HW デバイスあり） | **起動エラー** |
| `midi_backend.dll` が見つからない | **警告ログ**（MIDI 入力なしで継続） |
| WMS Runtime 未インストール（WMS DLL 使用時） | **警告ログ**（MIDI 入力なしで継続） |
