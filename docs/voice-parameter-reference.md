# ボイスパラメータ リファレンス（チップ種別ごと）

`hwbank.json`の各パッチが持つフィールド（`FmHwVoice`/`FmHwOp`/`FmChipExt`）は、
チップによって意味が異なる、あるいは他チップ向けの値を転用（再解釈）している
場合がある。本ドキュメントは、**チップを選んだときに実際に効くフィールドと
その意味**を素早く参照するためのリファレンスである。

設計の背景・経緯は`voice-data-design.md`、全チップの継承構造は
`chip-driver-architecture.md`を参照。

---

## 共通フィールド構成（型定義）

```
hw  (FmHwVoice, パッチ1つにつき1組): FB, ALG, AMS, PMS, NFQ, FB2
ops (FmHwOp, オペレータ4つ): AR,DR,SL,SR,RR,TL,KSR,KSL,MUL,DT1,DT2,PDT,AM,VIB,EGT,WS,REV,EGS,DT3
ext (FmChipExt, パッチ1つにつき1組): FIX,ALG_EXT,HWEP
```

`ext`/`FB2`/`PDT`等、多くのフィールドは「特定チップのみが参照し、他チップでは
無視される（0固定で問題ない）」という設計になっている。以下、チップごとに
**実際に参照されるフィールドのみ**を記載する。

---

## OPN (YM2203) — `COPN`

| フィールド | 意味 | 備考 |
|---|---|---|
| `hw.FB` | フィードバック (3bit) | |
| `hw.ALG` | アルゴリズム (3bit、0-7) | |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/MUL/DT1` | 通常のFM4opパラメータ | |
| `ops[i].EGT` | SSG-EG (4bit) | OPN/OPNA系のみ有効 |

### FXモード（3rd channel special mode、ch2専用）

| フィールド | 意味 |
|---|---|
| `ext.FIX` | モード選択: 0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数 |
| `ops[i].PDT` (int16_t) | モード1/2: 100/64セント単位オフセット。モード3: 0.1Hz単位の絶対周波数 |

ch2以外・`FIX=0`のパッチには影響しない。`queryCh`がFXモード要求時にch2固定を強制する（`COPNA`/`COPN2`の後半サブチップは実機制約により非対応）。

---

## OPM (YM2151) / OPP (YM2164) — `COPM`/`COPP`

| フィールド | 意味 |
|---|---|
| `hw.FB`/`hw.ALG`/`hw.AMS`/`hw.PMS`/`hw.NFQ` | 通常のFMパラメータ、実機レジスタ直接対応 |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/MUL/DT1/DT2/AM` | 通常のFM4opパラメータ（`DT2`はOPM実機通り2bit） |

### ノイズモード（ch7専用）

| フィールド | 意味 |
|---|---|
| `ext.ALG_EXT` (bit0) | 1=ノイズ有効。`queryCh`がch7固定を強制する |

---

## OPZ (YM2414) — `COPZ`（`COPM`派生）

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

---

## OPL (YM3526) / OPL2 (YM3812) — `COPL`/`COPL2`

| フィールド | 意味 |
|---|---|
| `hw.FB` | フィードバック (3bit) |
| `hw.ALG` (bit0のみ) | 0=FM/1=AM、2opのみ |
| `ops[0-1].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/AM/VIB/EGT` | 通常の2opパラメータ |
| `ops[i].WS` | Wave Select (OPL2以降、2bit) |

`TL`は全チップ共通で**0.75dB/step**である。OPL系の実機TLレジスタが6bit
(0-63、最大47.25dB)、OPN/OPM系が7bit(0-127、最大95.25dB)という違いは
レンジだけで、ステップ幅は同一。したがってOPL系へのTL変換はスケーリング
ではなく**等倍+クランプ**が正しく、`TL >> 1`のような縮小を掛けてはならない
(掛けるとステップ幅が1.5dBに読み替わり、減衰量が半分になる)。SBI/VMA等の
OPLネイティブ形式から起こしたバンクの`TL`は0-63の範囲に収まるが、これは
そのままで正しい。

### レジスタイメージからの変換: `SR`/`RR`/`EGT`(実機EGビット)の関係

OPL系(OPL/OPL2/OPL3)のパッチをレジスタイメージ(実機ダンプ等)から
起こす場合、実機レジスタ`0x20+slot`のbit5(EG Type、通称EGTビット)と
`0x80+slot`の下位4bit(RRフィールド)の組み合わせを、FITOMの`SR`/`RR`
両フィールドに、以下の規則で変換する必要がある(`ops[i].EGT`自体は
OPL系では別の用途(SSG-EGタイプ、OPN/SSG専用)のため無関係)。

