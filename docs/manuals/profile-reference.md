# FITOM_X プロファイル リファレンス

FITOM_Xの設定ファイル(プロファイル`*.profile.json`と、システム設定`fitom.conf.json`)を書く人のためのリファレンスです。

関連するリファレンス:
- バンクファイルの書き方: [バンクファイル リファレンス](./bank-reference.md)
- MIDIメッセージ: [MIDIメッセージリファレンスマニュアル](./midi-message-reference.md)

設定ファイルの形式は`config_schema/`のJSONスキーマ(`profile.schema.json`、`fitom.conf.schema.json`)で検証できます。

---

## 1. 2種類の設定ファイル

| | `fitom.conf.json` | `*.profile.json` |
|---|---|---|
| 内容 | ログの設定と、プロファイルを探すディレクトリ | 使う音源・MIDI入力・バンク・音量等 |
| 置き場所 | 実行ファイルと同じディレクトリ。無くてもよい | 任意 |
| 切り替え | しない | 曲や現場に応じて切り替える |

音声出力の設定(サンプルレート・バッファサイズ等)は、FITOM_Xではなく、HWプラグイン(エミュレータ等)が自分の設定ファイルで持ちます。各プラグインのドキュメントを参照してください。

---

## 2. `fitom.conf.json`

```json
{
  "log": {
    "level": "debug",
    "file":  "fitom.log",
    "console": true
  },
  "profiles": {
    "root": "profiles"
  }
}
```

| フィールド | 既定値 | 説明 |
|---|---|---|
| `log.level` | `"info"` | 出力する最小のログレベル。`trace` / `debug` / `info` / `warning` / `error` / `fatal` |
| `log.file` | `""` | ログファイルのパス。空文字でファイルに出さない |
| `log.console` | `true` | コンソールへ出すか |
| `profiles.root` | `"."` | `fitom_gui`のプロファイル選択画面が`*.profile.json`を探すディレクトリ。相対パスは**カレントディレクトリ**を起点にする(実行ファイルのディレクトリではない) |

`fitom.conf.json`が無い場合は、各アプリの既定値で動きます(`fitom_cli`: level=debug・ファイル`fitom_cli.log`、`fitom_gui`: level=info・ファイルなし。どちらもコンソールに出す)。

---

## 3. プロファイルの読み込みと切り替え

- **`fitom_cli`**: `fitom_cli <profile.json>`。プロファイルは省略できません
- **`fitom_gui`**: `fitom_gui [<profile.json> [<予備のprofile.json>]]`
  - プロファイルを省略すると、`profiles.root`のディレクトリ直下にある`*.profile.json`の一覧が出るので、選んで起動します。一覧に出る名前は、プロファイルの`profile_name`です(無ければファイル名)
  - 2つ目の引数を指定すると、1つ目のプロファイルが読み込めなかった(存在しない・JSONの書式が誤っている等)ときに、代わりにそちらを読み込みます
  - 起動後も、MIDIモニターの上部の「プロファイル切替」から別のプロファイルに切り替えられます。切り替えは`fitom_gui`をいったん終了し、選んだプロファイルで起動し直す形で行われます。選んだプロファイルが読み込めなければ、切り替える前のプロファイルで起動し直します
- 次の項目は、`fitom_gui`の画面で変えてOKを押すと、プロファイルに書き戻されます。他の項目は書き戻しでも変わりません
  - `midi_inputs`(MIDIポート設定ダイアログ)
  - `master_volume`、`master_pitch`、`part_gains`(システム設定ダイアログ)

---

## 4. プロファイルの項目

| 項目 | 必須 | 内容 | 節 |
|---|---|---|---|
| `profile_name` | ○ | プロファイルの表示名 | — |
| `hw_plugins` | | 使うHWプラグイン | 5 |
| `devices` | | 使う音源チップ | 6 |
| `midi_backend` | | MIDI入力を処理するDLL | 7 |
| `midi_inputs` | | 開くMIDI入力ポート | 7 |
| `banks` / `bank_overrides` | | バンクファイル | 10 |
| `psg_fallback_chip` | | PSG系の音色の既定のチップ | 10 |
| `sf2_channel_windows` | | SF2直行パスに送るMIDIチャンネル | 10 |
| `master_volume` / `master_pitch` | | 全体の音量・基準音程 | 9 |
| `part_gains` | | チップの出力ごとの音量 | 9 |

`midi_outputs`はスキーマにありますが、現在のバージョンでは使われません。

---

## 5. HWプラグイン(`hw_plugins`)

