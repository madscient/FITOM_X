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
| `ops[i].REV` | Reverberation (4bit、オペレータ単位) |
| `ops[i].EGS` | EG bias (7bit、オペレータ単位) |
| `ops[i].WS` | Wave Select (3bit、OPZ独自波形) |
| `ops[i].DT3` | 補助デチューン (OPZ ratio mode、オペレータ単位) |

固定周波数モード（`ext.FIX`、旧FITOM由来のOPZ用途）は未実装のまま（要データシート再確認）。2系統LFOリソースも旧FITOM同様未実装。

---

## OPL (YM3526) / OPL2 (YM3812) — `COPL`/`COPL2`

| フィールド | 意味 |
|---|---|
| `hw.FB` | フィードバック (3bit) |
| `hw.ALG` (bit0のみ) | 0=FM/1=AM、2opのみ |
| `ops[0-1].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/AM/VIB/EGT` | 通常の2opパラメータ |
| `ops[i].WS` | Wave Select (OPL2以降、2bit) |

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

適用(FITOM→実機、`COPL::updateVoice`等の実装)は上記の逆変換:

```cpp
// EGTビット: SR>0ならパーカッシブ(0)、SR==0ならサステイン(0x20)
((o.SR > 0) ? 0 : 0x20)
// RRレジスタ: SR>0ならSRを4bit変換、SR==0ならRRを4bit変換
(o.SR ? ar4(o.SR) : o.RR)
// ar4: 5bit→4bit変換(上位4bitを採用)
static uint8_t ar4(uint8_t v) { return v >> 1; }
```

この変換規則(FITOMの`SR`/`RR`と、実機EGTビット/RRレジスタの対応)は
OPL/OPL2/OPL3(4opモード含む)/OPLL系で共通する。ただしOPLLは、
`updateVoice`(発音チャンネル確保時)と`updateKey`(キーオン/キーオフ
のたび)という2つの関数に分けて実装しており、実際に`SR`の値が
RRレジスタへ反映されるのは`updateKey`のタイミングである点が、
OPL系(`updateVoice`のみで完結)と異なる。詳細はOPLL専用セクション
(下記)を参照。

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
| `ops[1].TL` | キャリアのみTLが意味を持つ（ops[0]はモジュレータ、TL無視） |

### レジスタイメージからの変換: `SR`/`RR`

OPLLは、実機EGビット(bit5、通称EGTビット、"SUS"表記のこともある)と
RRレジスタを、**`updateVoice`と`updateKey`という2つの関数に分けて**
書き込む、OPL系とは異なる実装アプローチを取っている(2026年7月に
検証・訂正)。

- `updateVoice`(発音チャンネル確保時に1回呼ばれる): この時点では
  常にFITOMの`RR`の値のみをRRレジスタに書く(仮の初期値)。
- `updateKey`(キーオン/キーオフのたびに呼ばれる): キーオン中は
  `SR>0`ならEGTビット=0にして`SR`の値(4bit変換)をRRレジスタに、
  キーオフ時はEGTビット=1にして`RR`の値をRRレジスタに、**動的に
  再書き込みする**。

`updateVoice`→(キーオン時)`updateKey`の順で呼ばれるため、**実際に
発音中に有効なのは`updateKey`が書いた値**であり、最終的な変換規則
自体はOPL系と同じ(`SR`の値がパーカッシブモード時の実際の減衰速度と
して、正しく実機に反映される)。`updateVoice`単体だけを見ると
`SR`が無視されているように見えるが、これは仮の初期値に過ぎず、
バグではない。

レジスタイメージから起こす場合の変換(OPL系と同じ規則):

| 実機の状態 | FITOM側への変換 |
|---|---|
| EGTビット=1(サステイン)、RRレジスタ=r | `SR=0`、`RR=r`(そのまま、ビット幅変換不要) |
| EGTビット=0(パーカッシブ)、RRレジスタ=r | `SR = r << 1`(4bit→5bit)、`RR`は任意(キーオフ時にのみ参照されるため) |

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

2026年7月〜、PSG系は全チップが共通の入口(`VOICE_PATCH_SSG`=0x40固定)/
HwBank名前空間を共有する設計に変更されている。詳細は
`docs/patch-structure-design.md`の「PSG系共有バンク」参照。

---

