# チップドライバ アーキテクチャ

新FITOMの全チップドライバの継承構造、sub-device自動生成、同種デバイス自動束ね、
VoicePatchType対応を横断的にまとめる。個々のチップドライバの検証過程で得られた
設計判断・既知の制限も併記する。

---

## 1. 基底クラス階層

```
ISoundDevice (抽象インターフェース)
  ├── CSoundDevice          単体チップの共通実装 (レジスタキャッシュ・
  │                         ChState管理・VoiceProcessor連携等)
  │     └── (各チップドライバの直接の基底、下記2章参照)
  │
  └── CMultiDevice           複数 ISoundDevice を束ねる共通基盤
        ├── CSpanDevice      グローバルch→実チップ+ローカルchに変換して委譲
        │                    (sub-device自動生成・同種デバイス束ね両方で使用)
        └── CUnison          全チップに同一ch番号をブロードキャスト
                             (デチューン等のユニゾン効果、現状未使用)
```

`CMultiDevice`/`CSpanDevice`/`CUnison`は`MultiDevice.h`にヘッダーとして実装されており、
チップドライバ側（`OPN2_new.cpp`/`OPL_new.cpp`等）から`#include`して直接継承できる。

### ISoundDeviceで新設した主要メソッド（本来チップ固有だがpublic化したもの）

| メソッド | 理由 |
|---|---|
| `updateTL(ch, op, lev)` | `CRhythmCh`のソフトLFO処理から直接呼ぶ必要があるため |
| `getChState(ch)` | `VoiceProcessor::onNoteOn`等が`CSpanDevice`経由でも正しく実チップの状態を取得できるようにするため |
| `setCC1Modulation(...)` | 同上 |
| `forceDamp(ch)` | デフォルト実装は`noteOff(ch)`のみ。`CMultiDevice`にも委譲実装が必要（3章参照） |

---

## 2. Sub-device 自動生成 (composite chip)

**1つの物理/エミュレーターチップ指定から、内部的に複数の`ISoundDevice`インスタンスを
自動生成する仕組み。** 実機の1チップが複数の独立した音源機能（FM本体・SSG・ADPCM・
内蔵リズム等）を持つ場合に対応する。`Config::resolveCompositeSpec()`が展開ルールを持ち、
`Config::pushDeviceEntries()`が実際に複数の`DeviceEntry`を生成する。各サブデバイスは
同一の物理ポートを共有する。

```cpp
struct SubDeviceSpec {
    uint32_t    deviceType;
    const char* labelSuffix;
    bool        usesExtraPort;  // 2ポート目(extraPort)を必要とするか
    bool        rhythmCapable;  // rhythm_modeプロファイル設定をこのサブデバイスに適用するか
};
```

### 展開ルール一覧（`resolveCompositeSpec`）

