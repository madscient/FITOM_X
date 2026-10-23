# チップファミリー別の実装メモ

`docs/design/chip-drivers.md`から、チップファミリーごとの節を分けた。

---

## 4. チップファミリー別クラス階層

### 4.1 OPN系

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
- **FXモード（3rd channel special mode）**：`COPN`のch2専用。`fxCapable_`フラグで
  対応可否を制御し、`COPNA`/`COPN2`は前半サブチップ(chip1_)のみ`true`、
  後半サブチップ(chip2_)は`false`（実機に該当レジスタが存在しないため）。
  `ext.FIX`でモード選択（0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数）、
  `hwOp[i].PDT`(int16_t)が各オペレータの値を持つ。`queryCh`はFXモード要求時に
  ch2固定を強制する（fxCapable_なチップのみ）。
- **`COPNB`**：実機YM2610無印はYM2612/YM2608(6ch)からADPCM制御回路のために
  各サブチップの先頭ch(グローバルch0とch3)を差し引いた実効4ch構成
  (有効なグローバルchは1,2,4,5)。コンストラクタと`init()`の両方で
  `enableCh(0, false)`/`enableCh(3, false)`を呼ぶ(`COPNA::init()`内の
  `reset()`で毎回再有効化されるため、`COPL3_2`と同じパターンで再無効化が
  必要)。旧FITOMは`COPN2`から派生する実装だったが、新実装では`COPNA`が
  共通基底に当たるためこちらから派生する。SSG/ADPCM-A/ADPCM-Bは
  `Config::resolveCompositeSpec()`により同一物理ポートを共有する別デバイス
  として自動生成される(YM2610Bとの違いはFMチャンネル数のみで、
  SSG/ADPCM-A/ADPCM-B自体は無印にも搭載されている)。

#### OPN FXモード (3rd channel special mode)

`COPN`のch2専用機能。実機OPNの「4オペレータそれぞれに独立したF-numberを
指定できるモード」（3rd channel special mode）を実装している。

#### 新規フィールド: `FmHwOp::PDT` (int16_t、オペレータ単位)

```cpp
int16_t PDT;
```

`ext.FIX`（チャンネル単位、ch2全体で1モードを共有）で選択したモードに応じて
解釈が変わる：

| `ext.FIX` | モード | `PDT`の意味 |
|---|---|---|
| 0 | 通常 (FXモード無効) | 未使用 |
| 1 | 疑似デチューン | 100/64セント単位の符号付きオフセット (`getFnumber(ch, PDT)`) |
| 2 | 非整数倍率 | 100/64セント単位オフセット (倍率→セント換算。`1200×log2(倍率)`) |
| 3 | 固定周波数 | 0.1Hz単位の絶対周波数 (`getFnumberFromHz(PDT/10.0)`) |

対数周波数空間では「セント単位の加算」＝「周波数の倍率」と等価なため、
疑似デチューン・非整数倍率は同じ`PDT`型・同じ`getFnumber(ch,offset)`計算経路で
統一的に扱える。固定周波数モードのみ、ノート番号を無視して直接Hz値を指定する
別経路 (`getFnumberFromHz`、新設) を使う。

#### `getFnumberFromHz()` — 直接Hz指定のFnum/Block変換

`CSoundDevice`に新設。`FnumUtils.h`のテーブル生成式（`val = freq×(2^17/master)×divide`）
をそのまま任意のHz値に適用し、11bit(0-2047)に収まるまでオクターブ正規化する。

#### `fxCapable_`フラグ（旧FITOM`fxena`相当）

実機OPNのFXモードは「チップのch2」にのみ存在する物理的制約があるため、
`COPN`コンストラクタに`fxCapable`パラメータを追加した。単体`COPN`(YM2203)は
`true`固定。`COPNA`/`COPN2`（内部に`COPN`を2つ保持するCSpanDevice構成）は
前半サブチップ(port1、ch0-2)のみ`true`、後半サブチップ(port2、ch3-5)は
`false`（実機に該当レジスタが存在しないため）。`queryCh`もFXモード要求時に
ch2固定を強制する（`fxCapable_`なチップのみ、`updateVoice`/`updateFreq`と統一）。

#### OPN (YM2203) — `COPN`

| フィールド | 意味 | 備考 |
|---|---|---|
| `hw.FB` | フィードバック (3bit) | |
| `hw.ALG` | アルゴリズム (3bit、0-7) | |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/MUL/DT1` | 通常のFM4opパラメータ | |
| `ops[i].EGT` | SSG-EG (4bit) | OPN/OPNA系のみ有効 |

##### FXモード（3rd channel special mode、ch2専用）

| フィールド | 意味 |
|---|---|
| `ext.FIX` | モード選択: 0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数 |
| `ops[i].PDT` (int16_t) | モード1/2: 100/64セント単位オフセット。モード3: 0.1Hz単位の絶対周波数 |

ch2以外・`FIX=0`のパッチには影響しない。`queryCh`がFXモード要求時にch2固定を強制する（`COPNA`/`COPN2`の後半サブチップは実機制約により非対応）。

### 4.2 OPM系

```
COPM : CSoundDevice                    (YM2151, 8ch)
  ├── COPP : COPM                      (YM2164、制御ロジック共通)
  └── COPZ : COPM                      (YM2414、WS/DT3/REV/EGS拡張あり)
```

- `updateKey`でリリース中再トリガー対策（`s.wasReleasing`、`ChState`が
  `run()`直前の状態を記録）を実装。他チップにも同様の対策を展開済み。
- `COPZ`は`hwOp[i].REV`/`hwOp[i].EGS`（レジスタ`0xC0+slot`の裏レジスタ、
  bit5=1で選択、オペレータ単位）を実装。REVは3bit幅。
  OPZの2LFOリソース対応は旧FITOMも未完成のため現状維持。
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

#### OPM (YM2151) / OPP (YM2164) — `COPM`/`COPP`

| フィールド | 意味 |
|---|---|
| `hw.FB`/`hw.ALG`/`hw.AMS`/`hw.PMS`/`hw.NFQ` | 通常のFMパラメータ、実機レジスタ直接対応 |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/MUL/DT1/DT2/AM` | 通常のFM4opパラメータ（`DT2`はOPM実機通り2bit） |

##### ノイズモード（ch7専用）

| フィールド | 意味 |
|---|---|
| `ext.ALG_EXT` (bit0) | 1=ノイズ有効。`queryCh`がch7固定を強制する |

---

#### OPZ (YM2414) — `COPZ`（`COPM`派生）

OPMの全フィールドに加え、以下が有効：

| フィールド | 意味 |
|---|---|
| `ops[i].REV` | Reverberation (3bit、オペレータ単位) |
| `ops[i].EGS` | EG bias (2bit、オペレータ単位) |
| `ops[i].WS` | Wave Select (3bit、OPZ独自波形) |
| `ops[i].DT3` | 補助デチューン (OPZ ratio mode、オペレータ単位) |

固定周波数モード（`ext.FIX`、旧FITOM由来のOPZ用途）は未実装のまま（要データシート再確認）。2系統LFOリソースも旧FITOM同様未実装。

パンポットは左寄せを表すビットが無く、左寄せ指定は両出力（センター相当）
として扱われる。

### 4.3 OPL系

```
COPL : CSoundDevice                    (YM3526/YM3801, 9ch, 2OP)
  └── COPL2 : COPL                     (YM3812, 2OP)
COPL3 : CSoundDevice                   (4OPモード専用, 6ch)
COPL3_2 : CSpanDevice                  (内部にCOPL2×2、2OP残余6ch)
```

**【重大バグ修正 2026年7月】** `COPL`/`COPL2`/`COPL3`/`COPLRhythm`の
`updateFreq()`が、B0レジスタに書き込むFnumber上位ビット
(`fnum.fnum >> 9`)を`& 1`で1bitにマスクしていた。実機のB0レジスタは
bit1-0の2bitでFnumber bit9-8を受け取る仕様(YM3812データシート準拠)
のため、このマスクはFnumberが1024以上になる音域(1オクターブの上半分
程度)でbit9側を静かに落とし、実機へ本来より大幅に低い周波数が伝わって
無音・破綻した音になっていた(旧FITOM OPL.cppの該当箇所にはこの
`&1`マスクが無く、移植時に誤って追加されたものと判明)。
`getFnumber()`は11bit精度の値を返す設計のため(前掲の「Fnumberテーブルと
noteoffsetの二重適用に関する既知の注意点」参照)、`fnum.fnum >> 9`は
0-3の2bit値になり得ることを前提に、マスク無しでそのままORする必要が
ある。`COPL3`(4OP側)・`COPLRhythm`にも同一パターンが複製されていた
ため、4箇所とも同時に修正した。

**【重大バグ修正 2026年7月】** `COPL`/`COPL2`/`COPL3`/`COPLRhythm`の
コンストラクタが、`fnumMaster`(実機マスタークロック)に、誤って
呼び出し元から渡される`sampleRate`(オーディオのサンプルレート、
44100等)をそのまま使っていた。`COPN`(正しい設計、`fnumMaster`は
`3993600`等の固定値をデフォルト引数に持つ)とは異なり、当時はOPL系だけが
この誤りを持っていると判明していた(後日OPLL系にも同一パターンが
残っていたことが発覚。下記2026年8月の追記参照)。影響: Fnumber計算式
`freq*(2^17/master)*divide`の`master`に、本来MHz単位であるべき値の
代わりに数万Hz程度の`sampleRate`が使われるため、計算結果が常に
65535(uint16_t上限)にクランプされ、**全てのOPL系チップでピッチが
常に不正確**になっていた(OPL3の疑似デチューン機能の検証中に発見)。
修正: `kMasterClock`という静的定数(`COPL`=3579545Hz、`COPL3`=14318180Hz
=`COPL`の4倍。`divide`(72/288)も4倍の関係になっており、両者は数学的に
同一のFnumberを生成する)をコンストラクタ内で使うよう変更し、
`sampleRate`引数はファクトリ関数シグネチャ一貫性のためだけに残し、
実際には使用しないようにした。

**【重大バグ修正 2026年8月】** 上記と全く同じ「`fnumMaster`に`sampleRate`
をそのまま使っていた」バグが`COPLL`(`COPLL2`/`COPLLP`/`COPLLX`/`CVRC7`が
共通で使う基底コンストラクタ)・`COPLLRhythm`にも残っていたことが判明
(「OPLL系の音程が意図通りにならない」というユーザー報告で発覚。
`tests/test_soundevice_freq.cpp`が`createCOPLLRhythm(&port, 3579545)`と
明示的に正しいクロック値を渡してテストを書いていたため、テストでは
症状が隠れていた)。OPLLはOPL系と同じ3.579545MHz(NTSC colorburst由来)・
`divide=72`の共通ファミリーのため、`OPLL_new.cpp`に`kOpllMasterClock=
3579545`を追加し、`COPL`と同じパターン(コンストラクタ引数`sampleRate`は
ファクトリ関数シグネチャ一貫性のためだけに残し未使用化)で修正した。