実機のEGビットは「サステイン(bit5=1)」と「パーカッシブ(bit5=0)」の
2つの動作モードを切り替える。パーカッシブモードでは、キーオン中でも
RRレジスタの値で減衰し続ける(通常のADSRの"サステイン"に相当する
挙動が無く、代わりにRRが「2段目の減衰」として働く)。FITOMは、この
2つのモードを`SR`(Sustain Rate)フィールドの有無で統一的に表現する。

| 実機の状態 | FITOM側への変換 |
|---|---|
| EGTビット=1(サステイン)、RRレジスタ=r | `SR=0`、`RR = r << 1`(4bit→5bit、下位ビットは0埋めでよい) |
| EGTビット=0(パーカッシブ)、RRレジスタ=r | `SR = r << 1`(4bit→5bit)、`RR`は任意(EGT=1時にのみ参照されるため実機データには反映されない。0のままでよい) |

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

---

## OPL3 (YMF262) 4OPモード — `COPL3`

| フィールド | 意味 |
|---|---|
| `hw.ALG` (bit0-2、3bit全体をパック値として使用) | bit0=CON1(前半ペア接続)、bit1=CON2(後半ペア接続)、bit2=ConnectionSEL(4OP結合有効化)。`updateVoice`の0x104(CONNECTIONSEL)レジスタ書き込み・`carmsk[8]`テーブルはこのbit2を参照する(2026年7月、一時`ext.ALG_EXT`に分離していたことで`updateKey`だけが別フィールドを参照する内部不整合があったのを`hw.ALG`へ統合し解消)。**キーオン連鎖の条件はbit2の値と逆**: 実機YMF262はConnectionSEL=1(4OP結合)の間、後半チャンネル自身のBxレジスタ(Key-On/Block/F-Number)を無視し前半チャンネルの値のみを使う(データシートB0-B8節「one channel uses only one frequency, block and KEY-ON value at a time, regardless whether it is a two- or four-operator channel」)。したがって`updateKey`が後半ペアへもKey-Onを送る必要があるのはbit2=0(独立2OP×2)の場合のみで、bit2=1では前半のみで4オペレータ全てが動作する(2026年7月、条件が実機仕様と逆になっていたバグを修正) |
| `hw.FB` | **前半ペア**(M1/C1)独立フィードバック |
| `hw.FB2` | **後半ペア**(M2/C2)独立フィードバック（実機は前半・後半で別レジスタを持つため分離） |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/WS/AM/VIB/EGT` | 通常の4opパラメータ |

`SR`/`RR`/実機EGTビットの変換規則は、上記OPL/OPL2セクション参照
(OPL3の4opモードも同じ規則)。

### 疑似デチューン

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

---

## OPLL系 (YM2413/YM2420/YMF281B/YM2423-X) — `COPLL`/`COPLL2`/`COPLLP`/`COPLLX`/`CVRC7`

| フィールド | 意味 |
|---|---|
| `hw.FB`/`hw.ALG` | 通常パラメータ (2op) |
| `ops[0-1].AR/DR/SL/SR/RR/KSR/AM/VIB/EGT` | 通常の2opパラメータ |
| `ops[1].TL` | キャリアTL。音量として`$30`下位4bit(3dB/step)に反映される |
| `ops[0].TL` | モジュレータTL。ユーザー音色レジスタ`$02`下位6bit(0.75dB/step)に反映される。プリセット音色では音色パラメータ自体を持たないため無視される |

### レジスタイメージからの変換: `SR`/`RR`

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

レジスタイメージから起こす場合の変換:

| 実機の状態 | FITOM側への変換 |
|---|---|
| EGTビット=1(サステイン)、RRレジスタ=r | `SR=0`、`RR=r`(そのまま、ビット幅変換不要) |
| EGTビット=0(パーカッシブ)、RRレジスタ=r | `SR = r << 1`(4bit→5bit)、`RR`は無視される(0のままでよい) |

### プリセット/ユーザー音色判定

| フィールド | 意味 |
|---|---|
| `ext.ALG_EXT` (bit0) | 1=プリセット音色（ROM、EGパラメータ変更不可）、0=ユーザー音色 |

`COPLL2`はFnumberのビット配置が独自（`updateFreq`個別実装）。

### `COPLLRhythm`（内蔵リズム、5パート）

| フィールド | 意味 |
|---|---|
| `hw.ALG` (下位3bit) | パート番号を直接指定。`queryCh`がこの値で特定chを強制する |

---

## PSG系共通 — `CPSGBase`

全PSG系チップ(SSG/EPSG/DCSG/SAA/SCC)が共通で使うフィールド。
実機レジスタは各チップで異なるため、チップ別セクション参照。

| フィールド | 意味 |
|---|---|
| `ops[0].AR/DR/SL/SR/RR` | ソフトウェアエンベロープ(HW EG未使用時、または実機にHW EG機構が無いチップで使用) |
| `ops[0].TL` | 基準音量(ベロシティ・CC#7/11等と合成される最終ラウドネス計算の起点) |
| `ops[0].EGT` (bit3=0x08) | HWエンベロープ使用フラグ(1=HW EG、0=ソフトウェアエンベロープ)。DCSG/SCCは実機にHW EG機構が無いため常に無視される |

### 音量レジスタの変換則（チップごとに異なる）

PSG系は音量レジスタの性質がチップごとに違うため、共通化できません。対数DACのチップだけ`fitom::linear2dB()`を通し、リニア乗算のチップは最終ラウドネスを線形にスケールします。

| チップ | 実機の性質 | 変換 |
|---|---|---|
| SSG / EPSG | 対数DAC (SSG=3dB/step、EPSGは5bitで1.5dB/step) | `linear2dB()` |
| DCSG | 対数の減衰量 (**2dB/step**、0-14=0〜-28dB、15=消音) | `kGM2dB`から直接換算(`linear2dB()`は0.75dBの2のべき乗倍しか刻めない) |
| SCC / SAA | **リニア乗算** | ラウドネスを線形にスケール |

リニア乗算のチップに`linear2dB()`を通すと、意図した減衰量がまったく出ません(-24dBのつもりが実際は-6.6dBにしかならず、実効ダイナミックレンジが0〜-23.5dBへ圧縮された上で無音へ落ちます)。

### 周波数レジスタ（トーン周期）の算出

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
`docs/patch-structure-design.md`の「PSG系共有バンク」参照。

---

## SSG (YM2149/AY-3-8910) — `CSSG`

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x07 (ミックス、bit[ch]=トーン/bit[ch+3]=ノイズ、Active Low) | 0=トーンのみ/1=ノイズのみ/2=両方/3=両方無効(消音)。ノイズ要求時は`queryCh`がユニット最終ch(単体SSGならch2)へ固定する |
| `hw.NFQ` (5bit) | 0x06 (ノイズ周波数) | ノイズ要求時のみ書き込まれる |
| `ops[0].EGT` (bit3) | — (フラグのみ、レジスタ直接対応なし) | 1=HWエンベロープ使用、0=ソフトウェアエンベロープ |
| `ops[0].AR/DR/SL/SR/RR` | — (ソフトウェア処理) | ソフトウェアエンベロープ用（`EGT`未使用時） |
| 音量(計算値) | 0x08-0x0A (ch0-2、下位4bit) | 0=無音/15=最大、48dB/3dBステップで変換 |
| Fnum | 0x00-0x05 (ch0-2、各2バイト、下位/上位) | 周期テーブル(`master/(16*freq)`)を12bitへスケーリング |

### HWエンベロープ（`EGT`bit3=1時）

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `ext.HWEP` (16bit) | 0x0B(Fine)+0x0C(Coarse) | HWエンベロープ周期（実機データシート確認: fine+coarseの単一16bit値、4分割ADSRではない） |
| `ops[0].EGT` (下位4bit) | 0x08+ch(bit4=1固定, 下位4bit) / 0x0D | エンベロープ波形シェイプ (CONT/ATT/ALT/HOLDのビット組み合わせ) |

HW EG使用時、`0x08+ch`のbit4を1に固定してHW EGモードを明示し、下位4bitに波形シェイプを書く。

---

## SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771) SSG互換部 — `CSSGS`/`CSSGS3`（`CSSG`派生）

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

### SSGS3 (YMZ771) のレジスタ配置

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

---

## DCSG (SN76489) — `CDCSG`

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

---

## DSG (YM2163) — `CDSG` / `CDSGRhythm`

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

### ビルトイン音色の番号割り当て

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

### パフォーマンスパッチ(SwPatch)の紐づけ

ビルトイン音色は内部生成のため`swBank`/`swProg`を持てない。ベロシティ感度・
ソフトLFO・トレモロ等を与えるには、OPLL系ROM音色と同じ
`role=="builtin_swpatch_meta"`のメタバンクを使い、
`builtin: { "patch_type": "DSG", "patch_no": <prog 0-19> }`で紐づける
(`PatchManager::resolveDsgBuiltinVoice()`が`findByBuiltinRef()`で探索する)。
OPLL系と1つのファイルを共有でき、`patch_type`で区別される。
詳細は`patch-structure-design.md`の「ROM/ビルトイン音色へのパフォーマンス
パッチ紐づけ」を参照。

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

### `CDSGRhythm`（内蔵リズム、5パート）

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

---

## SCC/SCC+ (K051649/K052539) — `CSCC`

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

### ch3/ch4波形メモリ共有制約（無印SCCのみ）

無印SCC(K051649)のみ、ch3とch4が物理的に波形メモリを共有する実機制約がある。`queryCh`が「ch3が既に確保している波形と完全一致する場合のみch4を割り当てる」制御を行う。SCC+(K052539、`DEVICE_SCCP`)はこの制約が解消されているため通常通り動作する。**パッチデータ側で特別な対応は不要**（`queryCh`が自動的に扱う）。

---

## AY8930 — `CEPSG`（`CSSG`と別クラス、要`DEVICE_EPSG`指定）

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

### Bank A/B の多重化とレジスタダンプ

拡張モードでは同一のレジスタアドレスがレジスタ`0x0D`のbit4でBank A/Bに多重化されます。アドレスだけをキーにするシャドウでは両バンクを区別できないため、`CEPSG`は内部シャドウでBank Bを`0x10`以降へ分離した「32レジスタ」として保持し、チップへ送る際に元のアドレスへ戻します。レジスタダンプもこの表現をそのまま表示するので、**`0x00-0x0F`がBank A、`0x10-0x1F`がBank B**として読めます。この番号体系はAY8930のエミュレータ実装が一般に採るものと一致します。

この`0x10`オフセットはドライバ内部の表現で、HW I/Fプラグインへは常に元のアドレス+バンク選択の形でしか送られません。

旧FITOMの`CEPSG`(EPSG.cpp)を完全移植。

---

## SAA1099 — `CSAA1099`

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

### HWエンベロープ（`EGT`bit3=1時、`ext.HWEP`下位6bit流用）

| bit | 実機レジスタ | 意味 |
|---|---|---|
| bit0-2 | 0x18(ch0-2)/0x19(ch3-5)のbit1-3 | mode (波形0-7) |
| bit3 | 同上のbit4 | resolution |
| bit4 | 同上のbit5 | clockSrc |
| bit5 | 同上のbit0 | rightInvert |

HWエンベロープはch0-2/ch3-5の3ch単位で共有されるハードウェア制約があるため、「音色データがデバイスを選択する」原則に従い、`queryCh`が該当グループの空きchを返すのみでモデル化する(専用フィールド不要)。既に同一エンベロープ設定(`ext.HWEP`下位6bit)で発音中のchがあるグループを優先することで、レジスタ競合を避ける。

`queryCh`が「既に同一エンベロープ設定で発音中のchのグループを優先する」制御を行う（ハードウェア制約: 1グループ=1エンベロープ設定）。パンポットは全chが等パワーパンニング（`ChState.panpot`から自動計算、パッチ側での指定は不要）。

---

## ADPCM-B (YM2608/YM2610/YM2610B/YM3801) — `CYmDelta`

| フィールド | 意味 |
|---|---|
| `ops[0].WS` (7bit、0-127) | ROMバンク内のPCMエントリ番号（`resolvePcmEntry`基準） |

`DEVICE_ADPCM`(Y8950)/`DEVICE_ADPCMB_OPNA`(OPNA)/`DEVICE_ADPCMB`(OPNB)でレジスタマップが異なるが、パッチデータ上の意味は共通。

## ADPCM-A (YM2610) — `CAdPcm2610A`

`ops[0].WS`(7bit)で同様にPCMエントリを指定。

## PCMD8 (YMZ280B) — `CAdPcmZ280`

`ops[0].WS`(7bit)で同様。4bit ADPCM固定。

## SSGS ADPCM (YMZ705 / YMZ732) — `CSSGSAdPcm`

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

---

## OPL4 AWM (YMF278+YRW801) — `COPL4AWM`

**専用スキーマ (`SampleZonePatch`)。他チップの`HwPatch`/`hwOp[]`とは完全に独立した
別の型を使う。** AWM音源は「1プログラム = 複数キーゾーンへの波形マッピング」
という、FMオペレータ型のパラメータ(AR/DR/SL/RR等)とは本質的に異なる形状の
データを持つため、`ops[]`のフィールドは一切使わない。

| フィールド (`SampleZone`、`zones[]`配列の各要素) | 意味 |
|---|---|
| `key_min`/`key_max` | このゾーンが適用されるMIDIノート範囲 |
| `vel_min`/`vel_max` | ベロシティレイヤー範囲 (省略時0-127=無制限) |
| `wave_index` | YRW801内蔵ROMの波形番号 (チップ側の生値) |
| `root_note` | 録音時の基準ノート (OPL4AWMは下記`pitch_offset`/`key_scaling`ベースの計算を使うため未使用。将来のADPCM系転用に備えた予約フィールド) |
| `pitch_offset` | **OPL4AWM専用**。波形ごとのピッチ校正値(100/128セント単位)。ROM波形は実測でないと絶対ピッチが分からないため必須(下記参照)。既定0 |
| `key_scaling` | **OPL4AWM専用**。波形ごとのノート追従率(%、100=通常追従、0=固定ピッチ)。既定100 |
| `tone_attenuate` | **OPL4AWM専用**。波形ごとの追加減衰量(7bit、加算)。既定0 |
| `volume_factor` | **OPL4AWM専用**。波形ごとの音量スケール(0-254、254=無補正)。既定254 |
| `sw_bank`/`sw_prog` | パフォーマンスパッチ(SwPatch)参照(`HwPatch::swBank/swProg`と同じ規約、-1=参照なし、2026年7月新設)。DrumNote側の個別上書きが優先される。**対応範囲**: ADPCM-B/PCMD8=全機能、ADPCM-A=ベロシティ感度/トレモロのみ(ピッチ制御不可のため)、AWM=参照は解決されるが音には未反映(実機LFO/VIBレジスタとの整合設計が別途必要)。詳細は`patch-structure-design.md`の「サンプルベース音源系へのパフォーマンスパッチ紐づけ」節参照 |

ノートオン時、`zones[]`を先頭から線形探索し、`key_min <= note <= key_max` かつ
`vel_min <= velocity <= vel_max` を満たす最初のゾーンの`wave_index`を使う。
該当ゾーンが無ければ`zones[0]`にフォールバックする。

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

バンクファイルは`hw_banks[].group: "AWM"`で指定し、通常の`.hwbank.json`とは
異なる専用スキーマ (`prog`ごとに`zones[]`を持つ、`*.samplezonebank.json`) で
記述する。YRW801内蔵GM ROMの標準マッピングは
`config/profiles/opl4awm_yrw801_gm.samplezonebank.json`
(メロディ128プログラム分) および
`config/profiles/opl4awm_yrw801_drum.samplezonebank.json`
(ドラム、`ws>=128`固定テーブル相当) として提供済み
(Linuxカーネルドライバ`sound/drivers/opl4/yrw801.c`
[`opl4_local.h`/`opl4_synth.c`/`yrw801.c`]から機械的に抽出。`wave_index`/
`key_min`/`key_max`は2026年7月時点で既に移植済みだったが、`pitch_offset`/
`key_scaling`/`tone_attenuate`/`volume_factor`は2026年8月に追加移植した。
移植スクリプトはyrw801.cの`regions_XX[]`配列をプログラム番号ごと・
`regions_drums[]`をドラム用としてパースし、既存JSONの各ゾーンへ
`wave_index`[+key範囲]完全一致でマージするもので、GM 553ゾーン・
ドラム57ゾーンとも欠落・曖昧一致ゼロで全件マッチした)。

例:
```json
{
  "name": "YRW801 GM (melodic)",
  "patches": [
    {
      "prog": 0,
      "name": "Acoustic Grand Piano",
      "zones": [
        { "key_min": 20, "key_max": 39, "wave_index": 300,
          "pitch_offset": 7474, "key_scaling": 100,
          "tone_attenuate": 0, "volume_factor": 200 },
        { "key_min": 40, "key_max": 45, "wave_index": 301,
          "pitch_offset": 6816, "key_scaling": 100,
          "tone_attenuate": 0, "volume_factor": 200 }
      ]
    }
  ]
}
```

OPL4指定時は`resolveCompositeSpec`により`COPL3`(4OP)+`COPL3_2`(2OP)+`COPL4AWM`の3サブデバイスに自動展開される。

---

## VoicePatchType 対応表

正確な一覧は`chip-driver-architecture.md`の「5. VoicePatchType 対応表」を参照。