| baseDeviceType | 展開されるサブデバイス |
|---|---|
| `DEVICE_OPNA` / `DEVICE_F286` / `DEVICE_OPN3` | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMB_OPNA` + `DEVICE_OPNA_RHY`(6パート) |
| `DEVICE_2610B` (OPNB/OPNBB) | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch) + `DEVICE_ADPCMB`(1ch) |
| `DEVICE_OPL3` / `DEVICE_OPN3_L3` | `DEVICE_OPL3`(4OPモード,6ch) + `DEVICE_OPL3_2`(2OP残余,6ch) |
| `DEVICE_OPLL` / `OPLL2` / `OPLLP` / `OPLLX` | FM本体(9ch) + `DEVICE_OPLL_RHY`(5パート、rhythm_mode時) |

上記以外（単体`COPN`、`COPM`系、`CSSG`単体等）は展開されず、1エントリ=1デバイスのまま。

---

## 3. 同種デバイス自動束ね (CSpanDevice bundling)

**プロファイルに同一`VoicePatchType`・同一物理接続種別・同一パン設定のデバイスが
複数存在する場合、`CSpanDevice`で1つの論理デバイスに自動統合する。** 旧FITOMの
`isSpannable`機構の後継だが、判定基準を**厳密な`deviceType`一致**から
**`VoicePatchType`一致**に緩和している（`DEVICE_OPNA`と`DEVICE_2610B`のFM部分は
どちらも`VOICE_PATCH_OPN2`であり、新FITOMでは同じ`createCOPNA()`実装にルーティング
されるため、束ね対象になる）。

`Config::mergeSpannableDevices()`が`buildDevice()`完了後（**sub-device展開の後**）に
1回実行され、統合されたエントリは`devices_[]`から削除される。実際のマルチチップ
`ISoundDevice`生成は`CFITOM::initDevices()`側が担当し、束ね候補ポートそれぞれに
`DeviceFactory::create()`を呼んだ上で`CSpanDevice`にラップする。

```
グループ化キー: (VoicePatchType, IPort::getInterfaceDesc(), IPort::getPanpot())
```

`forceDamp`が`CMultiDevice`にデフォルトで委譲実装を持たない問題（`noteOff`にフォール
バックしてしまい急速減衰が効かない）を発見し、`CSpanDevice`/`CUnison`双方に明示的な
委譲実装を追加済み。

---

## 4. チップファミリー別クラス階層

### 4.1 OPN系

```
COPN : CSoundDevice                    (YM2203, 3ch, 単体使用)
COPNA : CSpanDevice                    (内部に COPN×2 を保持、6ch)
  └── COPN2 : COPNA                    (YM2612系、ADPCM/FMenableレジスタなし)
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

### 4.2 OPM系

```
COPM : CSoundDevice                    (YM2151, 8ch)
  ├── COPP : COPM                      (YM2164、制御ロジック共通)
  └── COPZ : COPM                      (YM2414、WS/DT3/REV/EGS拡張あり)
```

- `updateKey`でリリース中再トリガー対策（`s.wasReleasing`、`ChState`が
  `run()`直前の状態を記録）を実装。他チップにも同様の対策を展開済み。
- `COPZ`は`hwOp[i].REV`/`hwOp[i].EGS`（レジスタ`0xC0+slot`、オペレータ単位、
  2026年7月にFmChipExtから移設）を実装。旧FITOM自体が未実装だった機能。
  OPZの2LFOリソース対応は旧FITOMも未完成のため現状維持。

### 4.3 OPL系

```
COPL : CSoundDevice                    (YM3526/YM3801, 9ch, 2OP)
  └── COPL2 : COPL                     (YM3812, 2OP)
COPL3 : CSoundDevice                   (4OPモード専用, 6ch)
COPL3_2 : CSpanDevice                  (内部にCOPL2×2、2OP残余6ch)
```

**【重大バグ修正 2026年7月】** `COPL`/`COPL2`/`COPL3`/`COPLRhythm`の
コンストラクタが、`fnumMaster`(実機マスタークロック)に、誤って
呼び出し元から渡される`sampleRate`(オーディオのサンプルレート、
44100等)をそのまま使っていた。`COPN`(正しい設計、`fnumMaster`は
`3993600`等の固定値をデフォルト引数に持つ)とは異なり、OPL系だけが
この誤りを持っていた。影響: Fnumber計算式`freq*(2^17/master)*divide`
の`master`に、本来MHz単位であるべき値の代わりに数万Hz程度の
`sampleRate`が使われるため、計算結果が常に65535(uint16_t上限)に
クランプされ、**全てのOPL系チップでピッチが常に不正確**になって
いた(OPL3の疑似デチューン機能の検証中に発見)。修正:
`kMasterClock`という静的定数(`COPL`=3579545Hz、`COPL3`=14318180Hz
=`COPL`の4倍。`divide`(72/288)も4倍の関係になっており、両者は数学的に
同一のFnumberを生成する)をコンストラクタ内で使うよう変更し、
`sampleRate`引数はファクトリ関数シグネチャ一貫性のためだけに残し、
実際には使用しないようにした。

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
- **EGT/RR動的書き換え技法**：OPN/OPL3と同じ技法（キーオン中はSRをRR位置に、
  キーオフ時はEGT=1に切り替えてRRをリリースレイトとして使う）をユーザー音色
  （プリセット音色以外）に実装。プリセット音色はROMのためEG変更不可。
