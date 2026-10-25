# render_measure: `fitom_render`で鳴らして測る

`fitom_render`でSMFをWAVに変換し、numpyで波形を測るスクリプト群。
`docs/STATUS.md`の3節(未確認事項)のうち、音程・音量・発音の有無のように
波形から数値で決まる項目を、人が聴かずに確かめるために使う。

測れるのはエミュレータ(FitomEmuIFのエンジン)の音だけ。音色の良し悪しや、
GUIの表示は測れない。

## 必要なもの

- Python 3 と numpy
- `fitom_render`(このリポジトリのビルド)
- FITOM_staging(プロファイル・バンク・`bin/engines/`のエンジンDLL)
- オフライン描画(`HWPlugin_InitOffline`/`HWPlugin_Render`)に対応したFitomEmuIF.dll。
  FITOM_stagingの`bin/`のものが古い場合は、FitomEmuIFのビルドを`--emuif`で渡す

## スクリプト

### `fm_pitch.py` — FM系チップの音程

```
python fm_pitch.py --staging <FITOM_staging> --render <fitom_render.exe>
                   [--emuif <FitomEmuIF.dll>] [--work <dir>] [--note 69]
                   [--rate 48000] [--only <文字列>] [--tolerance 5]
```

FITOM_stagingのプロファイルが参照するFitomEmuIFのサブプロファイルから、FM系チップを
1個ずつ取り出して鳴らし、指定したノート(既定A4)の基音を測る。

- チップごとに、そのチップだけを載せたプロファイルを`--work`に作る。エンジンとクロックは
  サブプロファイルのものをそのまま使う。`master_pitch`は440Hz
