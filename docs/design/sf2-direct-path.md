# SF2直行パス(SoundFont音源との連携)

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**:
- 利用者向けの仕様: `docs/manuals/midi-message-reference.md`の「2.6 SF2直行パス」「8.2 SF2直行パス チャンネル窓の動的割り当て」
- SoundFontプラグインとの約束: `docs/interfaces/plugin-hwif.md`の「SoundFont プラグインへの追加の約束」

---

## 概要

SF2直行パスは、MIDIチャンネルの一部を、FITOM_XのFM/PSG向けの処理を通さずにSoundFont音源(fluidsynth)へ送る経路である。

- fluidsynth(`fluid_synth_t`)は、専用のHW I/Fプラグイン FitomSf2IF(別リポジトリ)の中だけにある。FITOM_X本体はfluidsynthにリンクしない
- FITOM_Xは、このプラグインを他のHW I/Fプラグインと同じ`IHWPlugin`契約で扱う。MIDIバイト列を`HWPlugin_WriteBlock`で渡すだけで、`IHWPlugin.h`に関数を足していない
- どのMIDIチャンネルをSF2へ送るかは、MPU×chの単位で「窓」として割り当てる。CC#0の値では切り替えない
- 窓の中でのFITOM_Xの仕事は、チャンネル番号の付け替えとバンク選択の解決だけで、発音状態を持たない(「パッチベイに徹する」節)

## なぜ`ISoundDevice`の1実装にしないか

`ISoundDevice`は、少数の物理チャンネルをFITOM_XのDVA(`allocCh`/`queryCh`)が奪い合って割り当て、1物理チャンネルで1音を鳴らす、というFMチップの制約を前提にした抽象化である。`noteOn(ch, vel)`は、どの物理chを使うかをFITOM_X側が決め切っている前提のシグネチャになっている。

fluidsynthは逆に、16chそれぞれが内部でポリフォニーを管理し、1chに何音重ねるかをfluidsynth自身が決める。インターフェースの意味のレベルで噛み合わないため、`ISoundDevice`とは別の、メロディ・リズムに並ぶ第三の経路として設けている。

## プラグインへの閉じ込め

`fluid_synth_t`はFitomSf2IFだけが持ち、プロファイルの`devices[]`に`chip: "SF2"`のエントリを1つ置くことで、FITOM_Xからの送り先が1つに決まる。

- 重い外部ライブラリへの依存はHW I/FプラグインDLLの側に閉じ込める、というFITOM_X全体の方針に沿う
- 実機かエミュレータかを区別しない`IHWPlugin`契約のまま扱える。SF2を使えるかどうかが、特定のFMエミュレータプラグインを使うかどうかに左右されない
- 音声出力はプラグインの中で完結する。FITOM_Xは複数のHW I/Fプラグインの音声をミックスしないので、SF2の音とFM系エミュレータの音を1つのストリームにまとめることはできない。これはSF2に限らず、複数のプラグインを併用するときに共通の制約である

## 窓

窓は、MPU×ch から fluidsynth のチャンネル(`fluidsynth_chan`、0–15)への対応表である。窓に含まれるMPU×chのメッセージは、`MidiProcessor`が`CInstCh`/`CRhythmCh`へ振り分けるより前に`CFITOM::routeSf2ChannelMessage()`へ渡り、`PatchManager`による音色の解決もDVAも通らない。