FITOM_Xは、実機のチップもエミュレータも、同じ形式のHWプラグイン(DLL)を通して鳴らします。どちらであっても、プロファイルの書き方は同じです。

```json
"hw_plugins": [
  { "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
    "profile": "fmhwif_profile.json", "auto_devices": true },
  { "name": "FitomHwIF", "dll": "fitom_hw.dll",
    "profile": "fitom_hw_profile.json" }
]
```

| フィールド | 必須 | 説明 |
|---|---|---|
| `name` | ○ | `devices[].plugin`から指す名前 |
| `dll` | ○ | プラグインのDLLのパス。相対パスは実行ファイルのディレクトリを起点にする |
| `profile` | | プラグイン自身の設定ファイルのパス。省略すると、プラグインが既定の場所を探す。中身はプラグインが読み、FITOM_Xは解釈しない |
| `auto_devices` | | `true`にすると、プラグインが返すチップの一覧をそのまま`devices`として使う(下記) |
| `auto_devices_rhythm_mode` | | `auto_devices`で作ったデバイスのうち、内蔵リズムを使うチップの名前(`chip`の値)の配列。該当するチップに`rhythm_mode: true`を付けたのと同じになる |

**`auto_devices`**: プラグインが自分の設定ファイルでチップの構成を持ち、それを列挙して返すプラグイン(エミュレータ用のFitomEmuIF等)では、`devices`を書かずに済みます。プロファイルを切り替えると、プラグイン側の設定ファイルも一緒に切り替わります。挿さっているチップを判別できない実機用のプラグインでは使えないので、`devices`を書いてください。`auto_devices`と、同じプラグインを指す`devices`のエントリは併用できます。

プラグインのDLLや設定ファイルが読めない場合、そのプラグインは登録されず、そのプラグインを使うデバイスはすべて使えなくなります(ログにエラーが出ます)。

---

## 6. デバイス(`devices`)

使う音源チップを1つずつ並べます。

```json
"devices": [
  { "if": "HW", "label": "OPM", "plugin": "FitomEmuIF",
    "type": "FMHWIF", "engine": "YMEngine", "chip": "OPM", "index": 0, "pan": 0 },
  { "if": "HW", "label": "OPNA (SPFM)", "plugin": "FitomHwIF",
    "chip": "OPNA", "type": "SPFM_TOWER", "port": "COM3", "slot": 0, "pan": 0 }
]
```

FITOM_X自身が読むフィールド:

| フィールド | 必須 | 説明 |
|---|---|---|
| `if` | ○ | `"HW"`固定 |
| `plugin` | ○ | 使うHWプラグイン(`hw_plugins[].name`) |
| `chip` | | チップの種類。書ける値はREADMEの「対応チップ」の表。`devices`を手で書くときは必ず書く |
| `label` | | 表示名 |
| `rhythm_mode` | | 内蔵リズムを使うか(下記) |
| `extra_slot` | | 2ポートのチップ(OPNA/OPN2/OPL3等)を、2つのスロットに分けて挿した場合の2つ目のスロット番号 |
| `stereo_pair` | | 2台のチップをL/Rの組にする(下記) |

これ以外のフィールドは、FITOM_Xは解釈せず、デバイスを開くときにプラグインへそのまま渡します。何が必要かはプラグインのドキュメントに従ってください。参考として、現在のプラグインが使う主なフィールドは次のとおりです。

| プラグイン | フィールド |
|---|---|
| FitomEmuIF(エミュレータ) | `type`(`"FMHWIF"`固定)、`engine`(エンジン名)、`chip`、`index`(チップの番号)、`pan` |
| FitomHwIF(実機) | `type`(`RE1`/`RE4`/`SPFM_TOWER`/`SPFM_LIGHT`)、`serial`(RE1/RE4)、`port`(SPFM系のCOMポート)、`index`、`slot`(チップのスロット)、`pan` |

`pan`は0=ステレオ、1=Lのみ、2=Rのみ、3=モノラル。

### 内蔵リズム(`rhythm_mode`)

内蔵リズムを持つOPL系(OPL/Y8950/OPL2/OPL3/OPL4/OPL2EX)とOPLL系(OPLL/OPLL2/OPLLP/OPLLX/OPLLEX)のチップは、`"rhythm_mode": true`のときだけ内蔵リズムを使えます。このときチップのch6-8は内蔵リズムに明け渡され、通常の楽音には使われません。`false`(既定)なら、ch6-8は通常の楽音に使います。同じ種類のチップを複数使う場合、`rhythm_mode`はチップごとに指定できます。

