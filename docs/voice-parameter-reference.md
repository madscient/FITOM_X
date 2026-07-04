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
ops (FmHwOp, オペレータ4つ): AR,DR,SL,SR,RR,TL,KSR,KSL,MUL,DT1,DT2,FXV,AM,VIB,EGT,WS
ext (FmChipExt, パッチ1つにつき1組): REV,EGS,DM0,DT3,ALG_EXT,HWEP
```

`ext`/`FB2`/`FXV`等、多くのフィールドは「特定チップのみが参照し、他チップでは
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
| `ext.DM0` | モード選択: 0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数 |
| `ops[i].FXV` (int16_t) | モード1/2: 100/64セント単位オフセット。モード3: 0.1Hz単位の絶対周波数 |

ch2以外・`DM0=0`のパッチには影響しない。`queryCh`がFXモード要求時にch2固定を強制する（`COPNA`/`COPN2`の後半サブチップは実機制約により非対応）。

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
| `ext.REV` | Reverberation (4bit) |
| `ext.EGS` | EG bias (7bit) |
| `ops[i].WS` | Wave Select (3bit、OPZ独自波形) |
| `ops[i].DT3` | 補助デチューン |

固定周波数モード（`ext.DM0`、旧FITOM由来のOPZ用途）は未実装のまま（要データシート再確認）。2系統LFOリソースも旧FITOM同様未実装。

---

## OPL (YM3526) / OPL2 (YM3812) — `COPL`/`COPL2`

| フィールド | 意味 |
|---|---|
| `hw.FB` | フィードバック (3bit) |
| `hw.ALG` (bit0のみ) | 0=FM/1=AM、2opのみ |
| `ops[0-1].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/AM/VIB/EGT` | 通常の2opパラメータ |
| `ops[i].WS` | Wave Select (OPL2以降、2bit) |

---

## OPL3 (YMF262) 4OPモード — `COPL3`

| フィールド | 意味 |
|---|---|
| `hw.ALG` (3bit) | bit0=CON1(前半ペア接続)、bit1=CON2(後半ペア接続)、bit2=ConnectionSEL(4OP結合有効化) |
| `hw.FB` | **前半ペア**(M1/C1)独立フィードバック |
| `hw.FB2` | **後半ペア**(M2/C2)独立フィードバック（実機は前半・後半で別レジスタを持つため分離） |
| `ops[0-3].AR/DR/SL/SR/RR/TL/KSR/KSL/MUL/WS/AM/VIB/EGT` | 通常の4opパラメータ |

### 疑似デチューン

| フィールド | 意味 |
|---|---|
| `ops[0].DT2` (int8_t再解釈) | 前半ペア用、100/64セント単位の符号付きオフセット |
| `ops[2].DT2` (int8_t再解釈) | 後半ペア用、同上 |

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

## SSG (YM2149/AY-3-8910) — `CSSG`

| フィールド | 意味 |
|---|---|
| `hw.ALG` (下位2bit) | 0=トーンのみ/1=ノイズのみ/2,3=両方。`queryCh`がノイズ要求時ch2固定を強制 |
| `hw.NFQ` | ノイズ周波数 (5bit) |
| `ops[0].EGT` (bit3) | 1=HWエンベロープ使用、0=ソフトウェアエンベロープ |
| `ops[0].AR/DR/SL/SR/RR` | ソフトウェアエンベロープ用（`EGT`未使用時） |

### HWエンベロープ（`EGT`bit3=1時）

| フィールド | 意味 |
|---|---|
| `ext.HWEP` (16bit) | HWエンベロープ周期（実機データシート確認: fine+coarseの単一16bit値、4分割ADSRではない） |
| `ops[0].EGT` (下位4bit) | エンベロープ波形シェイプ (CONT/ATT/ALT/HOLDのビット組み合わせ) |

---

## DCSG (SN76489) — `CDCSG`

| フィールド | 意味 |
|---|---|
| `hw.ALG` | `==1`でノイズ。`queryCh`がch3固定を強制 |
| `ops[0].AR/DR/SL/SR/RR` | ソフトウェアエンベロープ（HWエンベロープ機構なし） |

---

## SCC/SCC+ (K051649/K052539) — `CSCC`

| フィールド | 意味 |
|---|---|
| `ops[0].WS` (7bit) | 波形番号（波形メモリへの直接インデックス） |
| `ops[0].AR/DR/SL/SR/RR` | ソフトウェアエンベロープ |

### ch3/ch4波形メモリ共有制約（無印SCCのみ）

無印SCC(K051649)のみ、ch3とch4が物理的に波形メモリを共有する実機制約がある。`queryCh`が「ch3が既に確保している波形と完全一致する場合のみch4を割り当てる」制御を行う。SCC+(K052539、`DEVICE_SCCP`)はこの制約が解消されているため通常通り動作する。**パッチデータ側で特別な対応は不要**（`queryCh`が自動的に扱う）。

---

## AY8930 — `CEPSG`（`CSSG`と別クラス、要`DEVICE_EPSG`指定）

SSGと同じ`hw.ALG`/`hw.NFQ`意味論に加え：

| フィールド | 意味 |
|---|---|
| `ops[0].WS` (4bit) | **デューティ比**（矩形波のパルス幅制御、AY-3-8910にはない拡張機能） |
| `ops[0].EGT`(bit3)/`AR/DR/SL/SR/RR` | SSGと同様（HW/ソフトウェアエンベロープ切替） |

AY8930のExpand Mode（Bank A/B切り替え、5bit音量）は常時有効。旧FITOMの`CEPSG`(EPSG.cpp)を完全移植。

---

## SAA1099 — `CSAA1099`

| フィールド | 意味 |
|---|---|
| `hw.ALG` (下位2bit) | 0=トーンのみ/1=ノイズのみ/2=両方（SSGと同じALG意味論） |
| `hw.NFQ` (下位2bit) | ノイズパラメータ（2系統: ch0-2/ch3-5共有） |
| `ops[0].EGT` (bit3) | 1=HWエンベロープ使用（ch0-2/ch3-5の3ch単位で共有） |

### HWエンベロープ（`EGT`bit3=1時、`ext.HWEP`下位6bit流用）

| bit | 意味 |
|---|---|
| bit0-2 | mode (波形0-7) |
| bit3 | resolution |
| bit4 | clockSrc |
| bit5 | rightInvert |

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

| フィールド | 意味 |
|---|---|
| `ops[0].WS` (uint8_t、フルレンジ0-255、他ADPCM系と異なり7bitマスクしない) | 0-127: GM Program Number（YRW801内蔵ROM、キーレンジ別複数波形に対応）。128-255: ドラム音色（128+GM標準ドラムノート番号）。専用のリズムデバイスは無く、ドラムマッププロファイル側でノートに応じた音色パッチを割り当てる |

OPL4指定時は`resolveCompositeSpec`により`COPL3`(4OP)+`COPL3_2`(2OP)+`COPL4AWM`の3サブデバイスに自動展開される。ベロシティ/CC#11による波形切り替えは実機データに存在しないため非対応（ADSR側のベロシティ感度で近似）。

---

## VoicePatchType 対応表

正確な一覧は`chip-driver-architecture.md`の「5. VoicePatchType 対応表」を参照。
