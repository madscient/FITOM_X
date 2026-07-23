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

### Fnumberテーブルとnoteoffsetの二重適用に関する既知の注意点

`CSoundDevice`のコンストラクタは`FnumRegistry::instance().getTable(fnumType,
master, divide, noteOffset_)`で`fnumTable_`を生成する際、`noteOffset_`を
テーブル生成式`(offset+i)/768`に**既に焼き込んでいる**。したがって基底クラス
`CSoundDevice::getFnumber()`(および同じ`fnumTable_`を参照する`CAdPcmBase`系の
`getFnumber()`)が実行時に`s.lastNote*64`へさらに`noteOffset_`由来の項を
加算するのは二重適用であり、block(オクターブ)が数オクターブ分ズレるバグと
なる(2026年7月に発見・修正。ノート48(C3)入力でBlk=0固定になる不具合として
表面化した)。`COPM::getFnumber()`のように`fnumTable_`を使わず独自テーブル
(KeyCode等)で計算するチップドライバは、実行時に`noteOffset_`相当の値を
加算する設計で問題ない(ただしその場合は`* 64`であり`* 64 / 12`のような
半音→セント変換の再スケーリングは不要。`noteOffset_`は既にテーブル
インデックス単位＝100/64セント単位で渡されているため)。

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
同一の物理ポート(`e.port`)を共有するが、`usesExtraPort=true`のサブデバイスは
`e.port2`も受け取る(2ポート目=`extraPort`が必要なもの。`DEVICE_ADPCMB_OPNA`/
`DEVICE_ADPCMA`もここに該当。実チップ上これらのレジスタは`port2`側[アドレス
0x100以降]に配置されるため。`CFITOM::resolveAdpcmHighPort()`が実際のポート
差し替えを行う。2026年7月、ユーザー指摘で発覚: 以前はSSGと同じport1のまま
割り当てておりレジスタアドレスが衝突していた)。

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
| `DEVICE_OPNA` / `DEVICE_F286` / `DEVICE_OPN3` | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMB_OPNA`(port2側) + `DEVICE_OPNA_RHY`(6パート) |
| `DEVICE_OPNB` (OPNB無印、YM2610) | FM本体(`COPNB`、実効4ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch、port2側) + `DEVICE_ADPCMB`(1ch、port1側) |
| `DEVICE_2610B` (OPNBB、YM2610B) | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch、port2側) + `DEVICE_ADPCMB`(1ch、port1側) |
| `DEVICE_OPL3` / `DEVICE_OPN3_L3` | `DEVICE_OPL3`(4OPモード,6ch) + `DEVICE_OPL3_2`(2OP残余,6ch) |
| `DEVICE_OPLL` / `OPLL2` / `OPLLP` / `OPLLX` | FM本体(9ch) + `DEVICE_OPLL_RHY`(5パート、rhythm_mode時) |

上記以外（単体`COPN`、`COPM`系、`CSSG`単体等）は展開されず、1エントリ=1デバイスのまま。

`DEVICE_OPNB`と`DEVICE_2610B`のサブデバイス構成(SSG/ADPCM-A/ADPCM-B)は同一。
両者の違いはFMチャンネル数(無印=実効4ch、B=6ch)のみで、それ以外のケーパビリティ
(SSG・ADPCM-A・ADPCM-Bの搭載)は共通(2026年7月、ステージング環境からの指摘で
訂正: 以前は無印にADPCM-B用メモリ空間が無いという誤った前提でADPCM-Bサブ
デバイスを生成していなかった)。

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
- **EGT/RR動的書き換え技法**：OPL3と同じ技法（キーオン中はEGT=0(decay)に
  切り替えてSRをRR位置に書き込み、SRをリリースレイトとして機能させる。キーオフ時は
  EGT=1(sustained)に戻してRRレジスタ本来の値を書く）をユーザー音色
  （プリセット音色以外）に実装。プリセット音色はROMのためEG変更不可。
  （実機データシート(YM2413アプリケーションマニュアル「EGT (ENVELOPE TYPE):
  Select sustain/decay」節)上、EGT=1はsustained(KON中はSUSTAIN LEVELを保持)、
  EGT=0はdecay(KON中もRELEASE RATEが効き続ける)。2026年7月、YMF278(OPL4)の
  アプリケーションノートの記載を誤って根拠にEGTの0/1を逆転させる修正を
  一度行ったが、YM2413側の資料および波形図と整合しないことが判明し差し戻した。
  詳細は4.3節OPL系の同項目を参照）。
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
| `VOICE_PATCH_OPN`(0x10) | OPN, OPNC | `COPN` | 4 |
| `VOICE_PATCH_OPN2`(0x11) | OPN2, OPN2C, OPN2L, OPNA, OPN3L, OPNB, 2610B, F286, OPN3 | `COPNA` / `COPN2` / `COPNB` | 4 |
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

## 8. CSoundDevice::chState_ の固定長配列オーバーフロー (2026年7月修正)

**症状**: `OPL4`を含むプロファイル(`emu_opl.profile.json`等)を`fitom_gui`で
読み込むと、デバイス初期化完了直後に無言でクラッシュする(ログ出力なし、
例外ダイアログも出ない`0xc0000005`アクセス違反)。Releaseビルドでのみ
安定再現し、`RelWithDebInfo`ビルドでは再現しないなど、ビルド構成に
よって発生有無が変わる不定動作だった。

**根本原因**: `CSoundDevice`は各チャンネルの状態を`ChState
chState_[MAX_CHS]`という固定長配列で保持しており、`MAX_CHS`は全チップ
共通の定数(旧値16)だった。一方`OPL4`のAWM(PCM/波形メモリ)部ドライバ
`COPL4AWM`(`core/src/OPL4.cpp`)は`CSoundDevice(DEVICE_OPL4AWM, 24, ...)`
で**24ch**として構築される。コンストラクタは`maxChs_`に渡された値を
クランプせずそのまま格納するため、`onMasterPitchChanged()`等の
`for (ch=0; ch<maxChs_; ++ch)`ループが、実体16要素しかない
`chState_`配列を範囲外(16〜23番目)まで読み書きしてしまうヒープ
バッファオーバーフローだった。

**調査の経緯**: Windowsイベントログ(Application Error)から
`fitom_gui.exe`が複数ビルド・複数日にわたり同じ`0xc0000005`で繰り返し
落ちていたことを確認。Releaseと同一の最適化フラグ(`/O2`)を保ったまま
`/Zi /DEBUG`を追加して再ビルドし、`%LOCALAPPDATA%\CrashDumps`の自動
ダンプと`dbghelp.dll`(P/Invoke経由)でクラッシュアドレスをシンボル化した
結果、`fitom::CSoundDevice::onMasterPitchChanged`
(`SoundDevImpl.cpp`の`chState_[ch].isActive()`)を指していた。

**修正**: 応急処置として`MAX_CHS`を24に拡大する案もあったが、「今
判明している最大値に追従するだけ」で将来より多chなチップが追加されれば
同じ問題を再発する場当たり的な対処のため採用しなかった。代わりに
`chState_`を`std::vector<ChState>`化し、コンストラクタで`maxChs_`と
**ちょうど同じ数**だけ`resize`する設計に変更した(`MAX_CHS`定数自体を
廃止)。配列サイズと`maxChs_`が食い違うという不変条件違反が構造的に
起こり得なくなる。

**同種パターンの横展開**: `MAX_CHS`を根拠にサイズを決めていた箇所を
全面調査し、以下も同時に修正した(チップファミリー間の一貫性の原則に
基づく):

- `core/src/PSG_new.cpp`の`CPSGBase::lfoTL_`/`envelopes_`
  (ソフトLFO基準TL・ソフトウェアADSR) — 同じ`MAX_CHS`依存の固定長配列
  だったため、`chState_`と同様に`vector`化
- `core/include/fitom/MultiDevice.h`の`CLinearPanDevice::masterVolume_`/
  `masterPan_` — `CSoundDevice::MAX_CHS`とは別に独自定義していた
  `kMaxChs_=16`という決め打ちサイズだった。現状16ch超のチップが
  `CLinearPanDevice`で束ねられることはないが、潜在的に同種の脆弱性を
  抱えていたため、束ねる2チップの`getChCount()`に基づいて動的にサイズを
  決める`vector`に変更した
- `core/src/OPLL_new.cpp`の`COPLL`(ch6-8をリズム専用に無効化する
  ループ)は、固定長配列の余裕に暗黙に依存して`maxChs_`に対する境界
  チェックを一切していなかった(コメントに「maxChs=6の場合は既に
  disable済みのため範囲外アクセスにならない」と明記されていた)。
  現状VRC7(maxChs=6)は常にリズムモード無効で呼ばれるため実害はない
  防御的な修正だが、`i < maxChs_`のガードを追加した

**教訓**: 「チップ固有の値を、全チップ共通の固定長配列サイズより
小さく保つ」という暗黙の不変条件は、チップドライバの実装者に伝わり
にくく、実際に3箇所で同種の脆弱性が見つかった。可能な限り「サイズと
実際のチャンネル数を構造的に一致させる」設計(今回のvector化)を優先し、
共有定数への依存は避けること。
