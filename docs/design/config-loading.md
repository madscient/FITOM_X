# 設定の読み込み

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**: 各項目の書き方と意味(利用者向け)は`docs/manuals/profile-reference.md`。スキーマは`config_schema/`。

この文書は、`fitom.conf.json`とプロファイル(`*.profile.json`)をFITOM_Xがどう読み、どう書き戻すかを書く。

---

## 2つのファイルに分ける理由

| 項目 | fitom.conf.json | *.profile.json |
|---|---|---|
| 変える頻度 | 初回のセットアップ時のみ | 曲・現場に応じて切り替える |
| 内容 | ログの設定と、プロファイルを探すディレクトリ | デバイス構成・MIDI入力・バンク・音量 |

HWプラグイン(実機・エミュレータ)と音声出力の設定は、`fitom.conf.json`にもプロファイルにも持たない。HWプラグインはプロファイルの`hw_plugins[]`で指定し、音声出力の設定はプラグインが自分の設定ファイル(`hw_plugins[].profile`)で持つ。FITOM_Xは実機かエミュレータかを区別しないので、その区別に関わる設定を本体に置かない。

## fitom.conf.json

`fitom_cli`と`gui/bridge`(`FITOMBridge::init`)は、実行ファイルと同じディレクトリに`fitom.conf.json`があれば`FITOMConfig::loadSystemConf()`で読み、`getLogLevel()`/`getLogFile()`/`getLogConsole()`の値で`Log::init()`を呼ぶ。ログはBoost.Logを使い、`fitom/Log.h`のマクロで全翻訳単位から使う。

`profiles.root`は、`fitom_gui`のプロファイル選択画面が使う(`FITOMBridge::listAvailableProfiles()`→`FITOMConfig::listProfiles()`、指定ディレクトリの直下だけを見る)。相対パスはカレントワーキングディレクトリを起点にする。

## プロファイルの切り替え

`fitom_gui`の実行中のプロファイルの切り替えは、自分自身を新しいプロセスとして起動し直す方式で行う。コア(`CFITOM`)は実行中の再初期化(`exit()`→`init()`の繰り返し)を想定しておらず、`CFITOM::exit()`はアプリ終了時に一度だけ呼ばれる前提で二重呼び出しを防ぐガード(`exited_`)を持つため、一度`exit()`すると以後の`exit()`は何もしなくなるから。

- 新しいプロセスの起動は、必ずこのプロセスの`bridge.exit()`(MIDI入力ポート・HWプラグイン[実機ならCOMポート/USB、エミュレータならオーディオストリーム]・内部用MIDIパイプの解放を含む)を終えてから行う。これらはプロセス間で共有できない資源なので、順序を逆にすると、新旧のプロセスが同じMIDIポート/HWデバイスを同時に掴もうとする
- 新しいプロセスの起動(`CreateProcess`等)自体に失敗した場合は、`exited_`ガードがリセットされないので、このプロセス内で`init()`し直すことはせず、コア無しの状態のまま利用者に手動の再起動を促す
- **選んだプロファイル自体が読めない場合**は、別の仕組みで復帰する。`fitom_gui`はコマンドラインの第2引数(予備のプロファイル)を受け付け(`fitom_gui <profile.json> [<fallback_profile.json>]`)、切り替えのときは切り替える前のプロファイルを第2引数として新しいプロセスへ渡す。新しいプロセスは、第1引数の読み込みに失敗したら、まだ一度も`init()`していない状態のまま第2引数で`bridge.init()`を試みる。新しいプロセスの中で完結するので、`exited_`ガードの問題は起きない

## 起動時の読み込み順序

```
1. fitom.conf.json を読み込む (実行ファイルと同ディレクトリに無ければ
   スキップし、各アプリの既定ログ設定のまま続行する)
   └─ ログ初期化 (log.level/file/console を反映)
   └─ (fitom_guiでプロファイル未指定の場合のみ) profiles.root配下の
      *.profile.jsonを一覧し、選択画面を表示する

2. プロファイルを読み込む (fitom_cli: 起動引数必須。fitom_gui: 起動引数、
   または上記の選択画面での選択)
   └─ hw_plugins[] の DLL を HWPluginRegistry に登録し、HWPlugin_Init() を呼ぶ
      (auto_devices なら HWPlugin_Enumerate() の結果から devices を作る)
   └─ devices[] から IPort を生成する (HWPort)。part_gains を適用する
   └─ sf2_banks / sf2_channel_windows を読み、構成の矛盾を検査する
   └─ midi_backend.dll (省略時はプラットフォーム既定) を解決する
   └─ midi_inputs を開く (MidiPlugin_OpenIn)

3. HW プラグインが自身の設定に基づき、必要であれば内部で音声出力を開始する
   (FITOM_X本体はこの処理に一切関与しない)

4. MIDI タイマーを起動する
```

