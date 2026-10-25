# チップファミリー別の実装メモ

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**:
- 全チップに共通する仕組み: `docs/design/chip-drivers.md`
- 利用者向けの音色パラメータの意味と値域: `docs/manuals/hwpatch-reference.md`
- HW I/Fプラグインへ渡すレジスタアドレスの規則: `docs/interfaces/plugin-hwif.md`

この文書は、チップファミリーごとのクラス構成、レジスタの扱い、実装上の注意を書く。音色パラメータの利用者向けの意味はマニュアルを正とし、ここにはドライバがそれをどのレジスタへどう書くかを書く。

---

## OPN系

```
COPN : CSoundDevice                    (YM2203, 3ch, 単体使用)
COPNA : CSpanDevice                    (内部に COPN×2 を保持、6ch)
  ├── COPN2 : COPNA                    (YM2612系、ADPCM/FMenableレジスタなし)
  └── COPNB : COPNA                    (YM2610無印、ch0/ch3を無効化し実効4ch)
COPNARhythm : CSoundDevice             (OPNA内蔵リズム、6パート、独立レジスタ体系)
```

- `COPNA`は`chip1_`(port1,ch0-2)+`chip2_`(port2,ch3-5)の2つの`COPN`サブチップを
  `CSpanDevice`で束ねる。ポート2のキーオンは`OPN2Port2`ラッパーがレジスタ`0x28`を
  インターセプトしてビット2を立てた上でport1に転送する（`0x28`はポート1専用の
  グローバルレジスタのため）。
- **`COPNB`**：実機YM2610無印はYM2612/YM2608(6ch)からADPCM制御回路のために
  各サブチップの先頭ch(グローバルch0とch3)を差し引いた実効4ch構成
  (有効なグローバルchは1,2,4,5)。コンストラクタと`init()`の両方で
  `enableCh(0, false)`/`enableCh(3, false)`を呼ぶ(`COPNA::init()`内の
  `reset()`で毎回再有効化されるため、`COPL3_2`と同じパターンで再無効化が
  必要)。SSG/ADPCM-A/ADPCM-Bは
  サブデバイスの自動生成により同一物理ポートを共有する別デバイス
  として生成される。
- 音色のフィールド: `hw.FB`(3bit)、`hw.ALG`(3bit)、`hwOp[0-3]`のAR/DR/SL/SR/RR/TL/KSR/MUL/DT1、`hwOp[i].EGT`(SSG-EG、4bit。OPN/OPNA系のみ)。OPN2/OPNA系の`hw.AMS`/`hw.PMS`はハードウェアLFOに未対応のため発音に効かない
- **F-Numberの分周はYM2203とそれ以外で違う**: FM部の1サンプルはYM2203が72クロック、YM2608/YM2612/YM2610が144クロック。`COPN`の`fnumDivide`は単体なら72、`COPNA`のサブチップなら144。クロックはポートが報告する値を使う(`docs/design/chip-drivers.md`の「クロック」)

### OPN FXモード

`COPN`のch2専用機能。実機OPNの「4オペレータそれぞれに独立したF-numberを
指定できるモード」（3rd channel special mode）を実装している。

`ext.FIX`（チャンネル単位、ch2全体で1モードを共有）で選択したモードに応じて、`FmHwOp::PDT`(int16_t、オペレータ単位)の
解釈が変わる：

| `ext.FIX` | モード | `PDT`の意味 |
|---|---|---|
| 0 | 通常 (FXモード無効) | 未使用 |
| 1 | 疑似デチューン | kfs単位の符号付きオフセット (`getFnumber(ch, PDT)`) |
| 2 | 非整数倍率 | kfs単位のオフセット (倍率→セント換算。`1200×log2(倍率)`) |
| 3 | 固定周波数 | 0.1Hz単位の絶対周波数 (`getFnumberFromHz(PDT/10.0)`) |

対数周波数空間では「セント単位の加算」＝「周波数の倍率」と等価なため、
疑似デチューン・非整数倍率は同じ`PDT`型・同じ`getFnumber(ch,offset)`計算経路で
統一的に扱える。固定周波数モードのみ、ノート番号を無視して直接Hz値を指定する
別経路 (`getFnumberFromHz`) を使う。

`CSoundDevice::getFnumberFromHz()`は、`FnumUtils.h`のテーブル生成式（`val = freq×(2^17/master)×divide`）
をそのまま任意のHz値に適用し、11bit(0-2047)に収まるまでオクターブ正規化する。

実機OPNのFXモードは「チップのch2」にのみ存在する物理的制約があるため、
`COPN`のコンストラクタは`fxCapable`パラメータを持つ。単体`COPN`(YM2203)は
`true`固定。`COPNA`/`COPN2`（内部に`COPN`を2つ保持するCSpanDevice構成）は
前半サブチップ(port1、ch0-2)のみ`true`、後半サブチップ(port2、ch3-5)は
`false`（実機に該当レジスタが存在しないため）。`queryCh`もFXモード要求時に
ch2固定を強制する（`fxCapable_`なチップのみ、`updateVoice`/`updateFreq`と統一）。

### F-numberの書き込み

`COPN::updateFreq()`/`updateFxModeFreq()`は、F-numberの上位バイト(0xA4+ch、Block+Fnum上位3bit)→下位バイト(0xA0+ch)の順に、どちらも`forceWrite=true`で書く。

- 近い音程(C3→D3等)では上位バイトの値が前回と一致することが多い。シャドウとの比較で上位バイトの書き込みを省くと、チップ側で音程が更新されない
- 順序を下位→上位に入れ替えると、最初のノートオンが鳴らず以降の音程も狂う。上位→下位が正しい
- どちらの書き込みでF-numberが確定するかは、確かめていない

## OPM系

```
COPM : CSoundDevice                    (YM2151, 8ch)
  ├── COPP : COPM                      (YM2164、制御ロジック共通)
  └── COPZ : COPM                      (YM2414、WS/DT3/REV/EGS拡張あり)
```

- 音色のフィールド: `hw.FB`/`hw.ALG`/`hw.AMS`/`hw.PMS`/`hw.NFQ`、`hwOp[0-3]`のAR/DR/SL/SR/RR/TL/KSR/MUL/DT1/DT2/AM(`DT2`はOPM実機通り2bit)。いずれも実機レジスタに直接対応する
- **ノイズ(ch7専用)**: `ext.ALG_EXT`のbit0=1でノイズを有効にし、`queryCh`がch7固定を強制する
- **ハードウェアLFO**: `setLFODepth()`/`setLFORate()`を実装しているのは`COPM`だけ(`docs/design/midi-processing.md`の「ハードウェアLFO」)。OPZの第2LFOには対応していない(`docs/proposals/opm-opz-hw-lfo.md`)
- **OPZのオペレータ拡張**: `COPZ`は`hwOp[i].REV`(3bit)/`hwOp[i].EGS`(2bit)をレジスタ`0xC0+slot`の裏レジスタ(bit5=1で選択、オペレータ単位)に書く。`hwOp[i].WS`(3bit、OPZ独自波形)、`hwOp[i].DT3`(補助デチューン、OPZのratio mode)もオペレータ単位
- **キーコード**: KCの下位4bitの音名は、C#=0・D=1・D#=2・E=4・F=5・F#=6・G=8・G#=9・A=10・A#=12・B=13・C=14(3・7・11・15は使わない)。`kKeyCode`の添字はC#起点の半音で、3.579545MHzではA4(ノート69)がKC=0x4A・KF=0になるように`kMasterTuneBase`を決めている(ymfmの`opm_key_code_to_phase_step`の表で、KC=0x4Aは439.94Hz)
- **クロック**: KC/KFは3.579545MHzで鳴らす前提の`kMasterTuneBase`から求め、ポートが報告するクロックとの比(1オクターブ768ステップで`768×log2(3579545/clock)`)だけ`masterTune_`をずらす。同じKC/KFなら音程はクロックに比例するため
- **固定周波数(`ext.FIX`)**: `COPM::updateVoice()`は`ext.FIX`が0でないときレジスタのbit5を立て、DT2を書かない。OPZの固定周波数モードとして働くかは確かめていない
- **OPZはキーオンと出力イネーブルのレジスタ割り当てがOPMと異なる**ため、
  `COPZ`は`updateKey`/`updatePanpot`を独自にオーバーライドする。

  | レジスタ | OPM (YM2151) | OPZ (YM2414) |
  |---|---|---|
  | `$08` | キーオン (bit6-3=スロットマスク / bit2-0=ch) | チャンネル選択。書き込むと`$E0-$FF`がプリセットメモリから復元される |
  | `$20+ch` bit7 | R出力イネーブル | R出力イネーブル |
  | `$20+ch` bit6 | L出力イネーブル | **キーオン** |
  | `$30+ch` bit0 | 未使用 | 出力イネーブル（実効値は`$20`bit7とのOR） |

  この制約から、`COPZ::updateKey`の書き込み順序は
  `$08` → `$E0-$FF`（ダンプ/サステイン） → `$20+ch` に固定される。
  `$08`を後に書くと設定したRRがプリセット値へ戻され、`$20`を先に書くと
  チャンネル選択前の書き込みとなってキーオンとして解釈されない。
  `$20`は同一チャンネルの再トリガーでシャドウレジスタによる重複抑止に
  よってエッジが消えないよう`forceWrite`で書く。
  OPZにはL単独定位を表すビットが無いため、`COPZ::updatePanpot`は左寄せを
  両出力（センター相当）として扱う。回帰テストは`tests/test_opz_keyon.cpp`。

## OPL系

```
COPL : CSoundDevice                    (YM3526, 9ch, 2OP)
  ├── C3801 : COPL                     (YM3801(Y8950)のFM部。COPLとの違いはデバイス種別と名前だけ)
  └── COPL2 : COPL                     (YM3812, 2OP)
        └── COPL2EX : COPL2            (Y8960の拡張OPL2部、chごとの定位つき)
COPL3 : CSoundDevice                   (4OPモード専用, 6ch)
COPL3_2 : CSpanDevice                  (内部にCOPL2×2、2OP残余6ch)
COPLRhythm : CSoundDevice              (OPL系内蔵リズム、5パート)
  └── COPL2EXRhythm : COPLRhythm       (Y8960の拡張OPL2部の内蔵リズム)
```