- **窓が要る理由**: `fluid_synth_t`はチャンネル番号そのものにバンク・プログラム・ピッチベンド・発音中のノートといった状態を持つ。FMチップの物理チャンネルのような、奪い合うだけの発音資源ではない。複数のMPU(最大4面×16ch)をそのままfluidsynthのチャンネルへ対応させると、別のMPUの同じチャンネル番号どうしが状態を取り合う。そこで対応を明示的な表として持ち、`fluidsynth_chan`の重複を許さない
- **メロディとリズムの区別は関係ない**: 窓はMIDIチャンネルの単位で、役割の区別(CC#0=0x78/0x79)より手前で決まる。SF2側で打楽器を鳴らすには、パーカッション用のバンク(SF2のbank 128等)を指す`sf2_banks`のエントリを選ぶ
- **窓に含まれないMPU×ch**は、これまでどおりCC#0の値によるネイティブの経路を通る。2つの経路は、CC#0の値ではなく窓に含まれるかどうかだけで分かれる
- **GUIからの送出も同じ経路を通る**: `MidiProcessor::sendControlChange()`/`sendProgramChange()`/`sendNoteOn()`/`sendNoteOff()`(GUIからの構造化送出)も、実MIDI入力と同じく窓を確かめてから振り分ける

窓の設定は2通りある。中身は同じ表である。

- **プロファイルのトップレベル`sf2_channel_windows`**: 起動時の初期値。`banks`の下ではなくトップレベルに置くのは、`banks`がバンクの中身への参照であるのに対し、これはMIDIチャンネルの経路の設定で、性質が違うため。読み込み時に、16件を超える・`fluidsynth_chan`が重複する・値が範囲外、のいずれかがあればプロファイルの読み込みを中止する(`FITOMConfig::loadSf2ChannelWindows()`)
- **プライベートSysEx sub-cmd 0x04**: 演奏中の割り当てと解除。`CFITOM::setSf2ChannelWindow()`が処理し、既に別のMPU×chに割り当てられている`fluidsynth_chan`への割り当て要求は、既存の割り当てを外さずに要求ごと無視する
  - `mpu`の特殊値: `0x7E`=このSysExを受信したMPU自身(内部用MIDIパイプで受信した場合はMPUが無いので無視)、`0x7F`=すべてのMPU。「すべて」は各MPUへ1件ずつ`setSf2ChannelWindow()`を呼ぶだけなので、割り当ては重複の検査によって最初の1つのMPUでしか成功せず、解除はすべてのMPUで成功する。この非対称は検査を特別扱いしないことの帰結として受け入れている
  - `ch`の特殊値`0x7F`(すべてのチャンネル)は解除のときだけ有効。割り当てで指定しても範囲外のチャンネルとして無視される
  - ペイロードは固定長のバイナリにしている。プライベートSysExのsub-cmd 0x01/0x02がJSONを使うのは、多数のフィールドの一部だけを上書きしたいからで、3つの小さな値しか持たない窓には当たらない

## バンクとプログラムの選択

SF2ファイルは1つの中に複数の(bank, preset)の組を持つ。CC#32の値をそのままSF2内のbank番号として渡すと、複数のSF2ファイルを使い分けられず、パーカッションの慣習であるbank 128もCC#32(0–127)では表せない。そこで`hw_banks`と同じく、プロファイルの`banks.sf2_banks`で間接参照する。

- `sf2_banks[]`の各エントリは`bank`(CC#32の値)・`file`(SF2ファイル)・`sf2_bank`(ファイル内のbank番号、0–128)の3つを必須とする。`file`の相対パスは、他の`banks.*[].file`と同じくプロファイルのディレクトリを起点に解決する
- 窓の中でCC#32を受けると、FITOM_Xが`sf2_banks`から`bank`の一致するエントリを引き、その窓の状態として覚える。一致するエントリが無ければ何もしない
- プログラムチェンジを受けると、解決済みのファイルとbank番号を、プライベートSysEx sub-cmd 0x05としてプラグインへ送る(書式は`docs/interfaces/plugin-hwif.md`)。ファイルはパスではなく`soundfont_index`(後述)で指す。SysExのデータは7bitでなければならず、非ASCIIを含みうるファイルパスを演奏中のメッセージに載せられないため
- CC#0はプラグインへ送らない。CC#32とプログラムチェンジによる明示的な選択と二重に状態を持ち、食い違うおそれがあるため
- CC#32がまだ一度も解決されていない窓では、プログラムチェンジも捨てる。ネイティブ経路の「直前の音色を保つ」ような救済は、FITOM_Xが状態を持つ経路だからできることで、ここではしない。`sf2_banks`のエントリは引けたが、そのbankやプログラムにプリセットが無い場合は、sub-cmd 0x05をそのまま送り、あとはfluidsynthの挙動に任せる

## パッチベイに徹する

窓の中でFITOM_Xがメッセージを解釈するのは、CC#0を捨てること、CC#32を`sf2_banks`で解決すること、チャンネル番号を付け替えることだけである。ノートの所有権や発音状態は追跡しない。

- 窓を割り当て直したり解除したりしても、FITOM_XはAll Notes Off等の後始末も、バンク・プログラム・ボリューム等の再送もしない。古いチャンネルでノートが鳴り続けることがあるが、実物のMIDIパッチベイでケーブルを繋ぎ替えたときと同じ現象として受け入れる。後始末は演奏する側の責任とする
- fluidsynthの同時発音数の上限と、上限に達したときの処理も、fluidsynthに任せる
- 窓に含まれるがSF2のデバイスが開けていない(プラグインのロードに失敗した等)場合は、メッセージを捨てる。ネイティブ経路には戻さない

## マスターボリュームとマスターピッチ

`CFITOM::setMasterVolume()`/`setMasterPitch()`は、値が変わるとGM2のUniversal Realtime SysEx(マスターボリューム、マスターファインチューニング)を組み立ててSF2のデバイスへ送る。マスターピッチの範囲(430–450Hz)は440Hzから±100セントに収まるので、ファインチューニングだけで足りる。FITOM_Xが外部から受信するのと同じ標準のメッセージなので、新しいプロトコルを作っていない。

## デバイスの登録と起動時の検査

- `chip: "SF2"`は`resolveChipDeviceId()`で`DEVICE_NONE`になり、`ISoundDevice`は作られない。`FITOMConfig::buildDevice()`は`ISoundDevice`の有無と無関係に`HWPlugin_Open`でポートを開くので、窓のメッセージはこのポート(`CFITOM::sf2Port_`)へ送る。`CFITOM::initDevices()`は、未知の`chip`のときに出す警告を、`SF2`には出さない
- `soundfonts`(プラグインへ渡すSF2ファイルの一覧)は、利用者が`devices[]`に書くのではなく、`sf2_banks[].file`から重複を除いてFITOM_Xが作り、`params_json`に足す。二重管理を避けるため
- `Sf2BankRegistry`は1回の`sf2_banks`の走査から、CC#32を解決する表と、`soundfonts`の一覧(と`file`→`soundfont_index`の表)の両方を作る。2つを別々に作ると食い違うおそれがあるため。`soundfont_index`は`sf2_banks`の中で各ファイルが初めて現れた順の連番。`Sf2BankRegistry`は`devices[]`を作る前に構築する
- `soundfonts`は読み込み時に決まり、演奏中には変わらない(`sf2_banks`を変えるSysExは無い)
- 次の構成は、プロファイルの読み込みを中止する
  - `chip: "SF2"`のデバイスが2つ以上ある(送り先が1つに決まらない)
  - `sf2_banks`か`sf2_channel_windows`に設定があるのに、`chip: "SF2"`のデバイスが無い(送り先が無い)
- 逆に、`chip: "SF2"`のデバイスはあるが`sf2_banks`も`sf2_channel_windows`も空、という構成は許す
- `type`・`serial`・`port`・`slot`・`clock`・`pan`等、FM/PSGチップ向けのフィールドはSF2のデバイスには書かない

## プリセット名

MIDIモニターやパッチ選択の画面でプリセット名を出すため、FITOM_XはSF2ファイルの`phdr`(プリセットヘッダ)だけを自前で読む(`fitom::parseSf2PresetHeaders()`)。SoundFont 2のRIFF構造のうち`LIST/pdta`の`phdr`だけを読み、音声データ(`LIST/sdta`)はチャンクの大きさだけ読み飛ばす。

- `Sf2BankRegistry`が、ファイルを初めて見たときに1回だけ読み、`(sf2_bank, program)`→プリセット名の表を作る(`resolvePresetName()`/`resolvePresetNameByIndex()`)
- プラグインには問い合わせない。プラグインが動いていなくても名前を出せる
- 名前が引けないときは、他のモードと同じく番号を表示する

## GUI

- **MIDIモニター**: 窓に含まれるMPU×chでは、`CInstCh`/`CRhythmCh`による解決をせず、Bank列にSF2ファイル名、Program列にプリセット名、Device列に`fluidsynth_chan`、Fnumber列に最後に送ったNote Onのバイト列(チャンネル付け替え後)を出す。表示のために、`CFITOM`が窓ごとに最後に送った値だけを持つ(`CFITOM::getSf2ChannelWindowInfo()`)。発音状態は追跡しないという方針は変えず、レジスタダンプモニターのシャドウレジスタと同じく「最後に送った値」の写しとして持つ
- **CH設定ダイアログ**: チャンネル単位で窓をON/OFFするチェックボックスと`fluidsynth_chan`の選択を持つ。初めてONにしたときは、使われていない`fluidsynth_chan`を`CFITOM::listAssignedSf2Windows()`から探して提案する。リズムチャンネルの切替と排他で、OKを押すまで窓の割り当ては送らない
- **パッチ選択**: `Sf2PatchPickerDialog`(Bank→Programの2階層)。OKを押すまで窓が割り当てられず、音を送る先が無いので、試聴はしない

## 見送った案と理由

- **fluidsynthを`ISoundDevice`の1実装として組み込む案**: 「なぜ`ISoundDevice`の1実装にしないか」節の理由
- **既存のFMエミュレータプラグイン(FitomEmuIF等)に組み込む案**: 複数のプラグインを併用する構成で、どのインスタンスが唯一の`fluid_synth_t`を持つかが曖昧になる。そのプラグインを使わない構成(実機だけ等)ではSF2が使えなくなる
- **FITOM_X本体がfluidsynthに直接リンクする案**: 重い外部ライブラリへの依存をプラグインDLLに閉じ込める方針の例外になる
- **`IHWPlugin.h`にソフトシンセ用の関数(`HWPlugin_SoftSynthNoteOn`等)を足す案**: `HWPlugin_WriteBlock`でMIDIバイト列を渡せば足りる
- **CC#0の特定の値(127)でSF2直行パスへ切り替える案**: fluidsynthのチャンネルが状態を持つので、複数のMPUをCC#0だけで振り分けると状態が衝突する。窓の方式は、Roland CM-64がLA音源部とPCM音源部へSysExでMIDIチャンネルを割り当てる方式を参考にした
- **CC#32をSF2内のbank番号としてそのまま渡す案**: 「バンクとプログラムの選択」節の理由
- **窓の割り当てに動的なスロットプールを使い、切替時に状態を再送する案 / 切替時にAll Notes Off相当の後始末だけはする案**: いずれもFITOM_Xに状態を持たせることになる
- **sub-cmd 0x04のペイロードをJSONにする案、窓の表全体を一括で置き換える案**: 3つの小さな値で、一部だけを上書きする必要が無い
- **プリセット名をプラグインへ問い合わせる案**: 問い合わせ用の関数が`IHWPlugin.h`に要り、プラグインが動いていないと使えない
- **プリセット名を利用者がサイドカーのJSONに手で書く案**: SF2ファイルにある情報を二重に管理させることになる
- **プリセット名を出さず番号だけにする案**: `phdr`を読む手間が小さいため採らなかった