## タイマーの刻みは1ms固定

`CFITOM::startTimerThread()`は引数でティックの間隔を受け取れるが、ポルタメントの速度テーブル(`kPortaSpeedTable`、`MidiCh.cpp`)やソフトウェアLFOのレート換算(`rateToTicks()`)は「1ティック=1ms」を前提に較正されているので、設定で変えられるようにはしていない。

## 読み込みで問題があったときの扱い

利用者向けの一覧は`docs/manuals/profile-reference.md`の「読み込みで問題があったとき」。読み込みを中止する(`FITOMConfig`の読み込みが`false`を返す)のは、プロファイルが見つからない・JSONの構文誤り、`sf2_channel_windows`の検証の失敗(`loadSf2ChannelWindows()`)、SF2の構成の矛盾(`chip: "SF2"`のデバイスが2つ以上、または`sf2_banks`/`sf2_channel_windows`があるのに`chip: "SF2"`のデバイスが無い)だけ。それ以外(プラグインのDLLが読めない、`HWPlugin_Init()`の失敗、`devices[].plugin`が未登録、MIDIのポートが見つからない等)は、ログに出してその要素を飛ばし、読み込みを続ける。

`HWPlugin_Init()`が失敗したプラグインは登録しない(`Enumerate`/`Open`を呼べない状態のプラグインを残すのは危険なため)。プラグイン自身の設定ファイルの内容と`devices[]`の記述(`engine`/`chip`/`index`等)が合わない場合は、`HWPlugin_Init()`は成功し、後の`HWPlugin_Open()`が`HW_ERR_NOT_FOUND`を返す。

`channel_map`等の既に使っていないキーは、読み込みでは単に読まないだけで、エラーにしない(スキーマの検証ではトップレベルが`additionalProperties: false`なので誤りになる)。

## 書き戻し

`FITOMConfig::saveProfile()`は、GUIで変えた項目(`midi_inputs`、`master_volume`、`master_pitch`、`part_gains`)だけを書き換え、他のフィールドは読み込んだときのまま残す。

**`part_gains`のデバイスの特定**: デバイスを`label`ではなく、「プラグイン名+`HWPlugin_Open`に渡すparams」で特定する。`label`は省略でき、同じ文字列を複数のデバイスに付けられる(例示用でないプロファイルに実際に重複がある)ため。この方式は、同じparamsのデバイスを同時に2つ開けないことを前提にしている(FitomEmuIFは1チップ1ハンドル)。これが崩れると、paramsでは1台に決まらない。

- 保存するのは、既定値から変えた部位だけ。変えていない部位も含めて全部保存すると、プラグイン側の既定値が変わっても古い値で上書きし続けることになる
- 該当するデバイスが無いエントリも、捨てずに書き戻す(プロファイルを一時的に書き換えただけで設定が消えないように)
- 値は線形の倍率で書く。dBで書かないのは、プラグインとの契約と同じ単位にすれば、無音(0)をそのまま書け、既定値が0の部位も同じ表記で扱えるため
- 書き戻す値は、0.37のような短い10進表記にし、読み直すと同じfloatになるようにしている

## パスの解決基点

| 何のパスか | 起点 |
|---|---|
| `hw_plugins[].dll`、`midi_backend.dll` | 実行ファイルのディレクトリ |
| `banks.*[].file`、`banks`/`bank_overrides`に書いた外部ファイル | プロファイルファイルのディレクトリ |
| `*.pcmbank.json`の中の`adpcm_json`/`bin_file` | その`*.pcmbank.json`のディレクトリ |
| `fitom.conf.json`の`profiles.root` | カレントワーキングディレクトリ |

`banks`系は、どちらも「参照元のファイルの置き場所を起点にする」という考え方で揃えている。プロファイルとバンク一式をどこに置いても、起動時のカレントディレクトリに依存せず同じ結果になる。