- 音色のフィールド: `hw.FB`(3bit)、`hw.ALG`(bit0のみ。0=FM/1=AM)、`hwOp[0-1]`のAR/DR/SL/SR/RR/TL/KSR/KSL/MUL/AM/VIB、`hwOp[i].WS`(OPL2以降、2bit)。`hwOp[i].EGT`はOPL系では使わない(実機のEGビットは`SR`から決まる)
- **`TL`は等倍で書く**: `TL`の1段は全チップ共通で0.75dBで、OPL系の実機TLレジスタ(6bit、最大47.25dB)とOPN/OPM系(7bit、最大95.25dB)の違いはレンジだけ。OPL系へは縮めずにクランプして書く(`TL >> 1`にすると1段が1.5dBに読み替わり、減衰量が半分になる)
- **B0レジスタのF-number上位は2bit**: `getFnumber()`は11bit精度の値を返すので、`fnum.fnum >> 9`は0-3の2bitになりうる。B0レジスタのbit1-0(Fnumber bit9-8)へマスクせずにそのままORする(`& 1`でマスクすると、1オクターブの上半分程度でbit9が落ち、音程が大きく狂う)。`COPL`/`COPL3`/`COPLRhythm`のいずれも同じ
- **クロックはYM3812換算で受け取る**: `COPL`/`COPL3`/`COPL3_2`/`COPLRhythm`は、どれも「72クロックで1サンプル」の換算(`divide`=72)でFnumberを計算する。YMF262(288クロック)とYMF278BのFM部(684クロック)には、`DeviceFactory`がチップのクロックを換算して渡す(`docs/design/chip-drivers.md`の「クロック」)。換算を4OP・2OP・リズムのサブデバイスで揃えるため、`COPL3`も`COPL`と同じ`divide`を使う。ポートがクロックを報告しないときは`COPL::kDefaultClock`(3579545Hz)
- **F-number上位バイトは`forceWrite=true`で書く**: B0レジスタにKONビットが同居し、キーオンのたびに値が変わる

### EGTとRRの動的な書き換え

`COPL`/`COPL3`/`COPLRhythm`は、キーオン/キーオフのたびに`updateKey()`がEGTビットとRRレジスタを書き換え、`SR`をキーオン中の減衰レートとして働かせる。

- `updateVoice`(発音チャンネル確保時に1回呼ばれる): `SR>0`かどうかで
  暫定的にEGT/RRを書く。直後の`updateKey`呼び出しで無条件に上書きされる仮の値に過ぎない。

  ```cpp
  // EGTビット: SR>0ならパーカッシブ(0)、SR==0ならサステイン(0x20)
  ((o.SR > 0) ? 0 : 0x20)
  ```

- `updateKey`(キーオン/キーオフのたびに呼ばれる): **`SR`が0かどうかに
  関わらず**、その時点のキーオン/キーオフ状態だけでEGT/RRを切り替える。
  キーオン中は常にEGT=0(パーカッシブ)にして`SR`の値(4bit変換、`SR=0`
  なら0)をRRレジスタへ、キーオフ時は常にEGT=1(サステイン)にして`RR`の
  値をRRレジスタへ書く(サステインペダルON中のキャリアのみ、キーオフ時に
  RR=4へフォールバックする例外あり)。

  ```cpp
  // EGTビット: キーオン中は常にパーカッシブ(0)、キーオフ時は常にサステイン(0x20)
  (keyOn ? 0 : 0x20)
  // RRレジスタ: キーオン中はSRを4bit変換(SR=0なら0)、キーオフ時はRRを4bit変換
  (keyOn ? ar4(o.SR) : o.RR)
  // ar4: 5bit→4bit変換(上位4bitを採用)
  static uint8_t ar4(uint8_t v) { return v >> 1; }
  ```

`updateVoice`→(キーオン時)`updateKey`の順で必ず呼ばれるため、実際に
発音中に有効なのは`updateKey`が書いた値である。`SR==0`の音色でも
キーオン中はRRフィールド=0のパーカッシブモードになる点に注意(実機の
RR=0はほぼ減衰しないため、聴感上は`updateVoice`が想定するサステイン
モードに近い結果になるが、レジスタ上の状態(EGTビット)は異なる)。

**EGTビットの0/1の向き**: EGT=0がdecay(KON中もRELEASE RATEが効き続ける)、EGT=1がsustained(KON中はSUSTAIN LEVELを保持する)。YM3526・YM3812・Y8950・YMF262・YM2413の各データシートはこの向きで一致している。YMF278(OPL4)のアプリケーションノートの「PERCUSSIVE SOUND/NON-PERCUSSIVE SOUND」節は逆に読める書き方をしているが、こちらが誤り。EGTの向きを見直すときは、複数のデータシートを突き合わせ、ビット名ではなく波形図の挙動(KEY ONを保持したままSLで平坦になるか、RRで減衰し続けるか)で判断すること。

OPLL系はこの動的な書き換えをしない(「OPLL系」節)。

### OPL3の4OPモード

`COPL3`は各ポートch0-2(3ch×2ポート=6ch)を4OP専用として使う。`COPL3_2`は残りのch6-8(3ch×2ポート)を2OPとして使う（`enableCh`でch0-5を無効化）。

- `hw.ALG`の3bitを{bit0:前半ペアの接続, bit1:後半ペアの接続, bit2:ConnectionSEL(4OP結合有効化)}のパック値として使う。`updateVoice`の0x104(CONNECTIONSEL)レジスタ書き込み・`carmsk[8]`テーブル・`updateKey`のいずれもこのbit2を参照する
- `hw.FB`は前半ペア(M1/C1)、`hw.FB2`は後半ペア(M2/C2)の独立したフィードバック(実機は前半・後半で別レジスタを持つ)
- **キーオンを後半ペアへも送るのはbit2=0のときだけ**: 実機YMF262データシート(B0-B8レジスタ節)は「4OP結合中、後半チャンネル自身のBxレジスタ(Key-On/Block/F-Number)は無視され、前半チャンネルの値のみが4オペレータ全体に使われる」と明記している。したがってbit2=1(4OP結合)では前半ペアのみに送れば足り、bit2=0(前半・後半が独立2OPペア×2)のときこそ両方に送る(キーオフ時は後片付けのため常に両方送る)
- **キャリアの判定(`carmsk[8]`)**: `hw.ALG`の値ごとのキャリアOPのビットマスク。bit2=0側(インデックス0-3、独立2OPペア×2)は前半ペアと後半ペアのマスクをOR合成した`{0xa,0xb,0xe,0xf}`、bit2=1側(インデックス4-7、4OP結合)は`{0x8,0x9,0xa,0xd}`(Nuked-OPL3の`OPL3_ChannelSetupAlg`と突き合わせ済み)
- **`VOICE_PATCH_OPL3`(0x30)は`COPL3`(4OP)専用**。`COPL3_2`(2OP)は独立した`VOICE_PATCH_OPL3_2`(0x22)を持つ。実機OPL3の2opモードはWSが3bit(8波形)まで使えるのに対し実機OPL2はWSが2bit(4波形)までしか対応せず、データフォーマットが異なるため

### 疑似デチューン

`COPL3`は`hwOp[0].PDT`/`hwOp[2].PDT`(int16_t、kfs単位)を、前半/後半ペアそれぞれの`getFnumber(ch,offset)`のオフセットとして使う(OPNのFXモードの疑似デチューンと同じフィールド・同じ計算式)。`DT1`/`DT2`はOPL系の実機に相当する機構が無いため使わない。

基底クラス`CSoundDevice::updateFnumber`は、通常のノート番号ベースのFnum(疑似デチューン無し)を計算した後、`updateFreq(ch,&fnum)`という形でその結果を引数として渡してくる。`COPL3::updateFreq`がこの引数をそのまま使うと疑似デチューンが常にバイパスされるので、`COPL3`は`updateFnumber`をオーバーライドし、基底クラスを呼ぶ前に疑似デチューンを計算して`pseudoDT1_[ch]`/`pseudoDT2_[ch]`にキャッシュしておき、`updateFreq`は渡された`fn`引数を無視して常にこのキャッシュを参照する。

## OPLL系

```
COPLL : CSoundDevice                   (YM2413, 9ch, 2OP)
  ├── COPLL2 : COPLL                   (YM2420、Fnumberレジスタ配置が独自)
  ├── COPLLP : COPLL                   (YMF281B)
  ├── COPLLX : COPLL                   (YM2423-X)
  ├── CVRC7 : COPLL                    (FS1001、maxChs=6でリズム回路自体を持たない)
  └── COPLLEX : COPLL                  (Y8960の拡張OPLL部)
COPLLRhythm : CSoundDevice             (OPLL内蔵リズム、5パート、独立レジスタ体系)
  └── COPLLEXRhythm : COPLLRhythm      (Y8960の拡張OPLL部の内蔵リズム)
```

- 音色のフィールド: `hw.FB`、`hwOp[0-1]`のAR/DR/SL/SR/RR/KSR/KSL/MUL/AM/VIB/WS。`hwOp[1].TL`(キャリア)は音量として`$30`下位4bit(3dB/step)に、`hwOp[0].TL`(モジュレータ)はユーザー音色レジスタ`$02`下位6bit(0.75dB/step)に反映される
- **プリセット/ユーザー音色**: `ext.ALG_EXT`のbit0=1でプリセット音色(ROM、EGパラメータ変更不可)、0でユーザー音色。プリセットのときは`hw.ALG`がプリセット番号になる
- **Fnumberのビットシフト**: `getFnumber()`は11bit精度を返すが、実機OPLLは9bit。`>>2`変換が必須。`COPLL2`はさらに独自のビット配置を持つため`updateFreq`を個別にオーバーライドしている
- **クロック**: ポートが報告するクロックで計算する(`divide`=72)。報告が無いときは`kOpllDefaultClock`(3579545Hz)