## SSG (YM2149/AY-3-8910) — `CSSG`

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `hw.ALG` (下位2bit) | 0x07 (ミックス、bit[ch]=トーン/bit[ch+3]=ノイズ、Active Low) | 0=トーンのみ/1=ノイズのみ/2=両方/3=両方無効(消音)。`queryCh`がノイズ要求時ch2固定を強制 |
| `hw.NFQ` (5bit) | 0x06 (ノイズ周波数) | ノイズ要求時のみ書き込まれる |
| `ops[0].EGT` (bit3) | — (フラグのみ、レジスタ直接対応なし) | 1=HWエンベロープ使用、0=ソフトウェアエンベロープ |
| `ops[0].AR/DR/SL/SR/RR` | — (ソフトウェア処理) | ソフトウェアエンベロープ用（`EGT`未使用時） |
| 音量(計算値) | 0x08-0x0A (ch0-2、下位4bit) | 0=最大/15=最小(反転極性)、48dB/3dBステップで変換 |
| Fnum | 0x00-0x05 (ch0-2、各2バイト、下位/上位) | `fnum.fnum >> (block+3)`で12bit周期値を算出 |

### HWエンベロープ（`EGT`bit3=1時）

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `ext.HWEP` (16bit) | 0x0B(Fine)+0x0C(Coarse) | HWエンベロープ周期（実機データシート確認: fine+coarseの単一16bit値、4分割ADSRではない） |
| `ops[0].EGT` (下位4bit) | 0x08+ch(bit4=1固定, 下位4bit) / 0x0D | エンベロープ波形シェイプ (CONT/ATT/ALT/HOLDのビット組み合わせ) |

HW EG使用時、`0x08+ch`のbit4を1に固定してHW EGモードを明示し、下位4bitに波形シェイプを書く。

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
| 音量(計算値) | `0x90\|(ch*32)\|vol` (ch0-2) / `0xF0\|vol` (ch3=ノイズ) | 0=最大/15=最小(反転極性) |
| Fnum(TonePeriod) | `0x80\|(ch*32)\|(period&0xF)` + 上位6bit別コマンド | ch0-2のみ(ch3=ノイズは周波数レジスタなし) |

---

## SCC/SCC+ (K051649/K052539) — `CSCC`

| フィールド | 実機レジスタ | 意味 |
|---|---|---|
| `ops[0].WS` (7bit) | 0x00-0x9F (ch×0x20バイトの波形メモリ、32バイト/ch) | 波形番号（`SccWaveRegistry`経由で実際の波形データを引き、メモリへ書き込む） |
| `ops[0].AR/DR/SL/SR/RR` | — (ソフトウェア処理) | ソフトウェアエンベロープ |
| 音量(計算値) | 0xA8-0xAC (ch0-4、下位4bit) | 0=最小/15=最大(正極性、他PSG系と逆) |
| Fnum(TonePeriod) | 0xA0+ch*2(下位8bit)/0xA1+ch*2(上位4bit、12bit周期値) | `fnum.fnum >> (block+3)` |
| キーオン | 0xAA (bit0-4=ch0-4) | — |

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
| 音量(計算値) | 0x08-0x0A (ch0-2、5bit) | 0=最大/31=最小(反転極性)。AY-3-8910より1bit広い |
| Fnum | 0x00-0x05 (ch0-2、各2バイト) | `fnum.fnum >> (block+2)` (SSGより1少ないシフト量) |

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
| `root_note` | 録音時の基準ノート (OPL4AWMはFnumber計算がチップ側で完結するため未使用。将来のADPCM系転用に備えた予約フィールド) |

ノートオン時、`zones[]`を先頭から線形探索し、`key_min <= note <= key_max` かつ
`vel_min <= velocity <= vel_max` を満たす最初のゾーンの`wave_index`を使う。
該当ゾーンが無ければ`zones[0]`にフォールバックする。

バンクファイルは`hw_banks[].group: "AWM"`で指定し、通常の`.hwbank.json`とは
異なる専用スキーマ (`prog`ごとに`zones[]`を持つ、`*.samplezonebank.json`) で
記述する。YRW801内蔵GM ROMの標準マッピングは
`config/profiles/opl4awm_yrw801_gm.samplezonebank.json`
(メロディ128プログラム分) および
`config/profiles/opl4awm_yrw801_drum.samplezonebank.json`
(ドラム、`ws>=128`固定テーブル相当) として提供済み
(Linuxカーネルドライバ`sound/drivers/opl4/yrw801.c`から機械的に抽出し、
元のハードコードロジックとの完全一致を32768通り全組み合わせで検証済み)。

例:
```json
{
  "name": "YRW801 GM (melodic)",
  "patches": [
    {
      "prog": 0,
      "name": "Acoustic Grand Piano",
      "zones": [
        { "key_min": 20, "key_max": 39, "wave_index": 300 },
        { "key_min": 40, "key_max": 45, "wave_index": 301 }
      ]
    }
  ]
}
```

OPL4指定時は`resolveCompositeSpec`により`COPL3`(4OP)+`COPL3_2`(2OP)+`COPL4AWM`の3サブデバイスに自動展開される。

---

## VoicePatchType 対応表

正確な一覧は`chip-driver-architecture.md`の「5. VoicePatchType 対応表」を参照。
