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

## `fitom_measure.py` — 部品

SMFの組み立て(直接デバイス選択・HwPatch/SwPatchの上書きのSysExを含む)、`fitom_render`の実行、
WAVの読み込み(16/24bit整数・32bit浮動小数)、基音とRMSの測定、FITOM_stagingの
サブプロファイルから単体チップのプロファイルを作る処理を持つ。新しい測定はこれを使って書く。

測定用のプロファイルは、FITOM_stagingのバンクセットを絶対パスに直した写しを使う。
バンクセットの中の相対パスはプロファイルの位置から解決されるため。SF2のバンクは、
SF2のデバイスを載せないプロファイルでは設定エラーになるので外している。