- **EGT/RR動的書き換え技法**：`COPL`/`COPL3`/`COPLRhythm`は、キーオン中は
  レジスタ0x20+op(bit5=EGT)を0(decay)に切り替えてSRをRRレジスタ位置に
  書き込み、SRをキーオン中の減衰レイトとして機能させる。キーオフ時は
  EGT=1(sustained)に戻してRRレジスタ本来の値を書く(実機データシート上、
  EGT=0はdecay=KON中もRELEASE RATEが効き続ける、EGT=1はsustained=KON中は
  SUSTAIN LEVELを保持する、という定義。旧FITOM(OPL.cpp)由来のこの実装が
  元々正しい)。
  **【2026年7月、誤った"修正"を一度加えてから差し戻した経緯】** 一時、
  YMF278(OPL4)アプリケーションノートの「PERCUSSIVE SOUND/NON-PERCUSSIVE
  SOUND」節(EGT=0がnon-percussive、EGT=1がpercussiveと明記)を根拠に
  0/1を逆転させたが、これは参照した資料(YMF278)側の記載誤りだった。
  YM3526・YM3812・Y8950・YMF262・YM2413の各データシート/アプリケーション
  マニュアルは全て「EGT=0: percussive/decay tone、EGT=1: non-percussive/
  sustain tone」(YM2413では「EGT (ENVELOPE TYPE): Select sustain/decay」節、
  EGT=1がsustained・EGT=0がdecay)で一致しており、旧FITOM由来の実装
  (EGT=0=decay/EGT=1=sustained)が元々正しかった。**今後EGTの0/1方向を
  再検討する際は、複数のデータシートを突き合わせ、波形図の実際の
  挙動(KEY ONを保持したままSLで平坦になるかRRで減衰し続けるか)で
  判断すること。ビット名の資料間の食い違いに注意。**
- **4OPモード**：`COPL3`は各ポートch0-2(3ch×2ポート=6ch)を4OP専用として使用。
  `hw.ALG`(3bit全体をパック値として使用)が{bit0:前半ペアCON, bit1:後半ペアCON,
  bit2:ConnectionSEL(4OP結合有効化)}を直接表現する(2026年7月、パッチエディタの
  ALG接続図表示を単一パラメータに一元化するため、一時`ext.ALG_EXT`に分離して
  いたConnectionSELを`hw.ALG`へ再統合。分離していた間、`COPL3::updateVoice`の
  0x104(CONNECTIONSEL)レジスタ書き込みと`carmsk[8]`テーブルは`hw.ALG`のbit2を
  参照し続けており、`COPL3::updateKey`だけが`ext.ALG_EXT`を見るという内部
  不整合が生じていたため、`hw.ALG`側への統合で解消した)。
  実機YMF262データシート(B0-B8レジスタ節)は「4OP結合中、後半チャンネル
  自身のBxレジスタ(Key-On/Block/F-Number)は無視され、前半チャンネルの
  値のみが4オペレータ全体に使われる」と明記している。したがって`hw.ALG`の
  bit2が立っている場合(4OP結合)はキーオンを前半ペアのみに送れば足り、
  bit2が0(前半・後半が独立2OPペア×2)の場合こそ両方に送る必要がある
  (キーオフ時は後片付けのため常に両方送る)。2026年7月、この条件が実機
  仕様と逆になっていたバグ(旧FITOM OPL3.cppの`voice->AL & 0x08`相当を
  条件そのまま復元していたが、legacyのbit3は本コードのConnectionSELビット
  とは別物だった)を発見・修正した。`carmsk[8]`テーブルでキャリア判定。
  **2026年8月修正**: `carmsk[8]`のうちbit2=0側(インデックス0-3、独立2OP
  ペア×2)は、前半ペア単体または後半ペア単体のマスクをそのまま使っており、
  もう一方のペアのキャリアopがvol/exp/vel/VTLの影響を一切受けないバグが
  あった(旧: `{0x2,0x3,0x8,0xc}` → 新: `{0xa,0xb,0xe,0xf}`、前半ペア単体
  マスクと後半ペア単体マスクをOR合成)。bit2=1側(インデックス4-7、
  ConnectionSEL有効時の真の4OP結合)はNuked-OPL3の`OPL3_ChannelSetupAlg`
  と突き合わせて検証済みで、`{0x8,0x9,0xa,0xd}`のまま変更なし(現行の
  バンクは全てALG 4-7のみを使うため、この修正だけでは音量/ベロシティ
  無効化の主要因にはならない。真因は別途調査中)。
  `COPL3_2`は残りch6-8(3ch×2ポート)を2OPとして使用（`enableCh`でch0-5を無効化）。