- **`COPLLRhythm`**：レジスタ`0x0E`（キーオン）、`0x36-0x38`（音量、2パート/
  レジスタでnibble共有）を独立して操作する。物理ch6/7/8には固定Fnumber
  （`kRhythmFnum`テーブル、旧FITOM完全移植）を書き込む。**親`COPLL`インスタンスへの
  参照を持たず、同一の物理ポートを共有していることを利用して直接`setReg`する**
  設計（旧FITOMは親オブジェクトのメソッド呼び出し経由だったが、レジスタアドレス
  空間はどちらのC++オブジェクトが書き込んでも同じ効果になるため簡略化できた）。

### 4.5 PSG系

```
CPSGBase : CSoundDevice                (ソフトウェアEG/ソフトウェアLFO制御の共通化のみ)
  ├── CSSG : CPSGBase                  (AY-3-8910/YM2149, 3ch)
  ├── CDCSG : CPSGBase                 (SN76489, 4ch)
  └── CSCC : CPSGBase                  (SCC/SCCP, 5ch, 波形ROM)
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
- **ミックスレジスタのALG=0/1バグ**：`CSSG::computeMixBit`でトーンのみ/ノイズのみ
  の対応ビットが入れ替わっていたバグを修正済み。

### 4.6 ADPCM系

```
CAdPcmBase : CSoundDevice               (PCMバンク管理・loadVoice純粋仮想の共通基底)
  ├── CYmDelta : CAdPcmBase             (Delta-T方式、YM2608/YM3801/YM2610B ADPCM-B)
  ├── CAdPcm2610A : CAdPcmBase          (YM2610 ADPCM-A、多チャンネルPCM、Delta-Tと無関係)
  └── CAdPcmZ280 : CAdPcmBase           (YMZ280B/PCMD8, 8ch)