### EGTとRRの静的な変換

`updateVoice`でユーザー音色を書き込むときに、`SR`の値だけでEGTビットとRRレジスタの組を確定させ、`updateKey`はキーオンビットの操作だけを行う。

| 音色データ | EGTビット(reg 0x00/0x01のbit5) | RRレジスタ(reg 0x06/0x07の下位4bit) |
|---|---|---|
| `SR > 0` | 0 (decay) | `SR >> 1` |
| `SR == 0` | 1 (sustained) | `RR` |

EGT=0側でRRレジスタに`RR`ではなく`SR`由来の値を書くのは、EGT=0の音色
データではRRフィールドが未使用（レジスタイメージ由来では0のことが多い）で
あり、そのまま書くと減衰しなくなるため。
プリセット音色はROMのためEGパラメータ変更自体が不可で、対象外。

**キーオフ後の減衰**: `SR>0`の音色はEGT=0のままキーオフするので、キャリアは
RRレジスタに関係なく速さ7(RR=7と同じ)で減衰し、`SR`・`RR`フィールドはどちらも
キーオフ後の速さに効かない。`SR=0`の音色(EGT=1)はRRレジスタ(`RR`)の速さで
減衰する。キーオフ後のモジュレータは減衰せず、キーオフ時点のレベルで止まる。
(**確認済み**: Nuked-OPLL v1.0.2のYM2413モデルで、`OPLL_EnvelopeGenerate()`の
速さの選択を読み、キャリアの出力とモジュレータの内部EGレベルを測った。実チップでは未確認)

**OPL系(`COPL`/`COPL3`)と方式が違う理由**：実機OPLLのEG挙動は
OPL系と異なり、発音中のEGTビット書き換えが期待どおりに効かない
（動的制御を入れた状態では音が想定どおりに減衰しない）。FITOM_Xとは別の環境での実チップの検証でこの差異を確かめたため、**OPLL系は静的変換を確定仕様とする**。
チップファミリー間の一貫性という原則に対する意図的な例外であり、
「OPL系に合わせて動的制御へ戻す」方向の変更は行わないこと
（実機挙動の差異が理由であり、実装上の未整備ではない）。OPLL/OPLL2/OPLLP/OPLLX/VRC7/OPLLEXは`COPLL`を共有するため全て同じ挙動。

この理由はNuked-OPLL(v1.0.2、YM2413モデル)の挙動とは合わない。Nuked-OPLLでは、
発音中に書き換えたEGT・RRはその時点から効き、キーオン時にEGT=0、キーオフ直前に
EGT=1と`RR`を書けば、キャリアはOPL系の動的な書き換えと同じ減衰になる(**確認済み**:
上と同じ方法で測った)。実チップとNuked-OPLLのどちらがこの点で正しいかは未確認。

静的変換には、実機OPLLのレジスタイメージから起こした音色を元の鳴り方のまま
再現する、という性質もある。元がEGT=0(パーカッシブ)の音色は、元のレジスタ
イメージのまま鳴らしてもEGT=0でキーオフするので、静的変換のキーオフ後の減衰
(速さ7)と一致する(実チップでも速さ7かは上のとおり未確認)。
動的な書き換えにすると、この種の音色はキーオフ後に`RR`の速さで減衰するように
変わる(FITOM_stagingの`banks/OPLL/opll_presets.hwbank.json`では125音色中69音色が
これに当たり、いずれも`RR`=元のRRレジスタ値を持つ)。

動的な書き換えで得られるのは、ユーザー音色のキャリアのキーオフ後の減衰を`RR`で
決められることだけである。ROM音色はEGパラメータを書けないので対象外で、
キーオフ後のモジュレータは書き換えに関係なく止まる。この得るものの小ささから、
上の食い違いがあっても静的変換を続ける。

EGTビットの0/1の向きはOPL系と同じ(「EGTとRRの動的な書き換え」節)。

## 内蔵リズム

OPL系(`COPLRhythm`)とOPLL系(`COPLLRhythm`)の内蔵リズムは、5つの楽器(BD/SD/TOM/CYM/HH)をFM本体のch6-8の3ch分のレジスタで鳴らす。