OPNA系とDSG(YM2163)の内蔵リズムは、楽音とは別の回路なので、`rhythm_mode`に関係なく常に使えます。

### L/Rの組(`stereo_pair`)

OPLL/OPL/PSG系のように、チップ自体がモノラル出力しか持たない音源で、L/Rに配線した同じチップ2台を1つのステレオの音源として使うための指定です。2台は常に同じ音を鳴らし、パン(CC#10)に応じて左右の音量を振り分けます。

**L側とR側の両方のエントリに`stereo_pair`を書きます。** `pan`を1と2に振り分けるだけでは組にならず、「左に固定された音源」と「右に固定された音源」が別々に並ぶだけです。組になるのは、同じ`chip`・同じプラグインのL側とR側です。

指定は2通りあり、左右をどこで分けるかが変わります。

| 指定 | L/Rの決め方 | 左右を分ける場所 | `pan` |
|---|---|---|---|
| `"stereo_pair": true` | `pan`の1(L)/2(R) | プラグインの出力 | L側`1`、R側`2` |
| `"stereo_pair": "L"` / `"R"` | この値 | チップ自身のL/R出力 | 両方`0` |

```json
"devices": [
  { "if": "HW", "label": "OPLL", "plugin": "FitomEmuIF", "type": "FMHWIF",
    "engine": "YMEngine", "chip": "OPLL", "index": 0,
    "pan": 1, "rhythm_mode": true, "stereo_pair": true },
  { "if": "HW", "label": "OPLL", "plugin": "FitomEmuIF", "type": "FMHWIF",
    "engine": "YMEngine", "chip": "OPLL", "index": 1,
    "pan": 2, "rhythm_mode": true, "stereo_pair": true }
]
```

- `true`の方式は、モノラル出力しか持たないチップ(OPLL系等)でも使えます
- `"L"`/`"R"`の方式は、チップ自身がL/R出力を持つものだけで使えます: OPM/OPP・OPZ/OPZ2・OPL3・OPL4(FM部)・OPN2/OPNA/OPNB/OPNBB系とそのADPCM-A/ADPCM-B。それ以外に指定すると、読み込み時に警告が出ます。プラグイン側の`pan`を0のままにできるので、同じチップの他の部分(OPL4のAWM部等)が自分のパンをそのまま使えます
- OPNAのSSG部のように、チップの中にL/Rを分ける仕組みが無い部分は、`"L"`/`"R"`の方式でも左右に分かれません(パンを振ると、定位ではなく音量だけが変わります)
- 片方が`"L"`/`"R"`、もう片方が`true`という混ぜた指定は、`true`の方式として扱います
- 内蔵リズムやSSG等を持つチップも、まとめて組になります。チップごとのパンを持つ部分(OPL4のAWM部、YMZ705/YMZ732/YMZ771のSSG部・ADPCM部、Y8960の拡張OPL2部・拡張OPLL部)は組にならず、それぞれ独立した音源として残ります。このため、「OPL4のFM部」と「OPL3」を組にすることもできます
- プラグイン側にも、L/R2台分のチップを用意してください
- 組になると、ログに`mergeStereoPairDevices: '...' (R) merged into '...' (L) as CLinearPanDevice pair`が出ます(末尾の`[chip-level L/R]`/`[plugin-routed L/R]`で、どちらの方式かが分かります)

---

## 7. MIDI(`midi_backend` / `midi_inputs`)

```json
"midi_backend": { "dll": "fitom_midi_rtmidi.dll" },
"midi_inputs":  ["MIDI キーボード", "UM-ONE"]
```

- **`midi_backend.dll`**: MIDI入力を処理するDLL。相対パスは実行ファイルのディレクトリを起点にします。省略すると、実行ファイルと同じディレクトリの`fitom_midi_rtmidi.dll`(Windows)/`.so`(Linux)/`.dylib`(macOS)を使います
- **`midi_inputs`**: 開くMIDI入力ポートの名前。最大4つで、`i`番目のポートが「MPU i」になり、それぞれが独立した16チャンネルを持ちます。空文字列`""`は、そのMPUにポートを割り当てないことを表します(例: `["", "UM-ONE"]`はMPU0を空け、MPU1にUM-ONEを割り当てます)。名前はOSのMIDIポートの名前と一致させます。見つからないポートは警告を出して飛ばします

---

## 8. MIDIチャンネルの初期状態

- 各MPUのMIDIチャンネル10はリズムチャンネル、他はメロディチャンネルとして始まります。演奏中にバンクセレクト(CC#0=0x78/0x79)で切り替えられます([MIDIメッセージリファレンスマニュアル](./midi-message-reference.md)の「4.1 チャンネルの役割切替」)。プロファイルで初期の割り当てを変えることはできません
- 起動時、全チャンネルにバンク0・プログラム0が選ばれた状態になります。Program Changeを送らずにNote Onを送っても鳴ります
- チャンネルごとの同時発音数は、選んだ音色が使うチップのチャンネル数で決まります

以前のバージョンの`channel_map`は廃止されました。書いても読み込みでは無視されますが、スキーマの検証では誤りになります。

---

## 9. 音量と音程

```json
"master_volume": 100,
"master_pitch": 440.0
```

| 項目 | 範囲 | 既定値 | 説明 |
|---|---|---|---|
| `master_volume` | 0–127 | 100 | 全体の音量。SF2直行パスの音量と、音声を出力するHWプラグイン(エミュレータ等)の出力に掛かります。チップのレジスタには反映しないので、実機のチップの音量は変わりません(HWプラグインが対応していない場合も変わりません) |
| `master_pitch` | 430.0–450.0 | 440.0 | 基準となるA音の周波数(Hz) |

### チップの出力ごとの音量(`part_gains`)

チップによっては、音を複数の出力から別々に出します(OPNAのFMとSSG等)。この出力の1つ1つを**部位**と呼びます。実機ではボード上の回路で混ぜるので、音量のバランスは機種によって違います。`part_gains`は、そのバランスをデバイスごとに持ちます。部位を持つのは、部位ごとの音量に対応したHWプラグインのデバイスだけです。

`fitom_gui`のシステム設定ダイアログで変えると書き戻される項目で、手で書く必要は通常ありません。

```json
"part_gains": [
  {
    "plugin": "FitomEmuIF",
    "params": { "type": "FMHWIF", "engine": "YMEngine", "chip": "OPNA", "index": 0 },
    "gains":  { "SSG": [0.5, 0.5] }
  }
]
```

| フィールド | 内容 |
|---|---|
| `plugin` | 対象デバイスのHWプラグイン(`hw_plugins[].name`) |
| `params` | 対象デバイスを特定する値。`devices`のエントリから`if`/`label`/`plugin`/`extra_slot`/`rhythm_mode`/`stereo_pair`を除いた残り(`auto_devices`のデバイスは、プラグインが返したエントリそのもの)。キーの並び順は問いませんが、値は完全に一致する必要があります |
| `gains` | 部位の名前 → `[L, R]`。倍率で、1.0 = 0 dB |

- 書かれるのは、プラグインの既定値から変えた部位だけです。書かれていない部位は既定値のまま鳴ります(既定値はプラグインが決め、1.0とは限りません)。既定値に戻した部位は、次の書き戻しで消えます
- デバイスは`label`ではなく`plugin`と`params`で特定します(`label`は同じものを複数のデバイスに付けられるため)
- `devices`の書き方を変えると、`params`が一致しなくなり、そのエントリは効かなくなります。読み込み時に警告`part_gains: 該当するデバイスがありません`が出ます。該当するデバイスが無いエントリも、消さずにそのまま書き戻します

---

## 10. バンク

### `banks`

バンクファイルを並べます。各配列の書き方は[バンクファイル リファレンス](./bank-reference.md)の「1. バンクファイルの種類と読み込み」を参照してください。

```json
"banks": {
  "hw_banks": [
    { "group": "OPM", "bank": 0, "file": "banks/OPM/fb01/rom1.hwbank.json" }
  ],
  "sw_banks": [
    { "bank": 0, "file": "banks/sw/default_32.swbank.json" }
  ]
}
```

**別のファイルに分ける**: `banks`に文字列を書くと、そのパスのファイルの中身(`hw_banks`等を持つオブジェクト)が、その位置に書かれたものとして読み込まれます。パスはプロファイルのディレクトリを起点にします。

```json
{ "banks": "common.bankset.json" }
```

バンクの構成(バンク・プログラムの番号の割り当て)はデバイスの構成に依存しないので、デバイスの構成が違う複数のプロファイルで同じバンクのファイルを共有できます。デバイスが無いチップ向けのバンクは、鳴らないだけです。

### `bank_overrides`

共有した`banks`のうち、一部のバンクだけをプロファイルごとに差し替えたり足したりするときは、トップレベルに`bank_overrides`を書きます。書き方は`banks`と同じです(オブジェクトでも、ファイルのパスの文字列でもよい)。各配列の要素は、次のキーが一致すれば`banks`側のエントリを置き換え、一致しなければ末尾に足します(消すことはできません)。

| 配列 | 一致を見るキー |
|---|---|
| `hw_banks` | `group` + `bank` |
| `drum_banks` | `prog` |
| `pcm_banks` | `bank` + `chip` |
| `sf2_banks` / `sw_banks` / `patch_banks` / `scc_wave_banks` | `bank` |

```json
{
  "banks": "common.bankset.json",
  "bank_overrides": {
    "hw_banks": [
      { "group": "OPN", "bank": 0, "file": "profile_specific/opn_bank0_variant.hwbank.json" }
    ]
  }
}
```

この例では、`common.bankset.json`のOPNのバンク0だけをこのプロファイル用のファイルに差し替え、他のバンクは共有のまま使います。

### `psg_fallback_chip`

PSG系の共有バンクの音色のうち、鳴らすチップ(`ext.target_voice_patch_type`)を指定していないものを、どのチップで鳴らすか。`"SSG"`(既定) / `"EPSG"` / `"DCSG"` / `"SAA"` / `"SCC"`。

### SF2直行パス(`banks.sf2_banks` / `sf2_channel_windows`)

SoundFont音源(FitomSf2IFプラグイン)を使うときの設定です。仕組みは[MIDIメッセージリファレンスマニュアル](./midi-message-reference.md)の「2.6 SF2直行パス」を参照してください。

- `devices`に`"chip": "SF2"`のデバイスを1つだけ置きます(SoundFontプラグインを指すもの)。SF2ファイルの一覧はFITOM_Xが`sf2_banks`から作ってプラグインへ渡すので、`devices`のエントリには書きません
- **`banks.sf2_banks[]`**: `{ "bank": CC#32の値, "file": SF2ファイル, "sf2_bank": ファイル内のバンク番号(0–128) }`。3つとも必須です。同じファイルを複数のエントリから指してかまいません
- **`sf2_channel_windows[]`**: `{ "mpu": 0–3, "ch": 0–15, "fluidsynth_chan": 0–15 }`。SF2へ送るMPU×チャンネルと、SoundFont音源側のチャンネルの対応です。16件まで、`fluidsynth_chan`の重複は不可。演奏中はプライベートSysExで変えられます

---

## 11. 読み込みで問題があったとき

| 状況 | 扱い |
|---|---|
| プロファイルが見つからない、JSONの書式が誤っている | **読み込みを中止** |
| `sf2_channel_windows`が16件を超える・重複・値が範囲外 | **読み込みを中止** |
| `chip: "SF2"`のデバイスが2つ以上ある | **読み込みを中止** |
| `sf2_banks`か`sf2_channel_windows`があるのに、`chip: "SF2"`のデバイスが無い | **読み込みを中止** |
| `hw_plugins[]`のDLLや設定ファイルが読めない | エラーをログに出し、そのプラグインを使うデバイスを飛ばして続ける |
| `devices[].plugin`が`hw_plugins[]`に無い | エラーをログに出し、そのデバイスを飛ばして続ける |
| `midi_inputs[]`のポートが見つからない | 警告を出し、他のポートで続ける |
| `midi_backend.dll`が読めない | エラーをログに出し、MIDI入力なしで続ける |
| `part_gains[]`に該当するデバイスが無い | 警告を出し、エントリは消さずに書き戻す |
| `part_gains[]`の書式が誤っている、プラグインが値を受け付けない | 警告を出し、その値だけ飛ばす |

---

## 12. 例

エミュレータだけを使う(チップの構成はプラグインの設定ファイルで決める):

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

実機とエミュレータを混ぜる:

```json
{
  "profile_name": "自宅スタジオ (SPFM 実機 + OPN2エミュ)",
  "hw_plugins": [
    {
      "name": "FitomEmuIF", "dll": "FitomEmuIF.dll",
      "profile": "fmhwif_profile.json",
      "auto_devices": true
    },
    { "name": "FitomHwIF", "dll": "fitom_hw.dll" }
  ],
  "devices": [
    { "if": "HW", "label": "OPM", "plugin": "FitomHwIF",
      "chip": "OPM", "type": "SPFM_TOWER", "port": "COM4", "slot": 0 }
  ],
  "midi_inputs": ["UM-ONE"]
}
```

例示のプロファイルは`config/profiles/`にもあります。