```

- **`CYmDelta`はチップごとに異なる`RegMap`（レジスタアドレス集合）を持つ**：
  `kY8950_DeltaT`(YM3801)/`kOPNA_DeltaT`(YM2608)/`kOPNB_DeltaT`(YM2610/YM2610B)の
  3種類。旧FITOMの`REGMAP`構造体（`control1`〜`panmask`の16フィールド）を完全
  移植し、`memory`/`panmask`フィールドの欠落（以前の実装）を修正済み。
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

---

## 5. VoicePatchType 対応表（チップドライバ横断）

`Config::deviceTypeToVoicePatchType()`が`deviceType`(DEVICE_*)→`VoicePatchType`
(VOICE_PATCH_*)を変換する。sub-device自動生成・同種デバイス束ねは全てこの値を
基準に動作する。詳細は`patch-structure-design.md`参照。

| VoicePatchType | 対応する deviceType | 生成クラス | オペレータ数 |
|---|---|---|---|
| `VOICE_PATCH_OPN`(0x10) | OPN, OPNB, OPNC | `COPN` | 4 |
| `VOICE_PATCH_OPN2`(0x11) | OPN2, OPN2C, OPN2L, OPNA, OPN3L, 2610B, F286, OPN3 | `COPNA` / `COPN2` | 4 |
| `VOICE_PATCH_OPM`(0x19) | OPM, OPP | `COPM` / `COPP` | 4 |
| `VOICE_PATCH_OPZ`(0x1a) | OPZ | `COPZ` | 4 |
| `VOICE_PATCH_OPZ2`(0x1b) | OPZ2 | `COPZ`（共用） | 4 |
| `VOICE_PATCH_OPL`(0x20) | OPL, Y8950 | `COPL` | 2 |
| `VOICE_PATCH_OPL2`(0x21) | OPL2 | `COPL2` | 2 |
| `VOICE_PATCH_OPL3_2`(0x22) | OPL3_2（OPL3の2opモード） | `COPL3_2` | 2 |
| `VOICE_PATCH_OPL_RHY`(0x23) | OPL_RHY（OPL系内蔵リズムチャンネル） | `COPLRhythm` | 1または2(楽器により異なる、混在可) |
| `VOICE_PATCH_OPLL`(0x28) | OPLL, OPLL2 | `COPLL` / `COPLL2` | 2 |
| `VOICE_PATCH_OPLLP`(0x29) | OPLLP | `COPLLP` | 2 |
| `VOICE_PATCH_OPLLX`(0x2a) | OPLLX | `COPLLX` | 2 |
| `VOICE_PATCH_VRC7`(0x2b) | VRC7 | `CVRC7` | 2 |
| `VOICE_PATCH_OPL3`(0x30) | OPL3, OPN3_L3 | `COPL3`（4OPモード専用） | 4 |
| `VOICE_PATCH_SD1`(0x38) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_MA3`(0x39) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_MA5`(0x3a) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_MA7`(0x3b) | (未実装) | - | 不明(将来実装時に確定) |
| `VOICE_PATCH_SSG`(0x40) | SSG, PSG, SSGL, SSGLP, SSGS, DSG | `CSSG` | 1 |
| `VOICE_PATCH_EPSG`(0x41) | EPSG | `CSSG`（共用） | 1 |
| `VOICE_PATCH_DCSG`(0x42) | DCSG | `CDCSG` | 1 |
| `VOICE_PATCH_SAA`(0x43) | SAA | `CSAA1099` | 1 |
| `VOICE_PATCH_SCC`(0x48) | SCC, SCCP | `CSCC` | 1 |
| `VOICE_PATCH_ADPCMB_Y8950`(0x50) | Y8950(ADPCM部) | `CYmDelta` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_ADPCMB`(0x51) | ADPCMB, **ADPCMB_OPNA** | `CYmDelta` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_ADPCMA`(0x52) | ADPCMA | `CAdPcm2610A` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_PCMD8`(0x53) | PCMD8 | `CAdPcmZ280` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_AWM`(0x54) | AWM | `COPL4AWM` | HwPatch対象外(SampleZonePatch使用) |
| なし(`VOICE_PATCH_NONE`) | OPNA_RHY, OPLL_RHY 等リズムデバイス | `COPNARhythm` / `COPLLRhythm` | HwPatch対象外(内蔵リズム、ダミーHwPatch使用) |

太字は複数の`deviceType`が同じ`VoicePatchType`に統合されている箇所（同種デバイス
自動束ねやsub-device生成の都合で意図的に統合したもの）。

「オペレータ数」列は、`hwbank.schema.json`の`ops`配列の実際の要素数
(1〜4で可変)、および`PatchManager::hwPatchToJson()`がHwBank保存時に
書き出すべき要素数を判定するための、正式な参照情報として使う
(2026年7月〜)。`ops`が「HwPatch対象外」の行は、そもそもHwPatchでは
なくSampleZonePatch(またはダミーHwPatch)を使うため、この文脈での
オペレータ数の概念自体が適用されない。

### 設計原則: デバイスに特殊ルーティングが必要かどうかの判断基準

新しいデバイス種別を追加する際、`resolveTriple()`の通常経路
(`voicePatchType`→`HwBankRegistry`検索→`findDeviceIndexByVoicePatchType`)
にそのまま乗せられるか、`0x70`(ビルトインリズム)のような専用バイパス
や、`isSampleBasedVoicePatchType`のような個別分岐が必要になるかは、
「リズム専用かどうか」ではなく、**そのデバイスの音色データが、標準の
`HwPatch`という形状でソフトウェア側に実在するかどうか**で決まる
(2026年7月、`COPLRhythm`実装時の議論から)。