- 音色は直接デバイス選択(CC#0)で選んだうえで、プライベートSysExのHwPatchの上書きで
  「全オペレータMUL=1・デチューン0・フィードバック0、キャリア1つだけ」に変える。元の音色の
  MULやデチューンで基音がずれないようにするため。SwPatchの上書きでビブラートも止める
- 基音は、Hann窓・ゼロ詰めのFFTのピークを放物線補間して求める。純音なら1セントより十分細かい
- 期待値からのずれ(セント)が`--tolerance`以内で、音が出ていれば合格。不合格の行に`<-- NG`が付き、
  不合格が1件でもあれば終了コード1

`--work`を省くと一時ディレクトリを作る。中に、チップごとのプロファイル・SMF・WAVと、
`fitom_render.log`(最後に変換したもの)が残る。

### `pitch_controls.py` — 音程を動かす操作

```
python pitch_controls.py --staging <FITOM_staging> --render <fitom_render.exe>
                         [--emuif <FitomEmuIF.dll>] [--work <dir>] [--tolerance 5]
```

チップごとにA4を鳴らし、ピッチベンド(ベンド幅2半音で+1/+2/-2半音)・ファインチューン(RPN#1で±50セント)・
`master_pitch`(430/450Hz)・ビブラート(SwPatchのLFO、CC#1)を1つずつ変えて、基音のずれを
何もしないときの基音と比べる。比べる相手が同じチップの基音なので、サンプルの元の音程が
分からないPCM系(OPL4 AWM)でも測れる。測り方の確認のために、音程が正しいと分かっている
OPN2を並べている。

ビブラートは、短い区間ごとの基音の列から振れ幅(2〜98パーセンタイルの半分)を求め、深さの0.5〜1.5倍なら合格。
`docs/STATUS.md`の4節に載っている既知の未対応(AWMのマスターピッチとビブラート)は、測って
`(unsupported)`と表示し、不合格に数えない。

### `rhythm_pitch.py` — OPLL系の内蔵リズムの音程

```
python rhythm_pitch.py --staging <FITOM_staging> --render <fitom_render.exe>
                       [--emuif <FitomEmuIF.dll>] [--work <dir>] [--tolerance 5]
```

内蔵リズム(CC#0=0x70、CC#32=0x28)のSD/TOMを、ノート番号を変えて鳴らす。スペクトルの最大の山を、
同じノートのメロディ(基音だけの音色)の基音で割った比が、ノートによらず一定(楽器のMUL)かを見る。
リズムがメロディ側と同じF-numberで鳴っていれば一定になる。

スペクトル全体の相互相関でずれを求める方法は使わない。SDのノイズが強いエンジンでは、
ずれないノイズの成分で相関の山が決まってしまう。

### `presence.py` — 鳴るか(と、音程のある音なら A4 の音程)

```
python presence.py --staging <FITOM_staging> --render <fitom_render.exe>
                   [--emuif <FitomEmuIF.dll>] [--work <dir>] [--only <文字列>] [--tolerance 5]
```

チップ・部位ごとに1音鳴らし、発音直後0.3秒の RMS が -60dB を超えるかを見る。`CASES`の表に、
サブプロファイル・チップ・内蔵リズムの有無・選ぶ音色(CC#0/CC#32/プログラム)・ノート・期待する基音・
比べる相手を並べる。比べる相手を書いた場合は、相手とのスペクトルの最大の山の差(セント)を出す
(同じ波形を別のエンジンで鳴らして、再生の速さが揃っているかを見る)。

### `levels.py` — 音量を動かす操作

```
python levels.py --staging <FITOM_staging> --render <fitom_render.exe>
                 [--emuif <FitomEmuIF.dll>] [--work <dir>] [--only <文字列>]
```

CC#7・ベロシティ・マスターボリューム(ユニバーサルSysEx)を127〜16に振り、発音直後の RMS(直流分を除く)を
値127のときからの dB で求め、GM2 の`40·log10(v/127)`dBと比べる。チップの音量レジスタの刻みより細かくは
合わないので、刻みの半分+0.5dBを許す。比較用の FM は OPNA。ymfm の OPN2(YM2612)は DAC の不連続を模していて、
小さい音でひずみが-30dB付近に残るので使わない。FM のベロシティは音色の VTL で効き方が決まるので測らない。

### `behavior.py` — モノ・レガート・チャンネルの再利用

```
python behavior.py --staging <FITOM_staging> --render <fitom_render.exe>
                   [--emuif <FitomEmuIF.dll>] [--work <dir>]
```

OPNAとOPM(pc88のサブプロファイル)で、次を測る。モノの3つはレガートのオン・オフの両方で測る。

- モノ(CC#126=1)で最初のノートが鳴る
- モノで前のノートを押さえたまま、鳴らすチップが変わる音色へプログラムチェンジすると、次のノートが新しい
  チップで鳴り、前のノートが残らない。どちらのチップで鳴ったかは、OPNAを左だけ・OPMを右だけに定位させて見分ける
- LFS=0のビブラートの音色を弾き直しても、2秒を過ぎてビブラートが続く
- 減衰する音色でAからBへつなぐと、レガートオンではBの頭で音量が跳ね上がらない(オフでは跳ね上がる。対照)
- ポリで6チャンネルを埋めてから1つを離し、2秒以内に次のノートを鳴らす(リリース中のチャンネルが必ず再利用
  される)と、LFS=0のビブラートが2秒を過ぎても続く

### `epsg_expanded.py` — AY8930の拡張モードのバンク切り替え

```
python epsg_expanded.py --staging <FITOM_staging> --render <fitom_render.exe>
                        [--emuif <FitomEmuIF.dll>] [--work <dir>]
```

3音を同時に鳴らして3チャンネルを全部使い、デューティ比(50%・25%・12.5%)を変えて、各ノートの基音の音程と、
第2倍音と基音の比をパルス波の理論値と比べる。ch0はBank A、ch1/ch2はBank Bにデューティ比などのレジスタを
持つので、バンクの切り替えを誤ると、デューティ比が効かないか音程が崩れる。

## `fitom_measure.py` — 部品

SMFの組み立て(直接デバイス選択・HwPatch/SwPatchの上書きのSysExを含む)、`fitom_render`の実行、
WAVの読み込み(16/24bit整数・32bit浮動小数)、基音とRMS(直流分を除く)の測定、FITOM_stagingの
サブプロファイルから単体チップのプロファイルを作る処理(内蔵リズムを使う指定を含む)、
ピッチベンド・RPN・短い区間ごとの基音の列、1エンジンに複数のチップを載せたプロファイルを作る処理を持つ。
新しい測定はこれを使って書く。

## 測り方が不具合を見分けられるか

新しい測定を足したら、確かめたい修正を一時的に外して(またはわざと壊して)測り、不合格になることを確かめる。
合格するだけでは、測り方が正しい証拠にならない。

測定用のプロファイルは、FITOM_stagingのバンクセットを絶対パスに直した写しを使う。
バンクセットの中の相対パスはプロファイルの位置から解決されるため。SF2のバンクは、
SF2のデバイスを載せないプロファイルでは設定エラーになるので外している。