- **`COPLLRhythm`のレジスタ**: レジスタ`0x0E`（キーオン）、`0x36-0x38`（音量、2パート/レジスタでnibble共有）を独立して操作する。親`COPLL`インスタンスへの参照を持たず、同一の物理ポートを共有していることを利用して直接`setReg`する(レジスタアドレス空間はどちらのオブジェクトが書いても同じ効果になる)
- **パート番号**: `COPLLRhythm`は音色データの`hw.ALG`(下位3bit)でパート番号を受け、`queryCh`がその値で特定のchを強制する(内蔵リズム音源専用バンク[CC#0=0x70]経由では、`PatchManager`が解決したチャンネルを`forcedCh`として渡す)。`COPLRhythm`は`ext.rhythm_ch`でパートを指定する(`docs/design/patch-resolution.md`の「内蔵リズム音源専用バンク」)
- **音程**: `COPLLRhythm`・`COPLRhythm`は、ノート番号に応じてFnum/Blockレジスタを計算し、内蔵ドラム音のピッチシフト演奏を可能にしている(基底クラス`CSoundDevice::getFnumber()`の標準的なノート→Fnum変換をそのまま使う)。`COPNARhythm`は各チャンネルが固定ピッチの打楽器を鳴らすだけで、ノート番号に依存する処理は無い
- **共有チャンネル**: 5つの楽器に対し3ch分のFnumレジスタしかなく、HH/SD(ch7)、CYM/TOM(ch8)が同じレジスタを共有する。HH・CYMはノイズ性の発振でFnumがほぼ発音に寄与しないため、HH・CYM側はFnumを更新しない。結果として、共有チャンネルのFnumはSD・TOM側が排他的に制御する。BDはch6を単独で使う
- **共有チャンネルの音色レジスタ**: `COPLRhythm`のFB/ALG/Pan(0xC0+物理ch)は物理ch単位のレジスタで、ch7(HH/SD)・ch8(TOM/CYM)は共有するため、後に書いた方が勝つ(仕様として許容している)
- Y8960の拡張ブロックの内蔵リズムが持つ定位も、F-numberと同じくSD/TOMだけが書く(「Y8960 の拡張ブロック」節)

## PSG系

```
CPSGBase : CSoundDevice                (ソフトウェアEG/ソフトウェアLFO制御の共通化のみ)
  ├── CSSG : CPSGBase                  (AY-3-8910/YM2149, 3ch)
  │     └── CSSGS : CSSG               (YMZ705/YMZ732 SSG互換部, 3ch×2ユニット = 6ch)
  │           ├── CSSGS3 : CSSGS       (YMZ771 SSG互換部, 6ch。レジスタ配置のみ差し替え)
  │           └── CSSGEX : CSSGS       (Y8960 SSG部, 6ch。レジスタ配置と定位を差し替え)
  ├── CEPSG : CPSGBase                 (AY8930, 3ch。拡張モードを常時使う)
  ├── CDCSG : CPSGBase                 (SN76489, 4ch)
  ├── CSCC : CPSGBase                  (SCC/SCCP/SCCEX, 5ch, 波形メモリ)
  └── CSAA1099 : CPSGBase              (SAA1099, 6ch)
```

- **`CPSGBase`の設計原則（重要）**：ハードウェア固有の機能は一切持たず、
  「ソフトウェアエンベロープ（`SoftEnvelope`、実機FM相当のADSR）」と
  「ソフトウェアLFO（振幅LFOのみ。`lfoTL_`共有状態を更新して仮想`updateVolExp()`
  を呼ぶだけ）」の制御を共通化するに留める。`updateVolExp`・
  `updateFreq`・ノイズ周波数LFO・`queryCh`（ch2優先）は**SSG(CSSG)固有**の
  実装なので`CSSG`側に置く。`resetLfoBaseline(ch)`（`lfoTL_[ch]=64`初期化）のみが真に共通のヘルパーとして
  `CPSGBase`に残る。
- **バンク切り替えを持つチップは、内部シャドウでバンクを分離する**：
  AY8930の拡張モードのように、同一の8bitアドレスをモードレジスタのビットで
  多重化するチップは、`CSoundDevice::regBak_`も`HWPort`のシャドウも8bit
  アドレスだけをキーにするため、そのままでは両バンクが同じスロットを
  共有してしまう。これは表示だけの問題ではなく、①`setReg()`の同値スキップ
  (`forceWrite=false`)が別バンクの値と比較されて必要な書き込みが消える、
  ②`getReg()`のread-modify-writeが別バンクの値を読む、という実害を出す。
  `CEPSG`は`kBankB`(=0x10)を足したアドレスで内部シャドウを「32レジスタ」として持ち、
  `writeBank()`が「必要ならバンク選択を出してから、チップへは8bitアドレスで
  送る」形にしている。この番号体系はAY8930のエミュレータ実装が一般に
  採る表現(MAMEのay8910_deviceも register_latch + (mode&1)<<4)と一致する。
  オフセットはチップの1バンク分のレジスタ数に合わせること(余分に大きく
  取るとレジスタダンプに空白行が並ぶだけになる)。
  レジスタダンプへは`ISoundDevice::getBankedRegisterView()`でこの内部
  シャドウを渡す(非対応チップは`false`を返し、HWPortのシャドウが
  使われる)。この`0x10`オフセットはドライバ内部の表現で、HW I/Fプラグインへは常に元のアドレス+バンク選択の形でしか送られない。
- **PSG系のマスタークロックは`IPort::getClock()`から取る**：`DeviceFactory::create()`の
  `sampleRate`引数はHW I/Fプラグインが報告する**音声出力のサンプルレート**
  (既定44100)であり、チップクロックではない。FM系・ADPCM系と同じく
  `port->getClock()`を使い、取得できない場合のみ`sampleRate`へフォールバック
  すること(`docs/design/chip-drivers.md`の「クロック」)。さらにOPN系
  (YM2608/YM2610/YM2610B)の内蔵SSG部は実機仕様上φM/4で動作するため、
  `DeviceEntry::clockDivider`(サブデバイスの展開時に`SubDeviceSpec`から設定)で
  分周してから渡す。単独チップのSSGはポートが報告するクロックがそのまま
  SSG部のクロックなので分周しない。
- **`CSSG`はレジスタアドレスを仮想の索引メソッド経由で決める**：YMZ705(SSGS)の
  ようにYM2149相当のSSGブロックを複数内蔵するチップに備え、`CSSG`のレジスタ
  アクセスは`unitBase(ch)`(=`(ch/3)*0x20`)と`unitCh(ch)`(=`ch%3`)を必ず経由する。
  単体のYM2149(`maxChs_`=3)では`unitBase()`が常に0・`unitCh()`が`ch`そのもの
  になるため、アドレスは1バイトも変わらない。ノイズ発生器・HWエンベロープ・
  ミックスレジスタはいずれもユニット内3chで共有される実機構造なので、
  ノイズ音色の`queryCh`も「各ユニットの最終ch(ch2/ch5)を順に探す」形に
  一般化してある。
  さらに、実アドレスは`toneReg`/`noiseReg`/`mixReg`/`volReg`/`envPeriodReg`/
  `envShapeReg`の6つの仮想メソッドに集約してある。**ビットフィールドの意味は
  YM2149と同じでアドレスの並べ方だけが違うチップは、これらを差し替えるだけで
  対応できる**(YMZ771/SSGS3が実例)。逆に言うと、これらを経由せず生アドレスを直書きすると派生チップが
  黙って壊れるので、新しいレジスタを足すときも必ず索引メソッドを追加すること。
- PSG系は全チップが共通の入口(`VOICE_PATCH_SSG`=0x40固定)/HwBank名前空間を共有する(`docs/design/patch-resolution.md`の「PSG系共有バンク」)

### 共通のフィールド

| フィールド | 意味 |
|---|---|
| `hwOp[0].AR/DR/SL/SR/RR` | ソフトウェアエンベロープ(HW EG未使用時、または実機にHW EG機構が無いチップで使用) |
| `hwOp[0].TL` | 基準音量(ベロシティ・CC#7/11等と合成される最終ラウドネス計算の起点) |
| `hwOp[0].EGT` (bit3=0x08) | HWエンベロープ使用フラグ(1=HW EG、0=ソフトウェアエンベロープ)。DCSG/SCCは実機にHW EG機構が無いため常に無視される |

### 音量レジスタの変換則

PSG系は音量レジスタの性質がチップごとに違うため、共通化できない。対数DACのチップだけ`fitom::linear2dB()`を通し、リニア乗算のチップは最終ラウドネスを線形にスケールする。

| チップ | 実機の性質 | 変換 |
|---|---|---|
| SSG / EPSG | 対数DAC (SSG=3dB/step、EPSGは5bitで1.5dB/step) | `linear2dB()` |
| DCSG | 対数の減衰量 (**2dB/step**、0-14=0〜-28dB、15=消音) | `kGM2dB`から直接換算(`linear2dB()`は0.75dBの2のべき乗倍しか刻めない) |
| SCC / SAA | **リニア乗算** | ラウドネスを線形にスケール |

リニア乗算のチップに`linear2dB()`を通すと、意図した減衰量がまったく出ない(-24dBのつもりが実際は-6.6dBにしかならず、実効ダイナミックレンジが0〜-23.5dBへ圧縮された上で無音へ落ちる)。

### トーン周期の算出

PSG系の周波数レジスタは F-number ではなく**周期**であり、音程が上がるほど
値が小さくなる。`CSoundDevice::getFnumber()`が行う11bit F-number前提の正規化（bit11が立っていたら右シフトしてblockを
繰り上げる／`block>7`をクランプして不足分をfnumの左シフトで補う）を通すと
周期そのものが壊れる。`CPSGBase`はテーブル値とオクターブを素通しする
周期テーブル専用の`getFnumber()`を持ち、実際のスケーリングは
`tonePeriod(fn, mask, chipShift)`が「テーブルの基準オクターブ
（`(noteOffset_ + 69*64)/768`）と実際のノートのオクターブの差」で行ってから、チップのレジスタ幅へクランプする。
シフト量を`block+3`のような定数で書いてはならない（`FNUM_OFFSET`を
変更した瞬間に全PSG系のピッチが1オクターブずれる）。

| テーブル型 | 生成式 | 使用チップ |
|---|---|---|
| `FnumTableType::SSG` | `master / (16 * freq)` | SSG(YM2149/AY-3-8910)、AY8930 |
| `FnumTableType::TonePeriod` | `master / (32 * freq)` | DCSG(SN76489)、SCC |

分周比がテーブルと異なるチップは`tonePeriod()`の`chipShift`で差分を吸収
する(AY8930のExpand Modeは1/8分周のため`chipShift=-1`)。

### ソフトウェアエンベロープ (SoftEnvelope)

`CPSGBase`は、AY-3-8910/YM2149のような
限定的なHWエンベロープしか持たないPSG系チップのため、独自のソフトウェア
ADSRエンベロープジェネレータ（`SoftEnvelope`クラス）を実装している。

- パラメータ範囲はFM音源(OPN)と同一 (AR/DR/SR: 5bit 0-31、SL/RR: 4bit 0-15)
- ATTACKフェーズは乗算的減衰（実機FM音源特有の凹型カーブ）
- DECAY/SUSTAIN/RELEASEフェーズは加算的増加（dB空間で線形＝指数的減衰）
- ベロシティ感度（VAR〜VRR）は他のFMチップと同じ仕組みをそのまま適用
- HW EG使用時（`EGT & 0x08`）はソフトウェアエンベロープを使わず、
  チップのハードウェアエンベロープに完全に委ねる

### SSG (YM2149/AY-3-8910) — `CSSG`

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x07 (ミックス、bit[ch]=トーン/bit[ch+3]=ノイズ、Active Low) | 0=トーンのみ/1=ノイズのみ/2=両方/3=両方無効(消音)。ノイズ要求時は`queryCh`がユニット最終ch(単体SSGならch2)へ固定する |
| `hw.NFQ` (5bit) | 0x06 (ノイズ周波数) | ノイズ要求時のみ書き込まれる |
| 音量(計算値) | 0x08-0x0A (ch0-2、下位4bit) | 0=無音/15=最大、48dB/3dBステップで変換 |
| Fnum | 0x00-0x05 (ch0-2、各2バイト、下位/上位) | 周期テーブル(`master/(16*freq)`)を12bitへスケーリング |

**HWエンベロープ（`EGT`bit3=1時）**: レジスタ`0x0B`(Fine)+`0x0C`(Coarse)は分割不可の単一16bit「Envelope Period」値なので、`ext.HWEP`(16bit)をそのまま書く。`0x08+ch`のbit4を1に固定してHW EGモードを明示し、`hwOp[0].EGT`の下位4bitを波形シェイプ(CONT/ATT/ALT/HOLD)として`0x0D`に書く。

```cpp
setReg(0x0B, ext.HWEP & 0xFF);        // Fine
setReg(0x0C, (ext.HWEP >> 8) & 0xFF); // Coarse
setReg(0x0D, hwOp[0].EGT & 0xF);      // Shape (CONT/ATT/ALT/HOLDのビット組み合わせ)
```

### DCSG (SN76489) — `CDCSG`

DCSGはアドレス指定レジスタを持たず、`writeRaw`によるコマンドバイト
列で制御する(通常のsetReg方式ではない)。

| フィールド | 実機コマンド | 意味 |
|---|---|---|
| `hw.ALG` | `==1`でノイズ(ch3固定) | `queryCh`がch3固定を強制 |
| `hw.FB` (bit0) | `0xE0\|((FB&1)<<2)\|(NFQ&3)` のbit2 | ノイズタイプ(周期性/白色雑音) |
| `hw.NFQ` (下位2bit) | 同上のbit0-1 | ノイズ周波数選択(3種+トーン2連動) |
| 音量(計算値) | `0x90\|(ch*32)\|att` (ch0-2) / `0xF0\|att` (ch3=ノイズ) | **減衰量**を書く。0=最大音量/15=無音(他のPSG系と極性が逆)。**2dB/step**で0-14が0〜-28dB、15のみ完全消音 |
| Fnum(TonePeriod) | `0x80\|(ch*32)\|(period&0xF)` + 上位6bit別コマンド | ch0-2のみ(ch3=ノイズは周波数レジスタなし)。周期は10bitへクランプ |

### SCC/SCC+ (K051649/K052539) — `CSCC`

レジスタ配置は無印SCC(K051649)とSCC+(K052539)で異なる。無印はch3/ch4が波形メモリを共有して4ブロック(0x00-0x7F)しか持たないため、以降の制御レジスタが前へずれる。

| フィールド | 無印SCC (K051649) | SCC+ (K052539) | 意味 |
|---|---|---|---|
| `hwOp[0].WS` (7bit) | 0x00-0x7F | 0x00-0x9F | 波形メモリ(32バイト/ch)。波形番号を`SccWaveRegistry`経由で引いて書き込む |
| Fnum(TonePeriod) | 0x80+ch*2 | 0xA0+ch*2 | 下位8bit / 上位4bit の2バイト。周期テーブル(`master/(32*freq)`)を12bitへスケーリング |
| 音量(計算値) | 0x8A+ch | 0xAA+ch | 0=無音/15=最大。**リニア乗算**(`out = wave × vol/15`)であり対数DACではない |
| チャンネル有効 | 0x8F | 0xAF | bit0-4=ch0-4 |
| deform | 0xC0 | 0xC0 | 8bit/4bit周波数モード・波形ローテーション制御。`init()`が0クリアする |

キーオンでチャンネル有効ビットを立てるが、キーオフでは落とさない。消音はソフトウェアエンベロープが音量を0まで下げることで行う(キーオフで有効ビットを落とすとリリースが鳴らないため)。

**ch3/ch4の波形メモリ共有**: 無印SCC(K051649)と、同じ配置を持つY8960のSCC部(`DEVICE_SCCEX`)は、ch3とch4が物理的に波形メモリを共有する。`queryCh`が「ch3が既に確保している波形と完全一致する場合のみch4を割り当てる」制御を行う(`CSCC::sharesCh34Wave()`)。SCC+(K052539、`DEVICE_SCCP`)はこの制約が無い。

`CSCC::setWaveBankNo()`はどこからも呼ばれておらず、波形バンク番号は常に0(STATUS 4節)。

### AY8930 — `CEPSG`

SSGと同じ`hw.ALG`/`hw.NFQ`の意味に加え、Expand Mode(Bank A/B切替、
レジスタ0xdのbit4-5)を常時有効化して拡張機能を使う。ch0はBank A側
(reg 0xb/0xc)、ch1/ch2はBank B側(reg 0x0-0x3)という非対称配置。

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x07 (Bank A、ミックス。SSGと同じ意味論) | 0=トーン/1=ノイズ/2=両方/3=両方無効 |
| `hw.NFQ`(5bit)+`hw.FB`(bit0-2、ノイズ用に転用) | 0x06 (Bank A、8bit拡張ノイズ周波数) | `(NFQ&0x1F)\|((FB&7)<<5)` |
| `hwOp[0].WS` (4bit) | 0x6+ch (Bank B) | **デューティ比**（矩形波のパルス幅制御、AY-3-8910にはない拡張機能。SCCの波形番号とは無関係、チップ依存の意味の転用パターン） |
| `hwOp[0].EGT`(bit3)/`SL`/`RR`/`DR`/`SR` | ch0: 0xb/0xc(Bank A) / ch1-2: 0x0-0x3(Bank B) | HW/ソフトウェアエンベロープ切替。ch0とch1/2でレジスタ配置が非対称 |
| `hwOp[0].EGT`(下位4bit、HW EG時) | 0xd下位4bit(ch0) / 0x3+ch(ch1-2、Bank B) | エンベロープ波形シェイプ |
| 音量(計算値) | 0x08-0x0A (ch0-2、5bit) | 0=無音/31=最大。AY-3-8910より1bit広い |
| Fnum | 0x00-0x05 (ch0-2、各2バイト、16bit) | Expand Modeは1/8分周のため、SSG周期テーブルの2倍(`chipShift=-1`) |

レジスタダンプは、内部シャドウの表現(「PSG系」冒頭のバンク切り替えの項)をそのまま表示するので、**`0x00-0x0F`がBank A、`0x10-0x1F`がBank B**として読める。

### SAA1099 — `CSAA1099`

実機データシートに基づく実装。

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x14(トーン有効bit[ch])/0x15(ノイズ有効bit[ch]) | 0=トーンのみ/1=ノイズのみ/2=両方（SSGと同じALG意味論） |
| `hw.NFQ` (下位2bit) | 0x16 (ch0-2→bit0-1、ch3-5→bit4-5の2系統) | ノイズパラメータ（2系統: ch0-2/ch3-5共有） |
| `hwOp[0].EGT` (bit3) | — (フラグのみ) | 1=HWエンベロープ使用（ch0-2/ch3-5の3ch単位で共有） |
| 音量(計算値、左右独立) | 0x00-0x05 (ch0-5、下位4bit=左/上位4bit=右) | パンポットから等パワーパンニング(cos/sin)で左右音量を算出 |
| Fnum | 0x08-0x0D(周波数8bit、ch0-5) + 0x10-0x12(オクターブ3bit×2ch/レジスタ) | 実機固有の周波数式(`freqReg = 511 - base*2^oct/hz`、`base = clock/512`。8MHzで15625)を直接計算。エミュレータのSAASoundも同じ式を出力の周波数とし、その2倍の速さで出力を反転する。2倍の`2*clock/512`は反転の速さで、周波数の式に使うと1オクターブ低く鳴る。他PSG系のFnumTable機構(`FnumTableType`)は使わずNone指定。Hzの求め方(ノート・kfs単位のファインチューン・チャンネルLFOを足し、マスターピッチを基準にする)と`ChState::Fnum`への詰め方(`block=oct` / `fnum=freqReg`)は`CDSG::getFnumber()`と同じ |
| 全チャンネル有効化 | 0x1C (bit0) | 初期化時に1回設定 |

**HWエンベロープ（`EGT`bit3=1時）**: `ext.HWEP`の下位6bitを流用する。

| bit | 実機レジスタ | 意味 |
|---|---|---|
| bit0-2 | 0x18(ch0-2)/0x19(ch3-5)のbit1-3 | mode (波形0-7) |
| bit3 | 同上のbit4 | resolution |
| bit4 | 同上のbit5 | clockSrc |
| bit5 | 同上のbit0 | rightInvert |

HWエンベロープはch0-2/ch3-5の3ch単位で共有されるハードウェア制約があるため、「音色データがデバイスを選択する」原則に従い、`queryCh`が該当グループの空きchを返すのみでモデル化する(専用フィールド不要)。既に同一エンベロープ設定(`ext.HWEP`下位6bit)で発音中のchがあるグループを優先することで、レジスタ競合を避ける。パンポットは全chが等パワーパンニング（`ChState.panpot`から自動計算、パッチ側での指定は不要）。

## SSGS系 (YMZ705 / YMZ732 / YMZ771)

```
CSSGS : CSSG                           (SSG互換部, 3ch×2ユニット = 6ch)
  └── CSSGS3 : CSSGS                   (YMZ771のSSG互換部。レジスタ配置とパンポット分解能のみ差し替え)
CSSGSAdPcm : CAdPcmBase                (ADPCM部, 8ch。SSGS/SSGS2のみ)
```

YM2149相当のSSGを2系統と、サンプル再生部(SSGS/SSGS2はADPCM、SSGS3はAMM)を
1チップに収めた音源。SSGS/SSGS2では両ブロックが同一の8bitレジスタ空間
($00-$3F がSSG、$40-$B3 がADPCM)を共有するため、サブデバイスの展開
(`DEVICE_SSGS`/`DEVICE_SSGS2` → SSG部 + `DEVICE_SSGS_ADPCM`)をしても
`extraPort`やアドレスオフセットは要らない。

| ch | ユニット | 実機レジスタ範囲(SSGS/SSGS2) |
|---|---|---|
| 0-2 | SSG-1 | `$00`-`$0D`（Fnum/ノイズ/ミックス/音量/HW EG）、`$10`-`$12`（パンポット） |
| 3-5 | SSG-2 | `$20`-`$2D`、`$30`-`$32`（同上） |

- **SSG互換部の音色データは単体のSSGと完全に共有する**(`VOICE_PATCH_SSG`)。
  レジスタ互換であり、追加されたパンポットは音色データではなくMIDIの
  CC#10で制御されるため、音色形式に差が出ない。3品種が混在する構成でも
  同一`VoicePatchType`なので`CSpanDevice`の束ね対象になる(`deviceType`はサブチップごとに独立して保持される)。
- ノイズ発生器・HWエンベロープ・ミックスレジスタはユニット内3chで共有される
  (単体のYM2149と同じ制約が2系統ぶん独立して存在する)。ノイズ要求時の
  `queryCh`はch2→ch5の順に空きを探す。
- **パンポット**: SSGS/SSGS2が4bit（`0`=左端 / `8`=中央 / `15`=右端）、SSGS3のみ
  5bit（`0` / `16` / `31`）。`ChState::panpot`(-64〜+63)を四捨五入で写す。
  値とL/Rレベルの対応はデータシートに記載が無く、いずれもYMZ280B準拠の
  分配則で解釈する。
- **パンポットのリセット値0は「中央」ではなく左端**。`CSoundDevice::noteOn()`は`panpot`が既定値0のままだと`panDirty`が立たず
  `updatePanpot()`を一度も呼ばないため、`init()`で全chに中央値を書いておかないと
  パンCCを送らない限り全チャンネルが左に張り付く。
- **クロック**: SSGブロックの動作クロックはどのチップでも2.048MHzで、周期テーブルの
  基準もこの値。マスタークロックからの分周比だけがチップで異なる
  (SSGS: 4.096MHz÷2 または 6.144MHz÷3 [S6Mピンで選択] / SSGS2: 12.288MHz÷6 /
  SSGS3: 16.384MHz÷8)。分周比がクロック値そのものに依存するので
  `SubDeviceSpec::clockDivider`(固定値)では表現できず、`DeviceFactory`の
  `ssgsSsgBlockClock()`で`deviceType`とクロック値から判定する。この判定規則は
  エミュレーションエンジン(EPSGemuEngineの`ssgsInternalClock`)と一致させること。

**YMZ732(SSGS2)はYMZ705(SSGS)の上位互換**で、SSG部・ADPCM部ともレジスタ
マップは完全に同一(YMZ732データシートにも「YMZ705(SSGS)とレジスタコンパチ
ブルです」と明記)。ドライバは`CSSGS`/`CSSGSAdPcm`をそのまま共用し、
`deviceType`を分けているのはクロックの分周比の違いだけのため。
SSGS2のシンプルアクセスモード(`/SEL`ピンをLにすると、データバス
8本のみ($40-$FFの192コード)で外部ROM上のシーケンスを起動できるハードウェア
モード)は、FITOMがCPUインターフェイスからレジスタを直接叩く構成で使わないため、ドライバ側の対応は不要(外部ROMのシンプルアクセスコード
テーブル領域`$000240`-`$00047F`も参照しない)。

**ADPCM部の`deviceType`はSSGSとSSGS2で共有する**(`DEVICE_SSGS_ADPCM`)。制御が
完全に同一のため分ける理由が無く、共有することで混在時も自動的に同一
グループとして束ねられ、PCMバンク/メモリイメージ(カタログ種別`SSGS_ADPCM`)も
1つで足りる。ADPCM部の詳細は「ADPCM系」節。

**YMZ705データシートの誤記2箇所**: SSGレジスタマップの$28-$2A/$30-$32は
「Pan-pot / Frequency」と書かれているが、ビットフィールド(M/L3-L0 と
P3-P0)と「$20-$2D はYM2149互換」という同ページの注記から、正しくは
$28-$2Aが音量・$30-$32がパンポット。ADPCMレジスタマップの$42のみ
P2-P0がD3-D1にずれて描かれているが、他7chと同じくP3-P0がD3-D0が正しい。
いずれもYMZ732データシートの対応表では正しく記載されており、そちらが
正しいことの裏付けになっている。

### SSGS3 (YMZ771) の差分

SSG音源としての機能はSSGS/SSGS2と全く同じで、`CSSGS`を継承する`CSSGS3`が
違いを3点だけ差し替える。音色データも同じ`VOICE_PATCH_SSG`。

1. **レジスタが「ユニットごと」ではなく「機能ごと」にまとめ直され、`$10`-`$32`の
   連続した空間に並ぶ**。ビットフィールドの意味はYM2149と同一のため、`CSSG`の
   仮想索引メソッド(「PSG系」節)を差し替えるだけで対応できる。

   | アドレス | 内容 |
   |---|---|
   | `$10`-`$1B` | トーン周期 TP1A/1B/1C/2A/2B/2C（各2バイト、Fine→Coarse） |
   | `$1C`/`$1D` | ノイズ周期 NP1 / NP2 |
   | `$1E`/`$1F` | ミックス設定（`N*C N*B N*A T*C T*B T*A`、YM2149の`$07`と同配置） |
   | `$20`-`$25` | 音量 M/L（1A,1B,1C,2A,2B,2C） |
   | `$26`-`$29` | エンベロープ周期 EP1（2バイト）/ EP2（2バイト） |
   | `$2A`/`$2B` | エンベロープ形状（CONT/ATT/ALT/HOLD） |
   | `$2C`-`$31` | パンポット（1A,1B,1C,2A,2B,2C、5bit） |
   | `$32` | SSGトータルボリューム |

2. **パンポットが5bit**(0-31、16=中央)。`CSSGS::panMax()`をオーバーライドして表現する。
3. **SSGトータルボリューム(`$32`)が追加**。128で100%の線形ボリュームで、
   **リセット値0は完全無音**。パンポットのリセット値(左端)と同じく、`init()`で
   明示的に書かないと一切音が出ない。FITOMは音量をチャンネルごとの音量
   レジスタで作るため、常に100%固定で書く。

クロックは16.384MHzを1/8して2.048MHz。データシートにSSGブロックの動作クロックの
記載は無いが、この分周比がファミリ共通の2.048MHzと一致する。

**AMM部は非対応**。ADPCMの代わりに搭載されたMPEG Audio系コーデックの
フレーズ再生部で、生成すべきサブデバイスがSSG部しか無いため、SSGS/SSGS2と
違いサブデバイスの展開もしない(単一デバイスのまま)。

## DSG (YM2163)

```
CDSG : CSoundDevice                    (YM2163 楽音部, 4ch)
CDSGRhythm : CSoundDevice              (YM2163 内蔵リズム, 5パート)
```

波形メモリ方式のNMOS音源。単一のバスアドレスに対し「D7=1でアドレスラッチ /
D7=0でデータ」の2回書き込みで駆動する実チップだが、この2回書き込みはHW I/F
プラグイン側が吸収するため、ドライバは通常どおり`setReg(0x80〜0x9F, data)`で
書く。ポートは1本のみ(`SplitPort`/`extraPort`は使わない)。

**ユーザー音色は存在しない**ため、音色パラメータを持つHwBank(JSONプリセット)を作ることはできない。
音色は「エンベロープ4種 × 波形5種」の20通りが、Program Changeの値から
機械的に決まる暗黙のバンクとして`PatchManager`が生成する
(`initDsgBuiltinPatches()`)。

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | reg 0x88+ch bit6-5 (E2E1) | エンベロープ選択(0-3) |
| `hwOp[0].WS` (下位3bit) | reg 0x88+ch bit2-0 (W3W2W1) | 波形メモリ選択(1-5。0/6/7は未定義=無音) |
| `hwOp[0].TL` | — (計算値へ合成) | 音色レベル。vol/exp/velと合わせて2bitの音量へ丸められる |
| サスティン(CC#64) | reg 0x88+ch bit4 (SUS) | ハードウェアのサスティンON/OFFに直結 |
| 音量(計算値) | reg 0x8C+ch bit5-4 (VL2VL1) | 0=0dB / 1=-6dB / 2=-12dB / 3=-∞。**4段階のみ** |
| 出力端子 | reg 0x8C+ch bit3-0 (F1-F4) | OR1-OR4への出力可否。定位ではないため常にOR1固定 |
| Fnum(専用計算) | reg 0x80+ch / reg 0x84+ch bit4-0 | `f = clock/(DV * 2^(3-oct))`。DV=10bit、oct=B2*2+B1 |
| キーオン | reg 0x84+ch bit6 (KON) | エッジ検出。同レジスタの分周数上位を壊さないこと |
| 強制減衰 | reg 0x84+ch bit5 (FD) | CC#120(All Sound Off)で使う。同じくエッジ検出 |

- **`CPSGBase`は継承しない**。`CPSGBase`は「チップ内蔵EGが貧弱なので
  ソフトウェアADSRで音量を時間変化させる」設計だが、YM2163は4種類の内蔵
  エンベロープを持ち、音色パラメータ(AR/DR/SL/SR/RR)自体を受け付けない。
  ソフトウェアEGを走らせる余地が無いため、`CSoundDevice`を直接継承し、
  振幅LFO用の`lfoTL_`だけを自前で持つ。
- **周波数は共有の周期テーブルを使わず、Hzから直接分解する**。
  発音周波数は `f = clock / (DV * 2^(3-oct))`、`oct = B2*2+B1` (0-3)、
  `DV`はDV7〜DV¼を並べた10bit整数(LSB=DV¼、データシートの分周数ΣDViの4倍値)。
  共有の`FnumTableType::TonePeriod`テーブルはSN76489/SCCの
  `f = master/(32*N)` 用に量子化されており(基準オクターブでのテーブル値が
  119〜239程度しか無い)、総分周比が32倍細かいYM2163に流用すると基準
  オクターブでの丸めがそのまま乗算され、最悪14セント近い音痴になる。
  `CSAA1099`が実機式を直接計算しているのと同じ理由で、`CDSG::getFnumber()`が
  ノート/ファインチューン/チャンネルLFOからHzを求め、`ChState::Fnum`へ
  `block=oct` / `fnum=DV` として詰める(GUIのFnumber表示も実機レジスタと
  同じ値になる)。
  `oct`が大きいほど`2^(3-oct)`が小さくなり`DV`の有効桁が増えるため、
  **`DV`が10bitに収まる範囲で最大の`oct`を選ぶ**こと。
  最低音は`DV=1023`/`oct=0`の約122.2Hz(MIDIノート47 B2のすぐ下)で、
  それより下はクランプする。
- **音量は2bit (VL2VL1) しか無い**。0dB / -6dB / -12dB / -∞ の4段階のみで、
  `linear2dB()`は0.75dBの2のべき乗倍しか刻めず6dBを表現できない(2bit幅だと
  12dB/stepになる)ため、`CDCSG`が2dB/stepのために`kGM2dB`から直接換算して
  いるのと同じ方式をとる。CC#7/CC#11/ベロシティの分解能はこの4段階に
  丸められる — これはチップの構造的な制約。
- **サスティンペダルはハードウェアのSUSビットに直結する**(reg 0x88+ch bit4)。
  ROM固定音色でEGパラメータを持たないため、OPLL系と同じくSUSビットだけで
  制御する。SUS=1でキーオフ後のリリースが1.2秒へ延び、エンベロープ0では
  キーオフ自体が効かなくなる(データシート図2)。
- **CC#120(All Sound Off)はFD(フォーシングダンプ)ビットを使う**。
  SUSを落としてからFDを立て、通常のリリースより速く消音する。KON/FDとも
  実機・エミュレータでエッジ検出のため、後続の`noteOff()`が両方を0へ戻す
  ことで次回の立ち上がりが再武装される。同じ理由で、発音中にreg 0x84+chの
  分周数上位/オクターブを書き替えてもリトリガーはされない(ピッチベンドに
  必要)。

### ビルトイン音色の番号割り当て

Program Change値 0-19 が、以下のように波形メジャー(同じ波形の4エンベロープが
連続する)で並ぶ。音色名も`<波形名>.<エンベロープ名>`で機械生成される。

```
prog = 波形番号(0-4) * 4 + エンベロープ番号(0-3)
  波形番号     0=St / 1=Or / 2=Cl / 3=Pf / 4=Hc   (レジスタ値 W = 波形番号+1)
  エンベロープ 0=Percussive / 1=Wind / 2=Sustain / 3=Plateau
```

一覧の表は`docs/manuals/hwpatch-reference.md`の「DSG」節。

ビルトイン音色は内部生成のため`swBank`/`swProg`を持てない。パフォーマンスパッチは、OPLL系ROM音色と同じ
`role=="builtin_swpatch_meta"`のメタバンクで、
`builtin: { "patch_type": "DSG", "patch_no": <prog 0-19> }`として紐づける
(`PatchManager::resolveDsgBuiltinVoice()`が`findByBuiltinRef()`で探索する。`docs/design/patch-resolution.md`の「ROM/ビルトイン音色へのパフォーマンスパッチ紐づけ」)。
`patch_no`は**0から**有効(prog 0 = `St.Percussive`が正規の音色)。
`BuiltinRef::isValid()`が`patchNo >= 0`である理由がこれで、`>= 1`にすると
prog 0 のエントリだけが黙って一致しなくなる。

### エンベロープとサスティン(SUS)の関係

SUSは音色パラメータではなくサスティンペダル(CC#64)で動く。
時定数はマスタークロックに比例する(下表は1MHz時)。

| E2E1 | SUS=0 | SUS=1 |
|---|---|---|
| 0 `Percussive` | 即最大 → 60msで1/2 → 1.2sで減衰。KOFFで60msリリース | 同左。**KOFFが効かない** |
| 1 `Wind` | 60msアタック → 保持。KOFFで120msリリース | 60msアタック → 保持。KOFFで1.2sリリース |
| 2 `Sustain` | 即最大 → 60msで1/2 → 保持。KOFFで60msリリース | 同左。KOFFで1.2sリリース |
| 3 `Plateau` | 即最大 → 保持。KOFFで即無音(ゲート) | 即最大 → 保持。KOFFで1.2sリリース |

### 内蔵リズム (`CDSGRhythm`)

楽音部とレジスタ空間が完全に独立している(0x90-0x97 と 0x80-0x8F)ため、楽音4chを潰さずに共存する。OPL/OPLL系のように`rhythm_mode`で楽音側のchを潰す必要が無いので、サブデバイスとして常に生成する。

| パート | 音 | トリガー(reg 0x90) | レベル | 出力端子 |
|---|---|---|---|---|
| 0 | BD (バスドラム) | bit0 | reg 0x95 | RH1 |
| 1 | HC (ハイコンガ) | bit1 | reg 0x96 | RH1 |
| 2 | SDN (スネアノイズ) | bit2 | reg 0x97 | RH2 |
| 3 | HHO (ハイハット Open) | bit3 | reg 0x94 | RH2 |
| 4 | HHD (ハイハット Close) | bit4 | reg 0x94 | RH2 |

- パート番号はトリガービットの並びに一致し、音色データの`hw.ALG`(下位3bit)で直接指定する(`COPLLRhythm`と同じ規約)。
- HHOとHHDは実機のリズム発振器を共有するため、レベルレジスタも共有する(OPLLリズムのHH/SDがch7を共有するのと同じ構図)。
- トリガービットは書くと発音して自動的に0へ戻る。他パートのビットを
  ORしてはならず、同値連打がシャドウキャッシュに抑止されないよう`forceWrite`で書く。
- **レベルはトリガーの後に書く**こと。トリガーが内蔵リズムEGのレベルを
  最大へリセットするため、先に書くと上書きされてベロシティが効かなくなる。
  `CSoundDevice::noteOn()`は`updateVolExp()`→`updateKey()`の順で呼ぶので、
  `CDSGRhythm::updateKey()`がトリガー送出の直後に自分でレベルを書き直している。
- レベル(LV4-LV0)は**線形減衰**で0が最大音量・31が最小音量(31でも無音には
  ならない)。LH(bit0)=0のままにして内蔵リズムEGの減衰を働かせる
  (LH=1は減衰せず固定レベルで鳴り続けるモード)。
- 音量はMIDI Volume(CC#7)とベロシティの組み合わせ(Expressionは含まない、
  `COPLLRhythm`と同じ)。音程レジスタは存在しないため`updateFreq`はno-op。

### GUIへの列挙

内蔵リズムもDSGのビルトイン音色も、通常の`HwBankRegistry`を一切経由しないため、パッチピッカーへは
自動的には現れない。対応チップ・パート名は
`gui/bridge/FITOMBridge.cpp`の`kBuiltinRhythmChips[]`/
`getBuiltinRhythmNames()`、カテゴリ(CC#0)は
`apps/fitom_gui/PatchPickerDialog.cpp`の`kCategories[]`が情報源で、
いずれもハードコードされた表。新しい内蔵リズム搭載チップを足したら
ここも更新すること(コア側の`resolveBuiltinRhythm()`だけ直しても
GUIからは選べないままになる)。

## Y8960 の拡張ブロック

Y8960(MSX用サウンドカートリッジ)は、既存チップの互換ブロックにチャンネル
ごとの拡張レジスタを足した構成をとる。仕様の正本は Y8960_Cartridge
リポジトリの `doc/spec/Y8960_Specifications.xlsx`。4ブロックとも
**チャンネルごとの8bit定位**(1=左端 / 128=中央 / 255=右端、リセット値128)を
持ち、MIDIのパンポット(-64..+63)からの変換は共通の`fitom::y8960PanValue()`
(`VolumeUtils.h`)が行う。値からL/Rレベルへの定パワー則は実機がハードウェアで
実装している(仕様書には記載が無く、利用者からの情報)ため、FITOM側は線形に
写すだけでよい。

| deviceType | ドライバ | 互換元 | 追加レジスタ |
|---|---|---|---|
| `DEVICE_OPLLEX` | `COPLLEX : COPLL` | YM2413 | BANK `0x40`-`0x48`(ROMプリセットバンク)、定位 `0x50`-`0x58` |
| `DEVICE_OPLLEX_RHY` | `COPLLEXRhythm : COPLLRhythm` | YM2413 内蔵リズム | 使う物理chの定位 `0x56`-`0x58` |
| `DEVICE_OPL2EX` | `COPL2EX : COPL2` | YM3812 | 定位 `0xF6`-`0xFE` |
| `DEVICE_OPL2EX_RHY` | `COPL2EXRhythm : COPLRhythm` | YM3812 内蔵リズム | 使う物理chの定位 `0xFC`-`0xFE` |
| `DEVICE_ADPCMB_OPL2EX` | `CY8960Delta : CYmDelta` | Y8950 ADPCM-B | 定位 `0xFF` |
| `DEVICE_SSGEX` | `CSSGEX : CSSGS` | YM2149 × 2 | SSG-2 が `0x10` から。chごとの音量 `0x20`-`0x22`/`0x24`-`0x26`、定位 `0x28`-`0x2A`/`0x2C`-`0x2E` |
| `DEVICE_SCCEX` | `CSCC` | K051649 (無印SCC) | 定位 `0x100`-`0x104`(実機の `0x9900`-`0x9904`) |

- **内蔵リズムを標準のOPLL/OPL2と別の`deviceType`にしている理由**: レジスタ体系
  (`0x0E`/`0x36`-`0x38`、`0xBD`)は標準と同一だが、定位レジスタを書くかどうかを
  `deviceType`側の索引(`FITOMConfig::getChipPanType()`)で引けるようにするため。
  OPL2EXのリズムは音色データ形式が同一なので`VOICE_PATCH_OPL_RHY`を共有し、
  OPLLEXのリズムは`VOICE_PATCH_BUILTIN_RHYTHM`(CC#0=0x70)の「OPLL」選択で、
  標準OPLLのリズムが無い構成ならこちらで鳴る(リズム音色は全バンク共通で標準OPLLと同じ)。
- **共有chの定位はSD/TOMが持つ**: ch7(HH/SD)・ch8(CYM/TOM)は2パートで1つの
  定位レジスタを共有する。F-numberと同じ規則で、SD/TOMだけが書き、HH/CYMは
  書かない(共有chの定位はSD/TOMの定位で決まる)。BDはch6を単独で使う。
- **`init()`で拡張レジスタをチップとシャドウの両方へ揃える**: OPLLEXのBANKの
  リセット値は仕様に記載が無く、シャドウの初期値(0)と同じ値の書き込みは
  `setReg()`が省くため。定位も仕様上のリセット値128をシャドウへ載せておく。
- **SSGEX**: YM2149相当を2系統内蔵した6chで、YMZ705(SSGS)と同じ「ユニットごと」の配置だが、ユニットの間隔(SSG-2が`$10`から)と定位が違う。プロファイルに書くクロックはSSGブロックの動作クロックそのもの
  (単体のSSGと同じ意味)で、SSGS系のような分周比の換算はしない。FITOMは音量を
  YM2149互換の4bit音量レジスタ(`$08`-`$0A` / `$18`-`$1A`)で作るため、chごとの音量(0-128)は常に128で固定する。
- **SCCEX**は無印SCCと同じ配置のため、ch3/ch4の波形メモリ共有の制約もそのまま
  引き継ぐ(`CSCC::sharesCh34Wave()`)。
- 定位を持つため、OPL2EX/OPLLEXの各サブデバイスは`CLinearPanDevice`の束ね
  (`stereo_pair`)の対象外。
- Y8960のOPM(YM2151)は拡張レジスタを持たない標準のYM2151で、クロックはMSXスロットの
  システムクロック3.579545MHz(仕様書には記載が無く、利用者からの情報)。`chip:"OPM"`として
  既存の`COPM`をそのまま使う(`COPM`は3.579545MHz基準のKC表で音程を出す)。
- Y8960の仕様は変更が続いている。仕様書が変わったらこの節を見直すこと。

## ADPCM系

```
CAdPcmBase : CSoundDevice               (PCMバンク管理・loadVoice純粋仮想の共通基底)
  ├── CYmDelta : CAdPcmBase             (Delta-T方式、YM2608/YM3801/YM2610B ADPCM-B)
  │     └── CY8960Delta : CYmDelta      (Y8960の拡張OPL2部の内蔵ADPCM-B、定位つき)
  ├── CAdPcm2610A : CAdPcmBase          (YM2610 ADPCM-A、多チャンネルPCM、Delta-Tと無関係)
  ├── CAdPcmZ280 : CAdPcmBase           (YMZ280B/PCMD8, 8ch)
  └── CSSGSAdPcm : CAdPcmBase           (YMZ705/YMZ732, 8ch)
```

ADPCM-Bの`deviceType`はチップごとに分かれ(レジスタマップが違うため)、`VoicePatchType`は共有する(音色データ形式が同じため)。

```
DEVICE_ADPCMB_Y8950   (59) → Y8950   (kY8950_DeltaT)
DEVICE_ADPCMB_OPNA    (60) → OPNA    (kOPNA_DeltaT)
DEVICE_ADPCMB_OPL2EX  (66) → Y8960   (Y8950と同じ配置 + 定位)
DEVICE_ADPCMB        (117) → OPNB/OPNBB (kOPNB_DeltaT)
```

`DEVICE_ADPCM`(119)は旧い汎用名で、`DEVICE_ADPCMB_Y8950`を使う。

- **音色データ**: すべてSampleZonePatchを使い、HwPatchの`hwOp[]`は使わない。再生する波形は、ノートとベロシティで選んだゾーンの`SampleZone::waveIndex`(PCMバンクのエントリ番号)で決まる(`SampleZonePatch::resolveZone()`)
- **音量レジスタの向き**: ADPCM-Bのボリュームと、YMZ280BのTotal Level(reg `$02+ch*4`)はゲインで、値が大きいほど大きい音。`VoiceProcessor::effectiveTL()`はラウドネス空間(0=無音、127=最大)の値なので、反転せずに書く。YMZ280Bのデータシート(英語版・日本語版とも)は「256ステップ」とだけ書いて向きを書いていないので、エミュレータ(MAME由来。出力を`sample*level/256`で求める)に合わせている。ラウドネスはGM2の`40·log10(L/127)`dBの目盛りで、線形のレジスタへそのまま写すとdB上の効きは半分になる。ADPCM-Bも同じ写し方で、両者を揃えている
- **`CYmDelta`はチップごとに異なる`RegMap`（レジスタアドレス集合）を持つ**：
  `kY8950_DeltaT`(YM3801)/`kOPNA_DeltaT`(YM2608)/`kOPNB_DeltaT`(YM2610/YM2610B)の
  3種類(`control1`〜`panmask`の16フィールド)。
  `RegMap`には`addrShift`フィールド（Start/Endアドレスレジスタの境界単位、
  バイトオフセットを右シフトする量）もあり、OPNA/Y8950は2（4byte境界）、
  OPNB/OPNBB(YM2610/2610B)は8（256byte境界、チップ回路仕様上の固定値で
  control2の設定値には依存しない）。`AdpcmVoice::startAddr`/`length`はバイト単位で保持し、`registerVoice()`/
  `updateVoice()`が`addrShift`で変換する。
- **DeltaN(Delta-N、ADPCM-Bの再生ピッチレジスタ)の基準周波数は固定16000Hz**：
  `FnumUtils.h`の`FnumTableType::DeltaN`は他の型(Fnumber/TonePeriod)と違い
  `masterPitch_`(A440チューニング、既定440Hz)ではなく、固定16000Hz基準を使う。`CYmDelta`の
  `kNoteOffset`/`kPitchOrigin`定数がこの16000Hz基準にキャリブレーションされて
  いるため。算出式は`delta_n=round(2^16*freq*divide/master)`
  (`divide`=OPNA/OPNB用マスタークロック分周比144)で、`divide`の乗算が
  欠落しているとC4のDeltaNが本来の約1/36という遅すぎる値になる。
- **ADPCM-Bのcontrol2 bit7/6(pan_left/pan_right)は出力有効化ビットを兼ねる**：
  両ビット0(センターパンのMIDI既定状態)だと実チップは演算結果を一切出力に
  加算しない。`CSoundDevice::noteOn()`の`updatePanpot()`呼び出しはpanDirty
  (前回値との差分)条件のため、MIDI側が明示的にパンCCを送らないと
  「0→0で変化なし」と判定され一度も呼ばれない。そのため`CYmDelta`の
  NoteOn処理は上位のdirtyフラグに関係なく`updatePanpot(ch)`を無条件に呼ぶ
  （このチップ固有の対応、他デバイスのパンレジスタは追従不要）。
- **`createCAdPcm()`のmasterクロックは`port->getClock()`を使う**：DeltaN算出のmasterにはサンプルレート
  ではなく実クロックを渡す(取得不可時のみサンプルレートにフォールバック)。
- **`updateKey`の役割分担**：Start/End(`updateVoice`)・
  DeltaN(`updateFreq`)・Volume(`updateVolExp`)は個別のフックで設定し、
  `updateKey`は`stopPcm()`（4段階の停止シーケンス）+固定値`0xa0`による
  純粋な再生トリガーのみ。
- **PCMバンク(オフセットテーブル)は物理チップ単位に分けて登録する**：OPNA用とOPNB/OPNBB用のADPCM-Bは
  音色データ形式が共通で同一VoicePatchType(`VOICE_PATCH_ADPCMB`)を共有するが、
  実際に配置される波形バイナリのバウンダリ整列(OPNA=32byte、OPNB/OPNBB=
  256byte)が異なるため、同じエントリ(オフセット/
  サイズ)テーブルを共有できない。`PcmBank::deviceType`と`PcmBankRegistry::findBankNoForDeviceType()`
  (deviceType完全一致、無ければVoicePatchType一致へフォールバック)で
  チップごとのバンクを引き、`CFITOM::initDevices()`はこちらを先に試す。プロファイルでは
  `pcm_banks[].chip`("OPNA"/"OPNB"/"OPNBB"/"Y8950")で指定する
  (`Config.cpp`の`resolvePcmBankChipDeviceType()`が変換)。entries[]の
  エントリ番号(waveIndex)とサンプル名の対応は物理チップに依らず共通の
  ため、音色パッチ定義自体はOPNA/OPNBどちらのデバイスにも共通で使い回せる。束ねた場合のバンク解決は`docs/design/chip-drivers.md`の「同種デバイスの束ね」
- **SSGS-ADPCM(`CSSGSAdPcm`)は他のADPCM系と2点で決定的に異なる**:
  - 波形の開始/終了アドレスレジスタを持たない。チップが外部メモリ先頭の
    ボイステーブル(データシート「8M byte Play data ROM address map」の
    $000000-$00017F、ボイス0-63のStart/Endアドレス)を自力で引くため、ドライバが書くのは6bitのボイス番号
    (`SampleZone::waveIndex`、0-63)だけでよい。よって`registerVoice()`は
    チップへ何も反映しない。PCMメモリイメージ側にこのボイステーブルが
    含まれている必要がある(`pcm_image_catalog`の種別`SSGS_ADPCM`)。64以上の`wave_index`は発音時に警告して無視する。
  - 再生ピッチを変えられない。持っているのはサンプリング周波数の4択
    (32k/16k/8k/4k、`pcmbank.json`の`sample_rate`から決まる)だけで、
    ノート番号からは算出しない(`updateFreq()`はno-op)。そのバンクを使う全chに一律で適用される。
  - 音量は4bit・パンポットは4bit(`0`=左端 / `8`=中央 / `15`=右端、SSG部と同じ分配則)。

## OPL4 AWM

```
COPL4AWM : CSoundDevice                (YMF278のAWM/PCM部, 24ch)
```

OPL4を指定すると、サブデバイスの自動生成で`COPL3`(4OP)+`COPL3_2`(2OP)+`COPL4AWM`の3つに展開される。AWM部はFM部と別の3つ目のレジスタバンク(0x200以降)にあり、オフセット0x200のポートで書く(`docs/design/chip-drivers.md`の「高位バンクのポート」)。音色データはSampleZonePatchで、AWMはソフトウェアLFO等のSwPatchを音に反映しない(STATUS 4節)。

- **波形番号の書き込み順**: `COPL4AWM::updateVoice()`は、波形番号の上位1bit(reg 0x20+ch bit0)を下位8bit(reg 0x08+ch)より**先に**書く。reg 0x08-0x1Fへの書き込みで波形ヘッダの読み込みが始まり、その時点のreg 0x20+ch bit0と組み合わせて9bitの波形番号が決まるため。逆にすると、前のノートのbit8のまま読み込まれ、256ずれた別の波形が鳴る。エミュレーションコアymfm(`pcm_engine::write()`がreg 0x08-0x1Fへの書き込みで`load_wavetable()`を呼ぶ)と、ALSAの`snd_opl4_note_on()`(`OPL4_REG_F_NUMBER`を`OPL4_REG_TONE_NUMBER`より先に書き、後者に「triggers header loading」とコメントしている)で確かめた
- **F-numberとOctaveはAWM専用の式で求める**: 共通の`getFnumberFromHz()`(FM合成のFnum用)は使わない。AWMの再生速度は`step=((0x400|fnum)<<(octave+7))>>2`(ymfm)で、Octave(reg 0x38 bit7-4)は符号付き4bit(-8〜+7)。`COPL4AWM::getFnumber()`は、ALSAの`snd_opl4_update_pitch()`と同じ基準点(note=60・オフセット0で`pitch=60*128=7680`→octave=-3, fnum=0)で計算する
- **波形ごとの校正値**: ROM波形は、どの音程・音量で収録されているかの情報を持たない。`COPL4AWM::getFnumber()`/`updateVolExp()`は、解決したゾーンの`SampleZone`の`pitchOffset`(100/128セント単位)・`keyScaling`(%)・`toneAttenuate`(加算減衰)・`volumeFactor`(0-254の乗算スケール)を、ALSAの`snd_opl4_update_pitch()`/`snd_opl4_update_volume()`と同じ規約で適用する。値はALSAの`sound/drivers/opl4/yrw801.c`と同じ
- **固定の音量ブースト**: `volumeFactor`は254未満の値(GMの移植データでは概ね140〜240)が大半で、`totalLevel=127-(127-totalLevel)*volumeFactor/254`だけを適用するとCC#7/CC#11/ベロシティを最大にしてもレジスタ上の最大音量に届かない。ALSAの`opl4_seq.c`の`volume_boost`(既定8)と同じく、`kVolumeBoost=8`を差し引き、上限を`0x7e`(0x7fではない)でクランプする
- **ミキサーレベル(reg 0xF8/0xF9)**: AWM側のレジスタバンクにしか無く、`COPL4AWM::init()`がFM/PCM両方のミキサーレベルを初期化する唯一の経路。値は3bitずつで、線形の減衰量ではなく8段階のテーブルからの選択。index0(最大音量)で初期化する
- **LevelDirect(reg 0x50 bit0)**: ノートオンの最初の音量設定だけLevelDirect=1(目標値へ即座に合わせる)にし、発音中のCC#7/CC#11の変化はLevelDirect=0(補間)にする(ALSAの`snd_opl4_note_on()`と同じ)。常に0で書くと、チャンネルを再利用したとき直前のノートの音量から目標値まで数十〜百数十msかけて近づくため、短いノートでは音量が立ち上がりきらず、音色も変わって聞こえる。`updateVolExp()`は、reg 0x68のKEYONビット(bit7)が立っていなければノートオン処理中とみなしてLevelDirect=1にする。`ChState::isRunning()`を使わないのは、`CSoundDevice::noteOn()`の中で`s.run()`が実際の`updateKey(true)`より先に実行され、まだKEY ONを送っていない段階で`true`になるため