| デバイス | 音色データの実体 | 特殊扱いの要否 |
|---|---|---|
| `COPNARhythm`/`COPLLRhythm` | チップROM内蔵の固定音色(ソフトウェアが管理する音色データが実質存在しない) | 必要 (`VOICE_PATCH_NONE`+`0x70`専用バイパス、または`findDeviceIndexByDeviceType`) |
| AWM/ADPCM系 | ソフトウェア管理下にはあるが、キーゾーン+ベロシティレイヤー+波形インデックスという、`HwPatch`(FMオペレータ型)とは全く異なる形状 | 必要 (`isSampleBasedVoicePatchType`による`SampleZoneBankRegistry`への分岐) |
| `COPLRhythm` | 他のFMチップと全く同じ`HwPatch`形状(オペレータ数が1〜2と狭いだけ) | **不要** (`resolveTriple`は無改造、最初から本物の`VoicePatchType`を持つ) |

音色データが`HwPatch`の形状に収まるデバイスは、たとえチャンネル数や
用途が特殊(リズム専用等)であっても、`VOICE_PATCH_NONE`のような
「識別子なし」の扱いにする必要はなく、専用の`VoicePatchType`を与えて
「音色データがデバイスを選択する」という通常の原則にそのまま乗せる
方が、コードの見通しが良くなる。逆に、音色データの形状自体が
`HwPatch`と根本的に異なる場合(AWM等)や、ソフトウェア側に音色データが
実在しない場合(ROM固定音源)は、専用の分岐が構造的に避けられない。

---

## 6. 既知の制限・未実装機能

| 項目 | チップ | 状態 |
|---|---|---|
| OPZ 2系統LFOリソース | COPZ | 旧FITOMも未完成のため現状維持 |
| CAdPcmZ280の正式な旧FITOM比較 | CAdPcmZ280 | 旧FITOMにPCMD8.cppは存在するが、部分的にしか突き合わせていない |

VoicePatchType完全一致以外へのフォールバックは実装済み
(`DeviceFactory::acceptsFallback`、各チップの`*AcceptsFallback`関数群)。
旧FITOMの単純な互換リスト方式とは異なり、パッチの実際の内容
(`hwOp[].WS`等、実際にそのチップ固有の拡張機能を使っているか)を見て
可否を判定する、内容駆動型の設計になっている。

**推奨されるバンク設計方針**: 同一チップファミリー内では、パッチの
`voicePatchType`は常に「そのファミリーの最上位(最も機能が豊富な)チップ」
として宣言することを推奨する(例: OPN系は常に`VOICE_PATCH_OPN2`、OPL系は
常に`VOICE_PATCH_OPL3_2`、OPZ系は常に`VOICE_PATCH_OPZ2`)。`acceptsFallback`
は「上位型で宣言されたパッチが、実際にはそのファミリー内の下位チップの
機能だけで表現できる内容か」を判定し、可能なら自動的に下位チップへ
フォールバックする設計になっているため、この運用に従えば:

- 下位チップしか無い環境でも(パッチが実際に上位専用機能を使っていない
  限り)正しく発音できる
- 同じパッチバンクファイルを、下位/上位どちらの環境でも共有できる
  (「下位チップ用バンク」「上位チップ用バンク」を重複して用意する必要がない)
- パッチ作成者は、実際に上位専用機能(非サイン波WS等)を使う場合にのみ、
  その効果が失われることを許容している(=下位環境では鳴らない)と
  自覚的に判断すればよい

2026年7月時点で全チップファミリーの`acceptsFallback`実装を監査し、
OPL系(`coplAcceptsFallback`)が`VOICE_PATCH_OPL3_2`方向を扱っていない
欠落を発見・修正済み。他のファミリー(OPN/OPN2、OPM/OPZ/OPZ2、SSG/EPSG、
OPLLファミリー)は監査の結果、既に全方向を正しくカバーしていることを
確認した。