- **疑似デチューン**：`hwOp[0]/[2].PDT`(int16_t、100/64セント単位)を前半/後半
  ペアそれぞれの`getFnumber(ch,offset)`オフセットとして使用。OPNのFXモード
  (疑似デチューン、`ext.FIX=1`)と同じフィールド・同じ計算式を共有する
  (2026年7月〜。以前は`hwOp[0]/[2].DT2`をビット合成した14bit値(±8192)を
  使う独自実装だったが、`PDT`(元々16bit、±32767)に一本化し、より広い
  レンジをより単純な実装で実現した。`DT1`/`DT2`はOPL系では他チップ同様
  「実機に相当機構が無いため0固定」の状態に戻った）。
- **疑似デチューンのキャッシュ機構**：基底クラス`CSoundDevice::updateFnumber`
  は、通常のノート番号ベースのFnum(疑似デチューン無し)を計算した後、
  `updateFreq(ch,&fnum)`という形で、その結果を"強制的に"引数として渡して
  くる。`COPL3::updateFreq`がこの引数をそのまま使ってしまうと、疑似
  デチューンが常にバイパスされてしまう(2026年7月に発見・修正)。これを
  避けるため、`COPL3`は`updateFnumber`をオーバーライドし、基底クラスを
  呼ぶ"前"に疑似デチューンを計算して`pseudoDT1_[ch]`/`pseudoDT2_[ch]`
  にキャッシュしておき、`updateFreq`は渡された`fn`引数を無視して常に
  このキャッシュを参照する(旧FITOM OPL3.cppの`PseudoDT1[ch]`/
  `PseudoDT2[ch]`と同じ設計)。
- **`VOICE_PATCH_OPL3`(0x30)は`COPL3`(4OP)専用**。`COPL3_2`(2OP)は
  独立した`VOICE_PATCH_OPL3_2`(0x22)を持つ（**訂正**：以前`VOICE_PATCH_OPL2`
  と共有する設計にしていたが誤りだった。実機OPL3の2opモードはWSが3bit
  (8波形)まで使えるのに対し実機OPL2はWSが2bit(4波形)までしか対応せず、
  データフォーマットが異なるため独立分類が必要。OPL2/OPLへのフォールバックは、
  各々WS<4/WS==0の場合のみ許可する、`DeviceFactory::acceptsFallback`参照）。

#### OPL (YM3526) / OPL2 (YM3812) — `COPL`/`COPL2`

| フィールド | 意味 |
|---|---|
| `hw.FB` | フィードバック (3bit) |
| `hw.ALG` (bit0のみ) | 0=FM/1=AM、2opのみ |
| `ops[0-1].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/AM/VIB/EGT` | 通常の2opパラメータ |
| `ops[i].WS` | Wave Select (OPL2以降、2bit) |


**適用(FITOM→実機)は`updateVoice`+`updateKey`の2段階方式**
(2026年7月訂正: 以前は「OPL系は`updateVoice`のみで完結する」と誤って
記載していた。実際にはOPL系はキーオン/キーオフのたびに`updateKey`が
動的にEGT/RRを書き換える。なおOPLL系は2026年8月にこの2段階方式を
やめ、`updateVoice`だけで完結する静的変換に変更した。下記OPLL節参照)。

- `updateVoice`(発音チャンネル確保時に1回呼ばれる): `SR>0`かどうかで
  暫定的にEGT/RRを書く。carrier/modulatorを問わず、直後の`updateKey`
  呼び出しで無条件に上書きされる仮の値に過ぎない。

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

**OPLLとの違い**: OPL系はここに書いたとおり`updateKey`で動的に
EGT/RRを書き換えるが、**OPLL系(YM2413等)は2026年8月から動的書き換えを
やめ、`updateVoice`の静的変換のみ**になった(実機のEG挙動がOPL系と
異なり、発音中のEGTビット書き換えが期待どおりに効かないことが実機検証で
確認されたため)。詳細はOPLL専用セクション(下記)を参照。

#### OPL3 (YMF262) 4OPモード — `COPL3`

| フィールド | 意味 |
|---|---|
| `hw.ALG` (bit0-2、3bit全体をパック値として使用) | bit0=CON1(前半ペア接続)、bit1=CON2(後半ペア接続)、bit2=ConnectionSEL(4OP結合有効化)。`updateVoice`の0x104(CONNECTIONSEL)レジスタ書き込み・`carmsk[8]`テーブルはこのbit2を参照する(2026年7月、一時`ext.ALG_EXT`に分離していたことで`updateKey`だけが別フィールドを参照する内部不整合があったのを`hw.ALG`へ統合し解消)。**キーオン連鎖の条件はbit2の値と逆**: 実機YMF262はConnectionSEL=1(4OP結合)の間、後半チャンネル自身のBxレジスタ(Key-On/Block/F-Number)を無視し前半チャンネルの値のみを使う(データシートB0-B8節「one channel uses only one frequency, block and KEY-ON value at a time, regardless whether it is a two- or four-operator channel」)。したがって`updateKey`が後半ペアへもKey-Onを送る必要があるのはbit2=0(独立2OP×2)の場合のみで、bit2=1では前半のみで4オペレータ全てが動作する(2026年7月、条件が実機仕様と逆になっていたバグを修正) |
| `hw.FB` | **前半ペア**(M1/C1)独立フィードバック |
| `hw.FB2` | **後半ペア**(M2/C2)独立フィードバック（実機は前半・後半で別レジスタを持つため分離） |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/WS/AM/VIB/EGT` | 通常の4opパラメータ |

`SR`/`RR`/実機EGTビットの変換規則は、上記OPL/OPL2セクション参照
(OPL3の4opモードも同じ規則)。

##### 疑似デチューン

| フィールド | 意味 |
|---|---|
| `ops[0].PDT` (int16_t) | 前半ペア用、100/64セント単位の符号付きオフセット |
| `ops[2].PDT` (int16_t) | 後半ペア用、同上 |

`ops[i].DT1`/`DT2`は通常のFMデチューンパラメータとして予約されているが、
OPL系チップは実機にDT1/DT2に相当する機構が無いため常に0固定
(未使用)。旧FITOM(OPL3.cpp)はこの2つのフィールドをビット合成して
14bit値(±8192)の疑似デチューンとして転用していたが、2026年7月に
`PDT`(元々16bit、±32767でより広いレンジを持つ)に一本化した
(OPNのFXモードと同じフィールド・同じ計算式を共有する)。

`VOICE_PATCH_OPL3`(0x30)専用。2OP残余（`COPL3_2`）は独立した`VOICE_PATCH_OPL3_2`(0x22)を持つ。
実機OPL3の2opモードはWSが3bit(8波形)まで使えるためOPL2(2bit,4波形)とは別分類とし、
OPL2へのフォールバックは全オペレータでWS<4の場合のみ許可する。

### 4.4 OPLL系

```
COPLL : CSoundDevice                   (YM2413, 9ch, 2OP)
  ├── COPLL2 : COPLL                   (YM2420、Fnumberレジスタ配置が独自)
  ├── COPLLP : COPLL                   (YMF281B)
  ├── COPLLX : COPLL                   (YM2423-X)
  └── CVRC7 : COPLL                    (FS1001、maxChs=6でリズム回路自体を持たない)
COPLLRhythm : CSoundDevice             (OPLL内蔵リズム、5パート、独立レジスタ体系)
```

- **Fnumberビットシフト**：`getFnumber()`は11bit精度を返すが、実機OPLLは9bit。
  `>>2`変換が必須（このシフトが一度欠落し音程が2オクターブ近くズレるバグと
  なっていたため要注意）。`COPLL2`はさらに独自のビット配置を持つため
  `updateFreq`を個別にオーバーライドしている。
- **EGT/RRは静的変換（OPL系の動的書き換えを適用しない）**：2026年8月〜。
  `updateVoice`でユーザー音色を書き込む際に、`SR`の値だけでEGTビットと
  RRレジスタの組を確定させ、`updateKey`はキーオンビットの操作のみを行う。

  | 音色データ | EGTビット(reg 0x00/0x01のbit5) | RRレジスタ(reg 0x06/0x07の下位4bit) |
  |---|---|---|
  | `SR > 0` | 0 (decay) | `SR >> 1` |
  | `SR == 0` | 1 (sustained) | `RR` |

  EGT=0側でRRレジスタに`RR`ではなく`SR`由来の値を書くのは、EGT=0の音色
  データではRRフィールドが未使用（レジスタイメージ由来では0のことが多い）で
  あり、そのまま書くと減衰しなくなるため
  （`docs/manuals/hwpatch-reference.md`のOPLL節の変換表を参照）。
  プリセット音色はROMのためEGパラメータ変更自体が不可で、従来どおり対象外。

  **OPL系(`COPL`/`COPL3`)との違い**：OPL系は4.3節のとおりキーオン/キーオフ
  のたびにEGT/RRを動的に書き換える技法を使い続けるが、実機OPLLのEG挙動は
  OPL系と異なり、発音中のEGTビット書き換えが期待どおりに効かない
  （動的制御を入れた状態では音が想定どおりに減衰しない）。2026年8月の
  実機検証でこの差異が確認されたため、**OPLL系は静的変換を確定仕様とする**。
  チップファミリー間の一貫性という原則に対する意図的な例外であり、
  「OPL系に合わせて動的制御へ戻す」方向の変更は行わないこと
  （実機挙動の差異が理由であり、実装上の未整備ではない）。
  静的変換の副作用として、`SR>0`の音色はキーオフ後のリリースレートも
  `SR`由来の値になり、`RR`フィールドは効かなくなる。

  なおEGTビットの0/1の向き自体は従来どおり（実機データシート(YM2413
  アプリケーションマニュアル「EGT (ENVELOPE TYPE): Select sustain/decay」節)上、
  EGT=1はsustained(KON中はSUSTAIN LEVELを保持)、EGT=0はdecay(KON中も
  RELEASE RATEが効き続ける)。2026年7月、YMF278(OPL4)のアプリケーション
  ノートの記載を誤って根拠にEGTの0/1を逆転させる修正を一度行ったが、
  YM2413側の資料および波形図と整合しないことが判明し差し戻した。
  詳細は4.3節OPL系の同項目を参照）。
- **`COPLLRhythm`**：レジスタ`0x0E`（キーオン）、`0x36-0x38`（音量、2パート/
  レジスタでnibble共有）を独立して操作する。物理ch6/7/8には固定Fnumber
  （`kRhythmFnum`テーブル、旧FITOM完全移植）を書き込む。**親`COPLL`インスタンスへの
  参照を持たず、同一の物理ポートを共有していることを利用して直接`setReg`する**
  設計（旧FITOMは親オブジェクトのメソッド呼び出し経由だったが、レジスタアドレス
  空間はどちらのC++オブジェクトが書き込んでも同じ効果になるため簡略化できた）。

COPNARhythmは各チャンネルが固定ピッチの打楽器を鳴らすのみで、ノート
番号に依存する処理は不要。一方COPLLRhythm(・OPL系のCOPLRhythm)は、
ノート番号に応じてFnum/Blockレジスタを計算し、内蔵ドラム音のピッチ
シフト演奏を可能にしている(基底クラス`CSoundDevice::getFnumber()`の
標準的なノート→Fnum変換をそのまま使う)。これらは5つの楽器に対し3ch分の
Fnumレジスタしかなく、一部の楽器(HH/SD、CYM/TOM)が同じレジスタを
共有する構造だが、HH・CYMはノイズ性の発振でFnumがほぼ発音に寄与しない
ため、HH・CYM側はFnum更新自体を行わない(2026年7月、ユーザー要望。
以前は異なるノート番号で同時発音すると後着優先で上書きされる仕様
だったため、ピッチ変化に意味のあるSD・TOM側のFnumがHH・CYMに意図せず
上書きされることがあった)。結果として、共有チャンネルのFnumは実質的に
SD・TOM側が排他的に制御する形になる。

#### OPLL系 (YM2413/YM2420/YMF281B/YM2423-X) — `COPLL`/`COPLL2`/`COPLLP`/`COPLLX`/`CVRC7`

| フィールド | 意味 |
|---|---|
| `hw.FB`/`hw.ALG` | 通常パラメータ (2op) |
| `ops[0-1].AR/DR/SL/SR/RR/KSR/AM/VIB/EGT` | 通常の2opパラメータ |
| `ops[1].TL` | キャリアTL。音量として`$30`下位4bit(3dB/step)に反映される |
| `ops[0].TL` | モジュレータTL。ユーザー音色レジスタ`$02`下位6bit(0.75dB/step)に反映される。プリセット音色では音色パラメータ自体を持たないため無視される |

##### レジスタイメージからの変換: `SR`/`RR`

OPLLは、実機EGビット(bit5、通称EGTビット、"SUS"表記のこともある)と
RRレジスタを、**`updateVoice`だけで静的に決める**(2026年8月〜)。
OPL系のような`updateKey`によるキーオン/キーオフごとの動的な
書き換えは行わない。

- `updateVoice`(音色設定時に呼ばれる): `SR`の値だけでEGTビットと
  RRレジスタの組を確定させる。

  | 音色データ | EGTビット | RRレジスタ |
  |---|---|---|
  | `SR > 0` | 0(パーカッシブ/decay) | `SR >> 1`(5bit→4bit) |
  | `SR == 0` | 1(サステイン) | `RR`(そのまま4bit) |

- `updateKey`(キーオン/キーオフのたびに呼ばれる): レジスタ`0x20+ch`の
  キーオンビットのみを操作する。EGT/RRには一切触れない。

`SR>0`の音色では、キーオフ後のリリースレートもRRフィールドではなく
`SR`由来の値になる(実機のEGTビット=0はキーオン中もキーオフ後も同じ
RRレジスタで減衰するため、静的変換では両者を分離できない)。`RR`を
リリース専用のレートとして効かせたい場合は`SR=0`にする必要がある。

> **なぜOPL系と方式が違うのか**: OPL系(`COPL`/`COPL3`)は`updateKey`で
> EGT/RRを動的に書き換えるが、実機OPLLのEG挙動はOPL系と異なり、発音中の
> EGTビット書き換えが期待どおりに効かない。2026年8月の実機検証でこの差異が
> 確認されたため、OPLL系は静的変換を確定仕様とする(OPLL/OPLL2/OPLLP/
> OPLLX/VRC7は`COPLL`を共有するため全て同じ挙動)。チップファミリー間の
> 一貫性に対する意図的な例外であり、OPL系に合わせて動的制御へ戻さないこと。

##### プリセット/ユーザー音色判定

| フィールド | 意味 |
|---|---|
| `ext.ALG_EXT` (bit0) | 1=プリセット音色（ROM、EGパラメータ変更不可）、0=ユーザー音色 |

`COPLL2`はFnumberのビット配置が独自（`updateFreq`個別実装）。

##### `COPLLRhythm`（内蔵リズム、5パート）

| フィールド | 意味 |
|---|---|
| `hw.ALG` (下位3bit) | パート番号を直接指定。`queryCh`がこの値で特定chを強制する |

### 4.5 PSG系

```
CPSGBase : CSoundDevice                (ソフトウェアEG/ソフトウェアLFO制御の共通化のみ)
  ├── CSSG : CPSGBase                  (AY-3-8910/YM2149, 3ch)
  │     └── CSSGS : CSSG               (YMZ705/YMZ732 SSG互換部, 3ch×2ユニット = 6ch)
  │           ├── CSSGS3 : CSSGS       (YMZ771 SSG互換部, 6ch。レジスタ配置のみ差し替え)
  │           └── CSSGEX : CSSGS       (Y8960 SSG部, 6ch。レジスタ配置と定位を差し替え)
  ├── CDCSG : CPSGBase                 (SN76489, 4ch)
  └── CSCC : CPSGBase                  (SCC/SCCP/SCCEX, 5ch, 波形ROM)
```

- **`CPSGBase`の設計原則（重要）**：ハードウェア固有の機能は一切持たず、
  「ソフトウェアエンベロープ（`SoftEnvelope`、実機FM相当のADSR）」と
  「ソフトウェアLFO（振幅LFOのみ。`lfoTL_`共有状態を更新して仮想`updateVolExp()`
  を呼ぶだけ）」の制御を共通化するに留める。検証の結果、`updateVolExp`・
  `updateFreq`・ノイズ周波数LFO・`queryCh`（ch2優先）は全て**SSG(CSSG)固有**の
  実装だったため`CSSG`側に移動済み（`CDCSG`/`CSCC`は独自実装で完全に上書きして
  おり、以前は死んだコードだった。特にノイズ周波数LFOはCSCCのch0波形データを
  誤って破壊しうる潜在バグだった）。
- `resetLfoBaseline(ch)`（`lfoTL_[ch]=64`初期化）のみが真に共通のヘルパーとして
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
  シャドウを渡す(非対応チップは`false`を返し、従来どおりHWPortのシャドウが
  使われる)。
- **PSG系のマスタークロックは`IPort::getClock()`から取る**：`DeviceFactory::create()`の
  `sampleRate`引数はHW I/Fプラグインが報告する**音声出力のサンプルレート**
  (既定44100)であり、チップクロックではない。FM系ドライバはマスタークロックを
  自前の定数として持つためこの引数を使わないが、PSG系はトーン周期の算出に
  実クロックが要る。ADPCM系(`createCAdPcm()`)と同じく`port->getClock()`を使い、
  取得できない場合のみ`sampleRate`へフォールバックすること。さらにOPN系
  (YM2608/YM2610/YM2610B)の内蔵SSG部は実機仕様上φM/4で動作するため、
  `DeviceEntry::clockDivider`(composite展開時に`SubDeviceSpec`から設定)で
  分周してから渡す。単独チップのSSGはポートが報告するクロックがそのまま
  SSG部のクロックなので分周しない。
- **PSG系の周波数レジスタは「周期」であり、基底の`getFnumber()`は使えない**：
  周期は音程が上がるほど値が小さくなるため、`CSoundDevice::getFnumber()`が
  行う11bit F-number前提の正規化（bit11が立っていたら右シフトしてblockを
  繰り上げる／`block>7`をクランプして不足分をfnumの左シフトで補う）を通すと
  周期そのものが壊れる。`CPSGBase`はテーブル値とオクターブを素通しする
  周期テーブル専用の`getFnumber()`を持ち、実際のスケーリングは
  `tonePeriod(fn, mask, chipShift)`が「テーブルの基準オクターブ
  （`(noteOffset_ + 69*64)/768`）と実際のノートのオクターブの差」で行う。
  シフト量を`block+3`のような定数で書いてはならない（`FNUM_OFFSET`を
  変更した瞬間に全PSG系のピッチが1オクターブずれる）。分周比がテーブルと
  異なるチップだけが`chipShift`で差分を吸収する（AY8930のExpand Modeは
  1/8分周のため`-1`）。
- **ミックスレジスタのALG=0/1バグ**：`CSSG::computeMixBit`でトーンのみ/ノイズのみ
  の対応ビットが入れ替わっていたバグを修正済み。
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
  対応できる**(YMZ771/SSGS3が実例。レジスタ書き込みの本体コードは一切変えて
  いない)。逆に言うと、これらを経由せず生アドレスを直書きすると派生チップが
  黙って壊れるので、新しいレジスタを足すときも必ず索引メソッドを追加すること。

#### PSG系共通 — `CPSGBase`

全PSG系チップ(SSG/EPSG/DCSG/SAA/SCC)が共通で使うフィールド。
実機レジスタは各チップで異なるため、チップ別セクション参照。

| フィールド | 意味 |
|---|---|
| `ops[0].AR/DR/SL/SR/RR` | ソフトウェアエンベロープ(HW EG未使用時、または実機にHW EG機構が無いチップで使用) |
| `ops[0].TL` | 基準音量(ベロシティ・CC#7/11等と合成される最終ラウドネス計算の起点) |
| `ops[0].EGT` (bit3=0x08) | HWエンベロープ使用フラグ(1=HW EG、0=ソフトウェアエンベロープ)。DCSG/SCCは実機にHW EG機構が無いため常に無視される |

##### 音量レジスタの変換則（チップごとに異なる）

PSG系は音量レジスタの性質がチップごとに違うため、共通化できません。対数DACのチップだけ`fitom::linear2dB()`を通し、リニア乗算のチップは最終ラウドネスを線形にスケールします。

| チップ | 実機の性質 | 変換 |
|---|---|---|
| SSG / EPSG | 対数DAC (SSG=3dB/step、EPSGは5bitで1.5dB/step) | `linear2dB()` |
| DCSG | 対数の減衰量 (**2dB/step**、0-14=0〜-28dB、15=消音) | `kGM2dB`から直接換算(`linear2dB()`は0.75dBの2のべき乗倍しか刻めない) |
| SCC / SAA | **リニア乗算** | ラウドネスを線形にスケール |

リニア乗算のチップに`linear2dB()`を通すと、意図した減衰量がまったく出ません(-24dBのつもりが実際は-6.6dBにしかならず、実効ダイナミックレンジが0〜-23.5dBへ圧縮された上で無音へ落ちます)。

##### 周波数レジスタ（トーン周期）の算出

PSG系の周波数レジスタは F-number ではなく**周期**であり、音程が上がるほど
値が小さくなる。このため`CPSGBase`は周期テーブル専用の`getFnumber()`を
持ち(基底の11bit F-number用正規化を通さない)、`tonePeriod()`が
「テーブルの基準オクターブと実際のノートのオクターブの差」でスケーリング
してからチップのレジスタ幅へクランプする。

| テーブル型 | 生成式 | 使用チップ |
|---|---|---|
| `FnumTableType::SSG` | `master / (16 * freq)` | SSG(YM2149/AY-3-8910)、AY8930 |
| `FnumTableType::TonePeriod` | `master / (32 * freq)` | DCSG(SN76489)、SCC |

分周比がテーブルと異なるチップは`tonePeriod()`の`chipShift`で差分を吸収
する(AY8930のExpand Modeは1/8分周のため`chipShift=-1`)。

2026年7月〜、PSG系は全チップが共通の入口(`VOICE_PATCH_SSG`=0x40固定)/
HwBank名前空間を共有する設計に変更されている。詳細は
`docs/design/patch-resolution.md`の「PSG系共有バンク」参照。

#### SSG (YM2149/AY-3-8910) — `CSSG`

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x07 (ミックス、bit[ch]=トーン/bit[ch+3]=ノイズ、Active Low) | 0=トーンのみ/1=ノイズのみ/2=両方/3=両方無効(消音)。ノイズ要求時は`queryCh`がユニット最終ch(単体SSGならch2)へ固定する |
| `hw.NFQ` (5bit) | 0x06 (ノイズ周波数) | ノイズ要求時のみ書き込まれる |
| `ops[0].EGT` (bit3) | — (フラグのみ、レジスタ直接対応なし) | 1=HWエンベロープ使用、0=ソフトウェアエンベロープ |
| `ops[0].AR/DR/SL/SR/RR` | — (ソフトウェア処理) | ソフトウェアエンベロープ用（`EGT`未使用時） |
| 音量(計算値) | 0x08-0x0A (ch0-2、下位4bit) | 0=無音/15=最大、48dB/3dBステップで変換 |
| Fnum | 0x00-0x05 (ch0-2、各2バイト、下位/上位) | 周期テーブル(`master/(16*freq)`)を12bitへスケーリング |

##### HWエンベロープ（`EGT`bit3=1時）

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `ext.HWEP` (16bit) | 0x0B(Fine)+0x0C(Coarse) | HWエンベロープ周期（実機データシート確認: fine+coarseの単一16bit値、4分割ADSRではない） |
| `ops[0].EGT` (下位4bit) | 0x08+ch(bit4=1固定, 下位4bit) / 0x0D | エンベロープ波形シェイプ (CONT/ATT/ALT/HOLDのビット組み合わせ) |

HW EG使用時、`0x08+ch`のbit4を1に固定してHW EGモードを明示し、下位4bitに波形シェイプを書く。

#### DCSG (SN76489) — `CDCSG`

DCSGはアドレス指定レジスタを持たず、`writeRaw`によるコマンドバイト
列で制御する(通常のsetReg方式ではない)。

| フィールド | 実機コマンド | 意味 |
|---|---|---|
| `hw.ALG` | `==1`でノイズ(ch3固定) | `queryCh`がch3固定を強制 |
| `hw.FB` (bit0) | `0xE0\|((FB&1)<<2)\|(NFQ&3)` のbit2 | ノイズタイプ(周期性/白色雑音) |
| `hw.NFQ` (下位2bit) | 同上のbit0-1 | ノイズ周波数選択(3種+トーン2連動) |
| `ops[0].AR/DR/SL/SR/RR` | — (ソフトウェア処理) | ソフトウェアエンベロープ（HWエンベロープ機構なし） |
| 音量(計算値) | `0x90\|(ch*32)\|att` (ch0-2) / `0xF0\|att` (ch3=ノイズ) | **減衰量**を書く。0=最大音量/15=無音(他のPSG系と極性が逆)。**2dB/step**で0-14が0〜-28dB、15のみ完全消音 |
| Fnum(TonePeriod) | `0x80\|(ch*32)\|(period&0xF)` + 上位6bit別コマンド | ch0-2のみ(ch3=ノイズは周波数レジスタなし)。周期は10bitへクランプ |

#### SCC/SCC+ (K051649/K052539) — `CSCC`

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
レジスタ配置は無印SCC(K051649)とSCC+(K052539)で異なります。無印はch3/ch4が波形メモリを共有して4ブロック(0x00-0x7F)しか持たないため、以降の制御レジスタが前へずれます。

| フィールド | 無印SCC (K051649) | SCC+ (K052539) | 意味 |
|---|---|---|---|
| `ops[0].WS` (7bit) | 0x00-0x7F | 0x00-0x9F | 波形メモリ(32バイト/ch)。波形番号を`SccWaveRegistry`経由で引いて書き込む |
| Fnum(TonePeriod) | 0x80+ch*2 | 0xA0+ch*2 | 下位8bit / 上位4bit の2バイト。周期テーブル(`master/(32*freq)`)を12bitへスケーリング |
| 音量(計算値) | 0x8A+ch | 0xAA+ch | 0=無音/15=最大。**リニア乗算**(`out = wave × vol/15`)であり対数DACではない |
| チャンネル有効 | 0x8F | 0xAF | bit0-4=ch0-4 |
| deform | 0xC0 | 0xC0 | 8bit/4bit周波数モード・波形ローテーション制御。`init()`が0クリアする |

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `ops[0].AR/DR/SL/SR/RR` | — (ソフトウェア処理) | ソフトウェアエンベロープ |

キーオンでチャンネル有効ビットを立てますが、キーオフでは落としません。消音はソフトウェアエンベロープが音量を0まで下げることで行います(キーオフで有効ビットを落とすとリリースが鳴らないため)。

##### ch3/ch4波形メモリ共有制約（無印SCC・SCCEX）

無印SCC(K051649)と、同じ配置を持つY8960のSCC部(`DEVICE_SCCEX`)は、ch3とch4が物理的に波形メモリを共有する実機制約がある。`queryCh`が「ch3が既に確保している波形と完全一致する場合のみch4を割り当てる」制御を行う。SCC+(K052539、`DEVICE_SCCP`)はこの制約が解消されているため通常通り動作する。**パッチデータ側で特別な対応は不要**（`queryCh`が自動的に扱う）。

##### SCCEX (Y8960 SCC部)

無印SCCと同じ配置に、chごとの定位レジスタ（実機の`0x9900`-`0x9904`、FITOMが書く番地は`0x100`-`0x104`。8bit、1=左端 / 128=中央 / 255=右端）を足したもの。音色データは`VOICE_PATCH_SCC`を共有する。

#### AY8930 — `CEPSG`（`CSSG`と別クラス、要`DEVICE_EPSG`指定）

SSGと同じ`hw.ALG`/`hw.NFQ`意味論に加え、Expand Mode(Bank A/B切替、
レジスタ0xdのbit4-5)を常時有効化して拡張機能を使う。ch0はBank A側
(reg 0xb/0xc)、ch1/ch2はBank B側(reg 0x0-0x3)という非対称配置。

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x07 (Bank A、ミックス。SSGと同じ意味論) | 0=トーン/1=ノイズ/2=両方/3=両方無効 |
| `hw.NFQ`(5bit)+`hw.FB`(bit0-2、ノイズ用に転用) | 0x06 (Bank A、8bit拡張ノイズ周波数) | `(NFQ&0x1F)\|((FB&7)<<5)` |
| `ops[0].WS` (4bit) | 0x6+ch (Bank B) | **デューティ比**（矩形波のパルス幅制御、AY-3-8910にはない拡張機能。SCCの波形番号とは無関係、チップ依存の意味の転用パターン） |
| `ops[0].EGT`(bit3)/`SL`/`RR`/`DR`/`SR` | ch0: 0xb/0xc(Bank A) / ch1-2: 0x0-0x3(Bank B) | HW/ソフトウェアエンベロープ切替。ch0とch1/2でレジスタ配置が非対称 |
| `ops[0].EGT`(下位4bit、HW EG時) | 0xd下位4bit(ch0) / 0x3+ch(ch1-2、Bank B) | エンベロープ波形シェイプ |
| 音量(計算値) | 0x08-0x0A (ch0-2、5bit) | 0=無音/31=最大。AY-3-8910より1bit広い |
| Fnum | 0x00-0x05 (ch0-2、各2バイト、16bit) | Expand Modeは1/8分周のため、SSG周期テーブルの2倍(`chipShift=-1`) |

##### Bank A/B の多重化とレジスタダンプ

拡張モードでは同一のレジスタアドレスがレジスタ`0x0D`のbit4でBank A/Bに多重化されます。アドレスだけをキーにするシャドウでは両バンクを区別できないため、`CEPSG`は内部シャドウでBank Bを`0x10`以降へ分離した「32レジスタ」として保持し、チップへ送る際に元のアドレスへ戻します。レジスタダンプもこの表現をそのまま表示するので、**`0x00-0x0F`がBank A、`0x10-0x1F`がBank B**として読めます。この番号体系はAY8930のエミュレータ実装が一般に採るものと一致します。

この`0x10`オフセットはドライバ内部の表現で、HW I/Fプラグインへは常に元のアドレス+バンク選択の形でしか送られません。

旧FITOMの`CEPSG`(EPSG.cpp)を完全移植。

#### SAA1099 — `CSAA1099`

実機データシート確認済み。旧FITOM(SAA.cpp)はSCC波形テーブルを誤って
流用しており、実機とは異なる不完全な実装だったため全面新規実装した。

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x14(トーン有効bit[ch])/0x15(ノイズ有効bit[ch]) | 0=トーンのみ/1=ノイズのみ/2=両方（SSGと同じALG意味論） |
| `hw.NFQ` (下位2bit) | 0x16 (ch0-2→bit0-1、ch3-5→bit4-5の2系統) | ノイズパラメータ（2系統: ch0-2/ch3-5共有） |
| `ops[0].EGT` (bit3) | — (フラグのみ) | 1=HWエンベロープ使用（ch0-2/ch3-5の3ch単位で共有） |
| 音量(計算値、左右独立) | 0x00-0x05 (ch0-5、下位4bit=左/上位4bit=右) | パンポットから等パワーパンニング(cos/sin)で左右音量を算出 |
| Fnum | 0x08-0x0D(周波数8bit、ch0-5) + 0x10-0x12(オクターブ3bit×2ch/レジスタ) | 実機固有の周波数式(`freqReg = 511 - base*2^oct/hz`)を直接計算。他PSG系のFnumTable機構(`FnumTableType`)は使わずNone指定 |
| 全チャンネル有効化 | 0x1C (bit0) | 初期化時に1回設定 |

##### HWエンベロープ（`EGT`bit3=1時、`ext.HWEP`下位6bit流用）

| bit | 実機レジスタ | 意味 |
|---|---|---|
| bit0-2 | 0x18(ch0-2)/0x19(ch3-5)のbit1-3 | mode (波形0-7) |
| bit3 | 同上のbit4 | resolution |
| bit4 | 同上のbit5 | clockSrc |
| bit5 | 同上のbit0 | rightInvert |

HWエンベロープはch0-2/ch3-5の3ch単位で共有されるハードウェア制約があるため、「音色データがデバイスを選択する」原則に従い、`queryCh`が該当グループの空きchを返すのみでモデル化する(専用フィールド不要)。既に同一エンベロープ設定(`ext.HWEP`下位6bit)で発音中のchがあるグループを優先することで、レジスタ競合を避ける。

`queryCh`が「既に同一エンベロープ設定で発音中のchのグループを優先する」制御を行う（ハードウェア制約: 1グループ=1エンベロープ設定）。パンポットは全chが等パワーパンニング（`ChState.panpot`から自動計算、パッチ側での指定は不要）。

#### PSG系のソフトウェアエンベロープ (SoftEnvelope)

`CPSGBase`（`CSSG`/`CDCSG`/`CSCC`共通基底）は、AY-3-8910/YM2149のような
限定的なHWエンベロープしか持たないPSG系チップのため、独自のソフトウェア
ADSRエンベロープジェネレータ（`SoftEnvelope`クラス）を実装している。

- パラメータ範囲はFM音源(OPN)と同一 (AR/DR/SR: 5bit 0-31、SL/RR: 4bit 0-15)
- ATTACKフェーズは乗算的減衰（実機FM音源特有の凹型カーブ）
- DECAY/SUSTAIN/RELEASEフェーズは加算的増加（dB空間で線形＝指数的減衰）
- ベロシティ感度（VAR〜VRR）は他のFMチップと同じ仕組みをそのまま適用
- HW EG使用時（`EGT & 0x08`）はソフトウェアエンベロープを使わず、
  チップのハードウェアエンベロープに完全に委ねる

##### AY-3-8910 HW EGレジスタの仕様修正

実機データシート確認の結果、レジスタ`0x0B`(Fine)+`0x0C`(Coarse)は
**分割不可の単一16bit「Envelope Period」値**であり、
4分割されたADSRパラメータではないことが判明した。
`FmChipExt.HWEP`（新設、16bit）にこの値を直接指定する形に修正した。

```cpp
setReg(0x0B, ext.HWEP & 0xFF);        // Fine
setReg(0x0C, (ext.HWEP >> 8) & 0xFF); // Coarse
setReg(0x0D, hwOp[0].EGT & 0xF);      // Shape (CONT/ATT/ALT/HOLDのビット組み合わせ)
```

### 4.5.1 SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771)

```
CSSGS : CSSG                           (SSG互換部, 3ch×2ユニット = 6ch)
  └── CSSGS3 : CSSGS                   (YMZ771のSSG互換部。レジスタ配置とパンポット分解能のみ差し替え)
CSSGSAdPcm : CAdPcmBase                (ADPCM部, 8ch。SSGS/SSGS2のみ)
```

YM2149相当のSSGを2系統と、サンプル再生部(SSGS/SSGS2はADPCM、SSGS3はAMM)を
1チップに収めた音源。SSGS/SSGS2では両ブロックが同一の8bitレジスタ空間
($00-$3F がSSG、$40-$B3 がADPCM)を共有するため、composite展開
(`DEVICE_SSGS`/`DEVICE_SSGS2` → SSG部 + `DEVICE_SSGS_ADPCM`)しても
`extraPort`やアドレスオフセットは要らない。以下、SSGS3固有の差分は
末尾の「SSGS3 (YMZ771) の差分」にまとめる。

**YMZ732(SSGS2)はYMZ705(SSGS)の上位互換**で、SSG部・ADPCM部ともレジスタ
マップは完全に同一(YMZ732データシートにも「YMZ705(SSGS)とレジスタコンパチ
ブルです」と明記)。したがってドライバは`CSSGS`/`CSSGSAdPcm`をそのまま共用し、
`deviceType`を分けているのは下記のクロック分周比の違いだけのためである。
実際の差分は次の2点:

- **マスタークロックと分周比**: SSGSは4.096MHz÷2または6.144MHz÷3(S6Mピンで
  選択)、SSGS2は12.288MHz÷6固定(クロックは1択で、S6Mピンの役割自体が
  CPUインターフェイスモード選択へ変わっている)。どちらでもSSGブロックは
  2.048MHzになる。分周比がクロック値そのものに依存するので
  `SubDeviceSpec::clockDivider`(固定値)では表現できず、`DeviceFactory`の
  `ssgsSsgBlockClock()`で`deviceType`とクロック値から判定する。この判定規則は
  エミュレーションエンジン(EPSGemuEngineの`ssgsInternalClock`)と一致させること。
- **シンプルアクセスモード(SSGS2のみ)**: `/SEL`ピンをLにすると、データバス
  8本のみ($40-$FFの192コード)で外部ROM上のシーケンスを起動できるハードウェア
  モード。FITOMはCPUインターフェイスからレジスタを直接叩く構成でこのモードを
  使わないため、ドライバ側の対応は不要(外部ROMのシンプルアクセスコード
  テーブル領域`$000240`-`$00047F`も参照しない)。

- **SSG互換部の音色データは単体のSSGと完全に共有する**(`VOICE_PATCH_SSG`)。
  レジスタ互換であり、追加されたパンポットは音色データではなくMIDIの
  CC#10で制御されるため、音色形式に差が出ない。SSGSとSSGS2が混在する構成でも
  同一`VoicePatchType`なので`CSpanDevice`の束ね対象になる(束ねのグループ化キーは
  `VoicePatchType`のみで、`deviceType`はサブチップごとに独立して保持される。
  3節参照)。
- **ADPCM部の`deviceType`はSSGSとSSGS2で共有する**(`DEVICE_SSGS_ADPCM`)。制御が
  完全に同一のため分ける理由が無く、共有することで混在時も自動的に同一
  グループとして束ねられ、PCMバンク/メモリイメージ(カタログ種別`SSGS_ADPCM`)も
  1つで足りる。

- **パンポットのリセット値0は「中央」ではなく左端**(SSGS/SSGS2は4bitで
  0=左端 / 8=中央 / 15=右端、SSGS3は5bitで 0 / 16 / 31)。
  `CSoundDevice::noteOn()`は`panpot`が既定値0のままだと`panDirty`が立たず
  `updatePanpot()`を一度も呼ばないため、`init()`で全chに中央値を書いておかないと
  パンCCを送らない限り全チャンネルが左に張り付く。
- **ADPCM部は他のADPCM系と2点で決定的に異なる**:
  - 波形の開始/終了アドレスレジスタを持たない。チップが外部メモリ先頭の
    ボイステーブル(データシート「8M byte Play data ROM address map」の
    $000000-$00017F)を自力で引くため、ドライバが書くのは6bitのボイス番号
    (`SampleZone::waveIndex`、0-63)だけでよい。よって`registerVoice()`は
    チップへ何も反映しない。PCMメモリイメージ側にこのボイステーブルが
    含まれている必要がある(`pcm_image_catalog`の種別`SSGS_ADPCM`)。
  - 再生ピッチを変えられない。持っているのはサンプリング周波数の4択
    (32k/16k/8k/4k、`pcmbank.json`の`sample_rate`から決まる)だけで、
    ノート番号からは算出しない(`updateFreq()`はno-op)。
- **YMZ705データシートの誤記2箇所**: SSGレジスタマップの$28-$2A/$30-$32は
  「Pan-pot / Frequency」と書かれているが、ビットフィールド(M/L3-L0 と
  P3-P0)と「$20-$2D はYM2149互換」という同ページの注記から、正しくは
  $28-$2Aが音量・$30-$32がパンポット。ADPCMレジスタマップの$42のみ
  P2-P0がD3-D1にずれて描かれているが、他7chと同じくP3-P0がD3-D0が正しい。
  いずれもYMZ732データシートの対応表では正しく記載されており、そちらが
  正しいことの裏付けになっている。

#### SSGS3 (YMZ771) の差分

SSG音源としての機能はSSGS/SSGS2と全く同じで、`CSSGS`を継承する`CSSGS3`が
違いを3点だけ差し替える。音色データも同じ`VOICE_PATCH_SSG`のままなので、
3品種が混在する構成でも同一グループとして束ねられる。

1. **レジスタが「ユニットごと」ではなく「機能ごと」にまとめ直され、`$10`-`$32`の
   連続した空間に並ぶ**。ビットフィールドの意味はYM2149と同一のため、`CSSG`の
   仮想索引メソッド(4.5節)を差し替えるだけで対応でき、レジスタ書き込みの
   本体コードには一切手を入れていない。

   | アドレス | 内容 |
   |---|---|
   | `$10`-`$1B` | トーン周期 TP1A/1B/1C/2A/2B/2C（各2バイト、Fine→Coarse） |
   | `$1C`/`$1D` | ノイズ周期 NP1 / NP2 |
   | `$1E`/`$1F` | ミックス設定（`N*C N*B N*A T*C T*B T*A`、YM2149の`$07`と同配置） |
   | `$20`-`$25` | 音量 M/L（1A,1B,1C,2A,2B,2C） |
   | `$26`-`$29` | エンベロープ周期 EP1（2バイト）/ EP2（2バイト） |
   | `$2A`/`$2B` | エンベロープ形状（CONT/ATT/ALT/HOLD） |
   | `$2C`-`$31` | パンポット（1A,1B,1C,2A,2B,2C） |
   | `$32` | SSGトータルボリューム |

2. **パンポットが5bit**(0-31、16=中央)。SSGS/SSGS2の4bit(0-15、8=中央)から
   分解能だけが上がっている(値とL/Rレベルの対応はデータシートに記載が無く、
   いずれもYMZ280B準拠の分配則で解釈する)。`CSSGS::panMax()`をオーバーライド
   して表現する。
3. **SSGトータルボリューム(`$32`)が追加**。128で100%の線形ボリュームで、
   **リセット値0は完全無音**。パンポットのリセット値(左端)と同じく、`init()`で
   明示的に書かないと一切音が出ない。FITOMは音量をチャンネルごとの音量
   レジスタで作るため、常に100%固定で書く。

クロックは16.384MHzを1/8して2.048MHz。データシートにSSGブロックの動作クロックの
記載は無いが、この分周比がファミリ共通の2.048MHzと一致する。

**AMM部は当面非対応**。ADPCMの代わりに搭載されたMPEG Audio系コーデックの
フレーズ再生部で、生成すべきサブデバイスがSSG部しか無いため、SSGS/SSGS2と
違いcomposite展開もしない(単一デバイスのまま)。

#### SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771) SSG互換部 — `CSSGS`/`CSSGS3`（`CSSG`派生）

YM2149相当のSSGブロックを2系統内蔵し、合計6chを持つ。**音色データは
SSG(`VOICE_PATCH_SSG`)と完全に共有する** — 機能は同一であり、追加された
パンポットは音色データではなくMIDIのCC#10で制御されるため。上表の
フィールドはすべてそのまま適用でき、SSGS/SSGS2ではレジスタアドレスだけが
ユニットごとに`0x20`ずれる。YMZ732(SSGS2)はYMZ705(SSGS)の上位互換でレジスタ
マップは完全に同一、YMZ771(SSGS3)は配置が異なるものの機能は同一なので、
3品種とも同じ音色データがそのまま使える(混在構成でも同一`VoicePatchType`
として束ねられる)。

| ch | ユニット | 実機レジスタ範囲 |
|---|---|---|
| 0-2 | SSG-1 | `$00`-`$0D`（Fnum/ノイズ/ミックス/音量/HW EG）、`$10`-`$12`（パンポット） |
| 3-5 | SSG-2 | `$20`-`$2D`、`$30`-`$32`（同上） |

- ノイズ発生器・HWエンベロープ・ミックスレジスタはユニット内3chで共有される
  (単体のYM2149と同じ制約が2系統ぶん独立して存在する)。ノイズ要求時の
  `queryCh`はch2→ch5の順に空きを探す。
- パンポットはSSGS/SSGS2が4bit（`0`=左端 / `8`=中央 / `15`=右端）、SSGS3のみ
  5bit（`0` / `16` / `31`）。`ChState::panpot`(-64〜+63)を四捨五入で写す。
  値とL/Rレベルの対応はデータシートに記載が無く、いずれもYMZ280B準拠の
  分配則で解釈する。
- SSGブロックの動作クロックはどのチップでも2.048MHzで、周期テーブルの
  基準もこの値。マスタークロックからの分周比だけがチップで異なる
  (SSGS: 4.096MHz÷2 または 6.144MHz÷3 / SSGS2: 12.288MHz÷6 /
  SSGS3: 16.384MHz÷8)。

##### SSGS3 (YMZ771) のレジスタ配置

SSG音源としての機能はSSGS/SSGS2と同一だが、レジスタが「ユニットごと」ではなく
「機能ごと」にまとめ直され`$10`-`$32`に並ぶ。音色データ(`VOICE_PATCH_SSG`)は
共通で、この違いはチップドライバ(`CSSGS3`)が吸収する。

| アドレス | 内容 |
|---|---|
| `$10`-`$1B` | トーン周期 TP1A/1B/1C/2A/2B/2C（各2バイト、Fine→Coarse） |
| `$1C`/`$1D` | ノイズ周期 NP1 / NP2 |
| `$1E`/`$1F` | ミックス設定（YM2149の`$07`と同配置） |
| `$20`-`$25` | 音量 M/L（1A,1B,1C,2A,2B,2C） |
| `$26`-`$29` | エンベロープ周期 EP1 / EP2（各2バイト） |
| `$2A`/`$2B` | エンベロープ形状 |
| `$2C`-`$31` | パンポット（**5bit**、0=左端 / 16=中央 / 31=右端） |
| `$32` | SSGトータルボリューム（128で100%の線形ボリューム。リセット値0は完全無音のため常に100%を書く） |

ADPCMの代わりに搭載されたAMM(MPEG Audio系コーデック)部は当面非対応。

### 4.5.2 DSG (YM2163)

```
CDSG : CSoundDevice                    (YM2163 楽音部, 4ch)
CDSGRhythm : CSoundDevice              (YM2163 内蔵リズム, 5パート)
```

波形メモリ方式のNMOS音源。単一のバスアドレスに対し「D7=1でアドレスラッチ /
D7=0でデータ」の2回書き込みで駆動する実チップだが、この2回書き込みはHW I/F
プラグイン側が吸収するため、ドライバは通常どおり`setReg(0x80〜0x9F, data)`で
書く。ポートは1本のみ(`SplitPort`/`extraPort`は使わない)。

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
- **内蔵リズムは楽音部とレジスタ空間が完全に独立している**(0x90-0x97 と
  0x80-0x8F)。OPL/OPLL系のように`rhythm_mode`でFM側chを潰す必要が無いため、
  `resolveCompositeSpec()`はOPNAの`DEVICE_OPNA_RHY`と同様に
  `DEVICE_DSG_RHY`を**常時生成する**。
- **リズムのレベル書き込みはトリガーより「後」でなければならない**。
  reg 0x90のトリガービットは書くと発音して自動的に0へ戻る
  (=他パートのビットをORしてはならず、同値連打がシャドウキャッシュに
  抑止されないよう`forceWrite`で書く)。このトリガーが内蔵リズムEGのレベルを
  最大へリセットするため、レベル(reg 0x94-0x97)を先に書くと上書きされて
  ベロシティが一切効かなくなる。`CSoundDevice::noteOn()`は
  `updateVolExp()`→`updateKey()`の順で呼ぶので、`CDSGRhythm::updateKey()`が
  トリガー送出の直後に自分でレベルを書き直している。
  レベルはLV4-LV0の線形減衰(0が最大音量、31が最小音量。31でも無音には
  ならない)で、LH(bit0)=0のままにして内蔵リズムEGの減衰を働かせる
  (LH=1は減衰せず固定レベルで鳴り続けるモード)。
- **パート番号はトリガービットの並びに一致させる**:
  0=BD / 1=HC / 2=SDN / 3=HHO / 4=HHD。レベルレジスタはHH/BD/HC/SDNの4本
  しか無く、HHOとHHDは実機のリズム発振器を共有するため同じreg 0x94を使う
  (OPLLリズムのHH/SDがch7を共有するのと同じ構図)。
- **GUIの列挙は別途配線が要る**: 内蔵リズムもDSGのビルトイン音色も、
  通常の`HwBankRegistry`を一切経由しないため、パッチピッカーへは
  自動的には現れない。対応チップ・パート名は
  `gui/bridge/FITOMBridge.cpp`の`kBuiltinRhythmChips[]`/
  `getBuiltinRhythmNames()`、カテゴリ(CC#0)は
  `apps/fitom_gui/PatchPickerDialog.cpp`の`kCategories[]`が情報源で、
  いずれもハードコードされた表。新しい内蔵リズム搭載チップを足したら
  ここも更新すること(コア側の`resolveBuiltinRhythm()`だけ直しても
  GUIからは選べないままになる)。

#### DSG (YM2163) — `CDSG` / `CDSGRhythm`

波形メモリ方式のROM固定音色チップ。**ユーザー音色は存在しない**ため、
音色パラメータを持つHwBank(JSONプリセット)を作ることはできない。
音色は「エンベロープ4種 × 波形5種」の20通りが、Program Changeの値から
機械的に決まる暗黙のバンクとして`PatchManager`が生成する
(`initDsgBuiltinPatches()`)。`CPSGBase`は継承しない(チップ内蔵EGを持ち、
ソフトウェアADSRを走らせる余地が無いため)。

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | reg 0x88+ch bit6-5 (E2E1) | エンベロープ選択(0-3) |
| `ops[0].WS` (下位3bit) | reg 0x88+ch bit2-0 (W3W2W1) | 波形メモリ選択(1-5。0/6/7は未定義=無音) |
| `ops[0].TL` | — (計算値へ合成) | 音色レベル。vol/exp/velと合わせて2bitの音量へ丸められる |
| サスティン(CC#64) | reg 0x88+ch bit4 (SUS) | ハードウェアのサスティンON/OFFに直結 |
| 音量(計算値) | reg 0x8C+ch bit5-4 (VL2VL1) | 0=0dB / 1=-6dB / 2=-12dB / 3=-∞。**4段階のみ** |
| 出力端子 | reg 0x8C+ch bit3-0 (F1-F4) | OR1-OR4への出力可否。定位ではないため常にOR1固定 |
| Fnum(専用計算) | reg 0x80+ch / reg 0x84+ch bit4-0 | `f = clock/(DV * 2^(3-oct))`。DV=10bit、oct=B2*2+B1 |
| キーオン | reg 0x84+ch bit6 (KON) | エッジ検出。同レジスタの分周数上位を壊さないこと |
| 強制減衰 | reg 0x84+ch bit5 (FD) | CC#120(All Sound Off)で使う。同じくエッジ検出 |

##### ビルトイン音色の番号割り当て

Program Change値 0-19 が、以下のように波形メジャー(同じ波形の4エンベロープが
連続する)で並ぶ。音色名も`<波形名>.<エンベロープ名>`で機械生成される。

```
prog = 波形番号(0-4) * 4 + エンベロープ番号(0-3)
  波形番号     0=St / 1=Or / 2=Cl / 3=Pf / 4=Hc   (レジスタ値 W = 波形番号+1)
  エンベロープ 0=Percussive / 1=Wind / 2=Sustain / 3=Plateau
```

| prog | 音色名 | prog | 音色名 | prog | 音色名 |
|---|---|---|---|---|---|
| 0 | `St.Percussive` | 8 | `Cl.Percussive` | 16 | `Hc.Percussive` |
| 1 | `St.Wind` | 9 | `Cl.Wind` | 17 | `Hc.Wind` |
| 2 | `St.Sustain` | 10 | `Cl.Sustain` | 18 | `Hc.Sustain` |
| 3 | `St.Plateau` | 11 | `Cl.Plateau` | 19 | `Hc.Plateau` |
| 4 | `Or.Percussive` | 12 | `Pf.Percussive` | | |
| 5 | `Or.Wind` | 13 | `Pf.Wind` | | |
| 6 | `Or.Sustain` | 14 | `Pf.Sustain` | | |
| 7 | `Or.Plateau` | 15 | `Pf.Plateau` | | |

##### パフォーマンスパッチ(SwPatch)の紐づけ

ビルトイン音色は内部生成のため`swBank`/`swProg`を持てない。ベロシティ感度・
ソフトLFO・トレモロ等を与えるには、OPLL系ROM音色と同じ
`role=="builtin_swpatch_meta"`のメタバンクを使い、
`builtin: { "patch_type": "DSG", "patch_no": <prog 0-19> }`で紐づける
(`PatchManager::resolveDsgBuiltinVoice()`が`findByBuiltinRef()`で探索する)。
OPLL系と1つのファイルを共有でき、`patch_type`で区別される。
詳細は`docs/design/patch-resolution.md`の「ROM/ビルトイン音色へのパフォーマンス
パッチ紐づけ」を参照。

`patch_no`は**0から**有効(prog 0 = `St.Percussive`が正規の音色)。
`BuiltinRef::isValid()`が`patchNo >= 0`である理由がこれで、`>= 1`にすると
prog 0 のエントリだけが黙って一致しなくなる。

##### エンベロープとサスティン(SUS)の関係

SUSは音色パラメータではなくサスティンペダル(CC#64)で動く。
時定数はマスタークロックに比例する(下表は1MHz時)。

| E2E1 | SUS=0 | SUS=1 |
|---|---|---|
| 0 `Percussive` | 即最大 → 60msで1/2 → 1.2sで減衰。KOFFで60msリリース | 同左。**KOFFが効かない** |
| 1 `Wind` | 60msアタック → 保持。KOFFで120msリリース | 60msアタック → 保持。KOFFで1.2sリリース |
| 2 `Sustain` | 即最大 → 60msで1/2 → 保持。KOFFで60msリリース | 同左。KOFFで1.2sリリース |
| 3 `Plateau` | 即最大 → 保持。KOFFで即無音(ゲート) | 即最大 → 保持。KOFFで1.2sリリース |

##### `CDSGRhythm`（内蔵リズム、5パート）

楽音部とレジスタ空間が独立しているため、楽音4chを潰さずに共存する。
パート番号はリズムトリガー(reg 0x90)のビット位置に一致する。

| パート | 音 | トリガー(reg 0x90) | レベル | 出力端子 |
|---|---|---|---|---|
| 0 | BD (バスドラム) | bit0 | reg 0x95 | RH1 |
| 1 | HC (ハイコンガ) | bit1 | reg 0x96 | RH1 |
| 2 | SDN (スネアノイズ) | bit2 | reg 0x97 | RH2 |
| 3 | HHO (ハイハット Open) | bit3 | reg 0x94 | RH2 |
| 4 | HHD (ハイハット Close) | bit4 | reg 0x94 | RH2 |

- パート番号は音色データの`hw.ALG`(下位3bit)で直接指定する
  (`COPLLRhythm`と同じ規約)。
- HHOとHHDは実機のリズム発振器を共有するため、レベルレジスタも共有する。
- トリガービットは書くと発音して自動的に0へ戻る。他パートのビットを
  ORしてはならず、同値連打が抑止されないよう`forceWrite`で書く。
- レベル(LV4-LV0)は**線形減衰**で0が最大音量・31が最小音量(31でも無音には
  ならない)。LH(bit0)=0のままにして内蔵リズムEGの減衰を働かせる。
- **レベルはトリガーの後に書く**こと。トリガーが内蔵リズムEGのレベルを
  最大へリセットするため、先に書くと上書きされてベロシティが効かなくなる。
- 音量はMIDI Volume(CC#7)とベロシティの組み合わせ(Expressionは含まない、
  `COPLLRhythm`と同じ)。音程レジスタは存在しないため`updateFreq`はno-op。

### 4.5.3 Y8960 の拡張ブロック (OPLLEX / OPL2EX / SSGEX / SCCEX)

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
- **SSGEXのクロック**: プロファイルに書くのはSSGブロックの動作クロックそのもの
  (単体のSSGと同じ意味)で、SSGS系のような分周比の換算はしない。FITOMは音量を
  YM2149互換の4bit音量レジスタで作るため、chごとの音量(0-128)は常に128で固定する。
- **SCCEX**は無印SCCと同じ配置のため、ch3/ch4の波形メモリ共有の制約もそのまま
  引き継ぐ(`CSCC::sharesCh34Wave()`)。
- 定位を持つため、OPL2EX/OPLLEXの各サブデバイスは`CLinearPanDevice`の束ね
  (`stereo_pair`)の対象外。
- Y8960のOPM(YM2151)は拡張レジスタを持たない標準のYM2151で、クロックはMSXスロットの
  システムクロック3.579545MHz(仕様書には記載が無く、利用者からの情報)。`chip:"OPM"`として
  既存の`COPM`をそのまま使う(`COPM`は3.579545MHz基準のKC表で音程を出す)。

#### SSGEX (Y8960 SSG部) — `CSSGEX`（`CSSGS`派生）

YM2149相当を2系統内蔵した6ch。音色データはSSG(`VOICE_PATCH_SSG`)と完全に共有する。
YMZ705(SSGS)と同じ「ユニットごと」の配置だが、ユニットの間隔と定位が違う。

| アドレス | 内容 |
|---|---|
| `$00`-`$0D` | SSG-1（YM2149と同一配置） |
| `$10`-`$1D` | SSG-2（YM2149と同一配置） |
| `$20`-`$22` / `$24`-`$26` | ch 1A-1C / 2A-2C の音量（0-128、128で等倍）。FITOMは常に128を書く |
| `$28`-`$2A` / `$2C`-`$2E` | ch 1A-1C / 2A-2C の定位（**8bit**、1=左端 / 128=中央 / 255=右端） |

- 音量はYM2149互換の4bit音量レジスタ（`$08`-`$0A` / `$18`-`$1A`）で作る。
- プロファイルに書くクロックはSSGブロックの動作クロックそのもの（単体のSSGと同じ意味）。

### 4.6 ADPCM系

```
CAdPcmBase : CSoundDevice               (PCMバンク管理・loadVoice純粋仮想の共通基底)
  ├── CYmDelta : CAdPcmBase             (Delta-T方式、YM2608/YM3801/YM2610B ADPCM-B)
  ├── CAdPcm2610A : CAdPcmBase          (YM2610 ADPCM-A、多チャンネルPCM、Delta-Tと無関係)
  ├── CAdPcmZ280 : CAdPcmBase           (YMZ280B/PCMD8, 8ch)
  └── CSSGSAdPcm : CAdPcmBase           (YMZ705/SSGS, 8ch。詳細は4.5.1参照)
```

- **`CYmDelta`はチップごとに異なる`RegMap`（レジスタアドレス集合）を持つ**：
  `kY8950_DeltaT`(YM3801)/`kOPNA_DeltaT`(YM2608)/`kOPNB_DeltaT`(YM2610/YM2610B)の
  3種類。旧FITOMの`REGMAP`構造体（`control1`〜`panmask`の16フィールド）を完全
  移植し、`memory`/`panmask`フィールドの欠落（以前の実装）を修正済み。
  `RegMap`には`addrShift`フィールド（Start/Endアドレスレジスタの境界単位、
  バイトオフセットを右シフトする量）もあり、OPNA/Y8950は2（4byte境界）、
  OPNB/OPNBB(YM2610/2610B)は8（256byte境界、チップ回路仕様上の固定値で
  control2の設定値には依存しない）。以前は全チップに4byte境界を固定で
  使っていたためOPNB/OPNBBの再生アドレスが64倍ズレるバグがあった
  （`AdpcmVoice::startAddr`/`length`はバイト単位で保持し、`registerVoice()`/
  `updateVoice()`が`addrShift`で変換する）。
- **DeltaN(Delta-N、ADPCM-Bの再生ピッチレジスタ)の基準周波数は固定16000Hz**：
  `FnumUtils.h`の`FnumTableType::DeltaN`は他の型(Fnumber/TonePeriod)と違い
  `masterPitch_`(A440チューニング、既定440Hz)ではなく、旧FITOMの
  `Fnum.cpp::CFnumTable::GetDeltaN`以来の固定16000Hz基準を使う。`CYmDelta`の
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
- **`createCAdPcm()`のmasterクロックは`port->getClock()`を使う**：FM系チップ
  ドライバ(`createCOPNA`等)と同様、DeltaN算出のmasterにはサンプルレート
  ではなく実クロックを渡す(取得不可時のみサンプルレートにフォールバック)。
- **OPNA用ADPCM-BとOPNB/OPNBB用ADPCM-Bは、PCMバンク(オフセットテーブル)を
  物理チップ単位に分けて登録する必要がある**：両者は音色パラメータ形式が
  共通のため同一VoicePatchType(`VOICE_PATCH_ADPCMB`)を共有する設計だが、
  実際に配置される波形バイナリのバウンダリ整列(OPNA=32byte、OPNB/OPNBB=
  256byte、上記`addrShift`参照)が異なるため、同じエントリ(オフセット/
  サイズ)テーブルを共有できない。`PcmBankRegistry::findBankNoForVoicePatchType()`
  は「最初に見つかった一致」しか返せないため、これだけに頼ると片方の
  デバイスにもう片方用のオフセットテーブルが誤って割り当てられる
  (2026-07-24、OPNBがOPNA用の32byte境界オフセットを参照してノイズに
  なるバグとして発覚。FitomEmuIF/YMEngine側の`pcm_image_catalog.json`は
  `OPNB_ADPCM-B`という別キーで正しい256byte境界のバイナリを既にロード
  していたため、原因はhwif側ではなくFITOM_X本体のバンク解決側にあった)。
  `PcmBank::deviceType`(`DEVICE_ADPCMB_OPNA`/`DEVICE_ADPCMB`/
  `DEVICE_ADPCMB_Y8950`)と`PcmBankRegistry::findBankNoForDeviceType()`
  (deviceType完全一致、無ければVoicePatchType一致へフォールバック)を
  新設し、`CFITOM::initDevices()`はこちらを先に試す。profile.jsonでは
  `pcm_banks[].chip`("OPNA"/"OPNB"/"OPNBB"/"Y8950")で指定する
  (`Config.cpp`の`resolvePcmBankChipDeviceType()`が変換)。entries[]の
  エントリ番号(WS/waveIndex)とサンプル名の対応は物理チップに依らず共通の
  ため、HwBank/SampleZoneBank側の音色パッチ定義自体はchip指定の有無に
  関わらずOPNA/OPNBどちらのデバイスにも共通で使い回せる(chipで分離
  されるのはオフセットテーブルという実装内部の詳細のみで、パッチ互換性
  は失われない)。chip指定時、entries[]からのnamed patch自動合成
  (`PatchManager::loadPcmBankJson()`)はバンクごとに独立して走るため、
  同一の名前集合が複数バンク番号の下でパッチピッカーに重複表示され得るが、
  `pcm_banks[].offsets_only`(任意、既定false)をtrueにすると、そのバンクは
  Start/Endオフセットテーブルとしてのみ登録され named patch自動合成を
  スキップする。同一group内でchip違いのバンクを複数併用する場合、代表
  (通常はchip省略または最初のバンク)以外に`offsets_only: true`を指定すれば
  パッチピッカーでの重複表示を避けられる。
- **束ねられた(`CSpanDevice`)ADPCM-Bサブチップへは、代表デバイスの
  bankNoではなく各サブチップ自身のdeviceTypeでバンクを解決する**：OPNA/
  OPNB/OPNBBのADPCM-Bは同一VoicePatchType(`VOICE_PATCH_ADPCMB`)のため
  `mergeSpannableDevices()`により1つの`CSpanDevice`へ自動的に束ねられる
  (`docs/worklog/2026-07.md`参照、実機ログで確認済み)。しかし`CMultiDevice::
  setPcmRegistry()`(`MultiDevice.h`)は当初、代表デバイス(束ねの先頭、
  例えばOPNA)のdeviceTypeから解決した1つの`bankNo`を全サブチップへ
  ブロードキャストしていたため、OPNB/OPNBBのサブチップにもOPNA用バンクが
  そのまま伝播し、上記のdeviceType別バンク解決(`findBankNoForDeviceType()`)
  が束ね構成では実質無効化されるバグがあった(2026-07-24)。
  `CMultiDevice::setPcmRegistry()`を、各サブチップについて`c->
  getDeviceType()`(サブチップ自身の実際のdeviceType)で個別に
  `findBankNoForDeviceType()`を引き直し、見つかればそちらを優先するよう
  修正(見つからない場合のみ代表デバイス基準の`bankNo`にフォールバック、
  単一チップ種のみの束ね[SSG複数枚等]との後方互換を維持)。
- **`DEVICE_ADPCM`/`DEVICE_ADPCMB`/`DEVICE_ADPCMB_OPNA`の3分割**：旧FITOMは
  Y8950とOPNAの両方に`DEVICE_ADPCM`を共用していた（クラスが分かれていたため実害
  なし）が、新FITOMの単一ディスパッチ方式では区別が必要なため、
  `DEVICE_ADPCMB_OPNA`(60)を新設してOPNA用ADPCM-Bを独立させた。
  ```
  DEVICE_ADPCM      (119) → Y8950  (kY8950_DeltaT)
  DEVICE_ADPCMB_OPNA (60) → OPNA   (kOPNA_DeltaT)
  DEVICE_ADPCMB     (117) → OPNB   (kOPNB_DeltaT)
  ```
- **`updateKey`の役割分担**：旧FITOM同様、Start/End(`updateVoice`)・
  DeltaN(`updateFreq`)・Volume(`updateVolExp`)は個別のフックで設定し、
  `updateKey`は`stopPcm()`（4段階の停止シーケンス）+固定値`0xa0`による
  純粋な再生トリガーのみ。
- **プログラム番号の統一**：`CYmDelta`/`CAdPcm2610A`/`CAdPcmZ280`全て
  `hwOp[0].WS`(7bit、0-127)を使用。B-3の`resolvePcmEntry`と統一。
  以前は`hw.ALG`(3bit、0-7)や`s.lastNote`など不統一だった。
- **`queryCh(owner, patch, mode)`は必ず`mode`をそのまま基底
  (`CSoundDevice::queryCh`)へ転送すること**：`mode`は1=奪取なし/0=奪取あり
  を表し、`CSpanDevice::allocCh()`はmode=1→0の順で全サブチップを試すことで
  「空きのあるチップを優先し、全チップが本当に埋まって初めて奪取に回る」
  設計になっている。`CAdPcm2610A`が初期実装から`mode`引数を無視して常に
  `queryCh(owner, patch, 0)`固定で呼んでいたため、1台目のサブチップが
  mode=1の問い合わせでも内部で強制奪取して常に空きchを返してしまい、
  `CSpanDevice::allocCh()`のループが1台目で必ず成功し2台目以降に一切
  到達しないバグがあった(2026年7月、ADPCM-Aの2チップ束ねで後半chに
  ボイスが全く割り当てられないと発覚。他のチップドライバは全て`mode`を
  転送しており今回が唯一の例外だった)。意味のないオーバーライドだった
  ため削除し、基底の実装をそのまま継承させて修正。

---

#### ADPCM-B (YM2608/YM2610/YM2610B/YM3801) — `CYmDelta`

| フィールド | 意味 |
|---|---|
| `ops[0].WS` (7bit、0-127) | ROMバンク内のPCMエントリ番号（`resolvePcmEntry`基準） |

`DEVICE_ADPCM`(Y8950)/`DEVICE_ADPCMB_OPNA`(OPNA)/`DEVICE_ADPCMB`(OPNB)でレジスタマップが異なるが、パッチデータ上の意味は共通。

#### ADPCM-A (YM2610) — `CAdPcm2610A`

`ops[0].WS`(7bit)で同様にPCMエントリを指定。

#### PCMD8 (YMZ280B) — `CAdPcmZ280`

`ops[0].WS`(7bit)で同様。4bit ADPCM固定。

#### SSGS ADPCM (YMZ705 / YMZ732) — `CSSGSAdPcm`

**専用スキーマ (`SampleZonePatch`)。** 他のADPCM系と違い、開始/終了アドレスを
チップへ書かない — チップが外部メモリ先頭のボイステーブル(データシート
「8M byte Play data ROM address map」の`$000000`-`$00017F`、ボイス0-63の
Start/Endアドレス)を自力で引くため、ドライバが指定するのはボイス番号だけ。
PCMメモリイメージ側にこのボイステーブルが含まれている必要がある
(`pcm_image_catalog`の種別`SSGS_ADPCM`)。

| フィールド (`SampleZone`) | 意味 |
|---|---|
| `wave_index` | チップのADPCMボイス番号（**6bit、0-63**）。`pcmbank.json`の`entry_no`と一致させる。64以上は指定できず、発音時に警告して無視される |
| `root_note` | **未使用**（再生ピッチを変えられないため） |
| `key_min`/`key_max`/`vel_min`/`vel_max` | 他のサンプルベース音源系と同じ（ゾーン選択に使う） |
| `sw_bank`/`sw_prog` | ベロシティ感度/トレモロのみ有効（ピッチ制御が無いためチャンネルLFO/fine_transposeは無効。ADPCM-Aと同じ制約） |

再生ピッチは変えられず、**サンプリング周波数の4択(32k/16k/8k/4k)のみ**を持つ。
これはノート番号からではなくバンク側の収録レート(`pcmbank.json`の
`sample_rate`)から決まり、そのバンクを使う全chに一律で適用される。

音量は4bit・パンポットは4bit(`0`=左端 / `8`=中央 / `15`=右端、SSG部と同じ分配則)。

#### OPL4 AWM の校正値の実装

**波形ごとのピッチ/音量校正 (`pitch_offset`/`key_scaling`/`tone_attenuate`/
`volume_factor`)**: OPL4AWMのFnumber/Octaveは、ROM波形が実際にどのピッチ・
音量で収録されているかを表す情報を一切持たない(ワーブルテーブルヘッダ
にも記載が無い)ため、絶対Hz(A440基準)からの汎用計算だけでは正しい音高・
音量にならない。`COPL4AWM::getFnumber()`/`updateVolExp()`
(`core/src/OPL4.cpp`)は、ALSAドライバが波形ごとに実測して埋め込んでいる
校正値と全く同じ規約・同じ数値を使う(2026年8月新設。ユーザー報告
「AWMは音は出るが意図した波形と異なる」の調査で、この校正が無いと波形に
よっては数オクターブ単位でピッチがずれることが判明したため、下記の
校正値ごとALSAの表を移植した)。

`volume_factor`は254未満の値(GM移植データでは概ね140〜240)が大半のため、
これだけを適用するとCC#7/CC#11/velocityを全て最大にしてもレジスタ上の
最大音量に到達できず、AWM全体が常に一定量だけ静かになる。ALSA
(`sound/drivers/opl4/opl4_seq.c`)はこれを見込んで既定値8の`volume_boost`
(「Additional volume for OPL4 wavetable sounds」)を差し引いており、
`COPL4AWM::updateVolExp()`も同じ固定値を適用する(2026年8月、ユーザー報告
「OPL4AWMだけミックスレベルがかなり低い」により追加。ミキサーレベル
[reg 0xF8/0xF9]は`COPL4AWM::init()`で既に最大値に設定済みで、他に触る
経路も無いことを確認済み)。

## 7. OPN: updateFreq() は forceWrite=true 必須 (2026年7月修正、実機確認済み)

**症状**: OPNプロファイルにて、あるチャンネルが一度発音した後、ラウンド
ロビンで別のMIDIノートに再割り当てされた際、`ChState::lastNote`/
`lastFnum`(GUIモニター表示にも使われる)は正しく新しいノートを示す
のに、実際にチップから発音される音程が前のノートのままになる不具合が
実機(FitomEmuIF/YMFMEngine)で報告された。

**根本原因**: `CSoundDevice::setReg(reg, data, forceWrite=false)`は
デフォルトで「`regBak_`(自チップ内のレジスタシャドウ)と前回書いた値が
同じなら実書き込み自体を省略する」最適化を持つ。OPNの`updateFreq()`は
F-number上位バイト(0xA4+ch、Block+Fnum上位3bit)・下位バイト(0xA0+ch)
ともこの省略に任せていた(forceWrite省略=false)。C3→D3のように近い
音程では上位バイトの値が一致することが多く(実機再現バグ発生時、
両方ともBlock=4・上位3bit=2で完全一致していた)、このケースで上位
バイトの書き込みが省略され、実際の発音周波数が更新されない不具合が
発生していた。

**検証の経緯**: 当初「MSB(上位)書き込みでFnumがラッチされるため、
LSB(下位)を先に書く必要がある」という仮説で書き込み順序を
LSB→MSBに入れ替えて検証したが、これは**誤りで、症状が悪化した**
(最初のノートオンが無音、以降は不正な音程になる)。実際には元の
MSB→LSBの順序が正しく、`forceWrite=true`を追加するだけで実機確認済み
(2026年7月)。

**修正**: `core/src/OPN_new.cpp`の`updateFreq()`/`updateFxModeFreq()`
(FXモード)で、F-number上位・下位バイトの書き込みに`forceWrite=true`を
明示指定した。書き込み順序(MSB→LSB)は変更していない。

**スコープ**: 同じ理屈(regBak_のスキップ最適化とHW I/Fプラグイン側の
実際の内部状態がズレるリスク)はOPM/OPLL/PSG/ADPCM等、F-number/周期を
複数レジスタに分けて書く他のチップドライバにも当てはまり得るが、
実機確認が取れているのはOPNのみのため、今回はOPNのみ修正した。他
チップで同様の症状が確認された場合は、同じ修正(該当レジスタ書き込みへ
`forceWrite=true`指定)を個別に適用・検証すること。OPL系
(`OPL_new.cpp`/`OPL4.cpp`)は元々F-number上位バイト書き込みが
`forceWrite=true`だった(B0レジスタにKONビットが同居し、キーオンの
たびに値が変化するため、そもそもこの問題の影響を受けにくい構造でも
ある)。
