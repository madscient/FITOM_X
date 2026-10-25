# チップドライバの共通設計

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**:
- チップファミリーごとの実装の注意とレジスタ: `docs/design/chip-notes.md`
- 音色データと`VoiceProcessor`: `docs/design/voice-processor.md`
- パッチの解決: `docs/design/patch-resolution.md`
- 利用者向けの音色パラメータ: `docs/manuals/hwpatch-reference.md`

この文書は、すべてのチップドライバに共通する仕組み(継承構造、サブデバイスの自動生成、同種デバイスの束ね、ステレオ化、VoicePatchType)を書く。

---

## クラス階層

```
ISoundDevice (抽象インターフェース)
  ├── CSoundDevice          単体チップの共通実装 (レジスタキャッシュ・
  │                         ChState管理・VoiceProcessor連携等)
  │     └── 各チップドライバ (docs/design/chip-notes.md)
  │
  └── CMultiDevice           複数 ISoundDevice を束ねる共通基盤
        ├── CSpanDevice      グローバルch→実チップ+ローカルchに変換して委譲
        │                    (サブデバイスの自動生成・同種デバイスの束ねで使う)
        └── CUnison          全チップに同一ch番号をブロードキャスト
              └── CLinearPanDevice  L/Rに配線した同一チップ2台をステレオ化
```

`CMultiDevice`/`CSpanDevice`/`CUnison`/`CLinearPanDevice`は`MultiDevice.h`にヘッダーとして実装されており、
チップドライバ側（`OPN2_new.cpp`/`OPL_new.cpp`等）から`#include`して直接継承できる。

## CSoundDevice の共通の約束

### Fnumberテーブルとnoteoffset

`CSoundDevice`のコンストラクタは`FnumRegistry::instance().getTable(fnumType,
master, divide, noteOffset_)`で`fnumTable_`を生成する際、`noteOffset_`を
テーブル生成式`(offset+i)/768`に**既に焼き込んでいる**。したがって基底クラス
`CSoundDevice::getFnumber()`(および同じ`fnumTable_`を参照する`CAdPcmBase`系の
`getFnumber()`)が実行時に`s.lastNote*64`へさらに`noteOffset_`由来の項を
加算してはならない(二重に適用され、blockが数オクターブ分ずれる)。
`COPM::getFnumber()`のように`fnumTable_`を使わず独自テーブル
(KeyCode等)で計算するチップドライバは、実行時に`noteOffset_`相当の値を
加算する設計で問題ない(ただしその場合は`* 64`であり`* 64 / 12`のような
半音→セント変換の再スケーリングは不要。`noteOffset_`は既にテーブル
インデックス単位＝kfs単位で渡されているため)。

`getFnumber()`は11bit精度の値を返す。チップのレジスタ幅がそれより狭いチップ(OPLLの9bit等)は、ドライバが右シフトして合わせる。

### レジスタの書き込みとシャドウ

`CSoundDevice::setReg(reg, data, forceWrite=false)`は、自チップのレジスタシャドウ(`regBak_`)と同じ値なら実際の書き込みを省く。チップやエミュレータ側の内部状態がシャドウとずれうる場合や、書くこと自体に意味があるレジスタ(キーオンのようなエッジ検出、書くと自動で0に戻るトリガー)は、`forceWrite=true`で書く。

- F-numberを複数のレジスタに分けて書くチップで、片方の値が前回と同じだと書き込みが省かれ、音程が更新されないことがある。OPNは`forceWrite=true`で書いている(`docs/design/chip-notes.md`の「F-numberの書き込み」)。他のチップで同じ症状が出たら、同じ対処を個別に当てて確かめる
- `init()`で拡張レジスタを初期化するとき、シャドウの初期値(0)と同じ値は書き込みが省かれる。リセット値が仕様に無いレジスタは、`forceWrite`で書くかシャドウとチップの両方へ揃える

### チャンネル数と配列の大きさ

`CSoundDevice::chState_`は`std::vector<ChState>`で、コンストラクタで`maxChs_`とちょうど同じ数だけ確保する。チャンネルごとの状態を持つ配列は、全チップ共通の固定長の定数で大きさを決めず、そのチップのチャンネル数(`maxChs_`や束ねたチップの`getChCount()`)から決めること。固定長にすると、その長さより多いチャンネルを持つチップ(OPL4 AWMは24ch)で範囲外アクセスになる。`CPSGBase::lfoTL_`/`envelopes_`、`CLinearPanDevice::masterVolume_`/`masterPan_`も同じ方針でチャンネル数から大きさを決めている。

### チャンネルの割り当て

- **`queryCh(owner, patch, mode)`をオーバーライドするドライバは、`mode`をそのまま基底(`CSoundDevice::queryCh`)へ渡すこと。** `mode`は1=奪取なし/0=奪取ありを表し、`CSpanDevice::allocCh()`はmode=1→0の順で全サブチップを試すことで「空きのあるチップを優先し、全チップが本当に埋まって初めて奪取に回る」設計になっている。`mode`を無視して常に奪取すると、束ねた1台目だけが使われ続ける
- **リリース中の再トリガー対策**: OPN/OPM/OPL/OPL3のドライバは、`updateKey()`でキーオン時に`ChState::wasReleasing`が立っていれば、キャリアのRRを一時的に最大にしてからキーオンする。`wasReleasing`の決まり方は`docs/design/midi-processing.md`の「ノートオフとチャンネルの再利用の規則」
- **`forceDamp(ch)`**(CC#120)の既定の実装は`noteOff(ch)`だけ。急速に減衰させたいドライバはオーバーライドする。`CSpanDevice`/`CUnison`は各チップへ委譲する

### パンポットの向き

`ChState::panpot`は-64〜+63で、**負値=L / 正値=R は全ドライバ共通の規約**。L/Rの出力ビットを持つチップの`updatePanpot()`はこの向きで書く(OPM: `0x20` bit7/6、OPL3: `0xC0` bit4=CHA=L / bit5=CHB=R、OPN2/OPNA: `0xB4` bit7/6)。

OPL3のCHA/CHBは、どちらを左右へ配線するかがチップの外で決まる。CHA=Lとしているのは、ymfmが出力Aを先頭に出し、YMFMEngineがそれを左へ送るからである。実機のボードがこの向きでない場合、パンが左右逆になる。

### 公開しているチップ寄りのメソッド

本来チップ固有だが、`ISoundDevice`に公開しているメソッド。

| メソッド | 理由 |
|---|---|
| `updateTL(ch, op, lev)` | ソフトウェアLFO(トレモロ)の変調結果を`CSoundDevice::timerCallback()`から反映するため |
| `getChState(ch)` | `VoiceProcessor::onNoteOn`等が`CSpanDevice`経由でも正しく実チップの状態を取得できるようにするため |
| `setCC1Modulation(...)` | 同上 |
| `forceDamp(ch)` | CC#120の急速減衰をチップごとに実装するため |

---

## サブデバイスの自動生成

**1つの物理/エミュレーターチップ指定から、内部的に複数の`ISoundDevice`インスタンスを
自動生成する仕組み。** 実機の1チップが複数の独立した音源機能（FM本体・SSG・ADPCM・
内蔵リズム等）を持つ場合に対応する。`Config::resolveCompositeSpec()`が展開ルールを持ち、
`Config::pushDeviceEntries()`が実際に複数の`DeviceEntry`を生成する。

```cpp
struct SubDeviceSpec {
    uint32_t    deviceType;    // このサブデバイスの DEVICE_*
    const char* labelSuffix;   // ラベルに付与する接尾辞 (例: "-SSG")
    bool        usesExtraPort; // 2ポート目 (extraPort) を必要とするか
    bool        rhythmCapable; // rhythm_mode をこのサブデバイスに適用するか
    int         clockDivider = 1;    // 親チップのクロックからの換算比
    int         clockMultiplier = 1; // (×clockMultiplier÷clockDivider)
};
```

### 展開ルール

| baseDeviceType | 展開されるサブデバイス |
|---|---|
| `DEVICE_OPNA` / `DEVICE_F286` / `DEVICE_OPN3` | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMB_OPNA`(port2側) + `DEVICE_OPNA_RHY`(6パート) |
| `DEVICE_OPNB` (OPNB無印、YM2610) | FM本体(`COPNB`、実効4ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch、port2側) + `DEVICE_ADPCMB`(1ch、port1側) |
| `DEVICE_2610B` (OPNBB、YM2610B) | FM本体(6ch) + `DEVICE_SSG`(3ch) + `DEVICE_ADPCMA`(6ch、port2側) + `DEVICE_ADPCMB`(1ch、port1側) |
| `DEVICE_OPL` | FM本体(9ch) + `DEVICE_OPL_RHY`(5パート、そのインスタンスの`rhythm_mode:true`時のみ) |
| `DEVICE_Y8950` | FM本体(9ch) + `DEVICE_ADPCMB_Y8950`(1ch、内蔵ADPCM-B、常に生成) + `DEVICE_OPL_RHY`(5パート、そのインスタンスの`rhythm_mode:true`時のみ) |
| `DEVICE_OPL2` | FM本体(9ch) + `DEVICE_OPL_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPL3` / `DEVICE_OPN3_L3` | `DEVICE_OPL3`(4OPモード,6ch) + `DEVICE_OPL3_2`(2OP残余,6ch) + `DEVICE_OPL_RHY`(5パート、`rhythm_mode:true`時のみ。COPL3_2側port1サブチップのch6-8を専有) |
| `DEVICE_OPL4` | `DEVICE_OPL3`(4OPモード,6ch) + `DEVICE_OPL3_2`(2OP残余,6ch) + `DEVICE_OPL4AWM`(PCM部,24ch、高位バンク[アドレス0x200以降]側) + `DEVICE_OPL_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPLL` / `OPLL2` / `OPLLP` / `OPLLX` | FM本体(9ch) + `DEVICE_OPLL_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPLLEX` (Y8960) | FM本体(9ch) + `DEVICE_OPLLEX_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_OPL2EX` (Y8960) | FM本体(9ch) + `DEVICE_ADPCMB_OPL2EX`(1ch、常に生成) + `DEVICE_OPL2EX_RHY`(5パート、`rhythm_mode:true`時のみ) |
| `DEVICE_DSG` (YM2163) | 楽音部(`CDSG`、4ch) + `DEVICE_DSG_RHY`(5パート、**常に生成**) |
| `DEVICE_SSGS` (YMZ705) / `DEVICE_SSGS2` (YMZ732) | SSG互換部(`CSSGS`、YM2149相当×2 = 6ch) + `DEVICE_SSGS_ADPCM`(8ch)。両者は同一の8bitレジスタ空間($00-$3F / $40-$B3)を共有するためport2・オフセットとも不要。ADPCM部のdeviceTypeはSSGS/SSGS2で共有する |

上記以外（単体`COPN`、`COPM`系、`CSSG`単体、`DEVICE_SSGS3`等）は展開されず、1エントリ=1デバイスのまま。
チップごとの事情は`docs/design/chip-notes.md`の各節。

`DEVICE_OPNB`と`DEVICE_2610B`のサブデバイス構成(SSG/ADPCM-A/ADPCM-B)は同一で、違いはFMチャンネル数(無印=実効4ch、B=6ch)だけ。

### クロック

**チップドライバは、音程・再生レートをポートが報告するチップのクロック(`IPort::getClock()`、HWプラグインの`HWPlugin_GetClock`)から計算する。** エミュレーターでは、プラグイン設定に書いたクロックでチップが動くため、FITOM_X側で別の値を仮定すると、その比の分だけ音程がずれる。`DeviceFactory::create()`の`sampleRate`引数は音声出力のサンプルレートで、チップのクロックではない。

- ポートが0を返す(クロックを報告しない)とき、FM系のドライバはチップの標準クロック(YM2203=3.9936MHz、YM2608/YM2612/YM2610=8MHz、YM2151/YM2414/YM3526/YM3812/YM2413=3.579545MHz)で計算する。PSG系とADPCM系は`sampleRate`で代用する
- サブデバイスのドライバへは、`SubDeviceSpec::clockDivider`/`clockMultiplier`でチップのクロックを換算して渡す(`DeviceEntry`経由で`DeviceFactory::create()`へ届く)。換算が要るのは次のもの

| サブデバイス | 換算 | 理由 |
|---|---|---|
| OPNA/OPNB/OPNBBの内蔵SSG | ÷4 | 実機仕様上φM/4で動作する |
| OPL3の4OP/2OP/リズム | ÷4 | OPL系のドライバはYM3812と同じ「72クロックで1サンプル」の換算で計算する。YMF262は288クロックで1サンプル |
| OPL4の4OP/2OP/リズム | ×2÷19 | YMF278BのFM部は684(19×36)クロックで1サンプル(ymfmの`ymf278b`による)。72÷684=2÷19 |

FM部の1サンプルあたりのクロック数はチップごとに違い、ドライバの`fnumDivide`に持たせる。YM2203は72、YM2608/YM2612/YM2610は144で、同じF-Numberでもクロックが同じならYM2203の方が1オクターブ高く鳴る。`COPN`は単体(YM2203)なら72、`COPNA`のサブチップなら144で作る。

### 内蔵リズムのサブデバイスと rhythm_mode

- **OPL系/OPLL系の内蔵リズム(`DEVICE_OPL_RHY`/`DEVICE_OPLL_RHY`/`DEVICE_OPLLEX_RHY`/`DEVICE_OPL2EX_RHY`)は、そのチップインスタンスの`rhythm_mode`が`true`のときだけ生成する。** 内蔵リズムはFM本体のch6-8とレジスタを共有し、`rhythm_mode`のときFM本体側のch6-8を無効にする。内蔵リズムと通常の楽音chをまたいだDVA(動的ボイス割当)は無いので、両方を有効にするとレジスタを奪い合う。`resolveCompositeSpec()`の第2引数に、そのインスタンス自身の`rhythm_mode`の値を渡す
- OPL3/OPL4はch6-8が`COPL3_2`側のport1サブチップにあるため、`rhythm_mode`時はport1のch6-8だけを無効にし、port2側の3chは通常の2OPとして使える
- **OPNA系の`DEVICE_OPNA_RHY`とDSGの`DEVICE_DSG_RHY`は常に生成する。** レジスタ空間がFM本体・楽音部と完全に独立していて、共有もDVAの調整も要らないため(`resolveCompositeSpec()`の第2引数は無視される)
- **ADPCM-B(Y8950・OPL2EX・OPNA)も常に生成する。** レジスタ空間がFM本体のオペレータレジスタ・リズム用ch6-8と独立しているため

### 高位バンクのポート

各サブデバイスは同一の物理ポート(`e.port`)を共有する。`usesExtraPort=true`のサブデバイスは`e.port2`(2ポート目)も受け取る。

チップ上でFM本体と別のアドレス範囲に置かれたサブデバイスには、`CFITOM::resolveHighBankPort()`がオフセット付きのポート(`OffsetPort`)を割り当てる。オフセット量は`CFITOM::getHighBankOffset(deviceType)`が持つ(ADPCM-A/ADPCM-B[OPNA]は0x100、OPL4 AWMは0x200、それ以外は0)。OPL4のAWM部は、FM部が使う2バンク(port1=0x000-0x0FF、port2=0x100-0x1FF)とは別の3つ目のバンク(0x200以降)にあるため、2ポートのモデルでは表せず、このオフセットポートを使う(`usesExtraPort=false`)。

### レジスタダンプの表示範囲

GUIのレジスタダンプモニター(`RegisterDumpWindow`)は、物理ポート単位で表示サイズ(`dumpSize`)を決める。`CFITOM::buildPhysicalChipList()`は、同一物理ポートを共有する全サブデバイスについて「`getHighBankOffset()`+`getDeviceRegSize()`」を計算し、その最大値を採用する(最初に登録されたものだけでは足りない。OPL4は`DEVICE_OPL3`[0x000-0x1FF]が先に登録されるが、後から登録される`DEVICE_OPL4AWM`[0x200-0x2FF]の方が高位)。

`getDeviceRegSize()`が返すレジスタ空間サイズは`CFITOM.cpp`の`kRegSizeMap`に持つ。値はチップドライバが実際に書き込む最終レジスタアドレスを16byte境界へ切り上げたもので(例: OPLLの有効レジスタは0x38までなので0x40、OPNAは後半3chのport2側0x1B6までなので0x1C0)、1ポート分(0x100)より小さいチップはそのまま小さい範囲を表示する。分類の単位は`DeviceFactory::create()`のswitch(=どのチップドライバが担当するか)に一致させること。`kRegSizeMap`に未登録の`deviceType`しか乗っていない物理チップのみ、フォールバックとして0x100を出す。

`RegisterDumpWindow.cpp`側はダンプの実バイト数から表示アドレス範囲・行数を動的に組み立てるので、`dumpSize`さえ正しければGUI側の変更は要らない(ただし16列固定グリッドのため、`dumpSize`は必ず16の倍数であること)。

レジスタのアドレスをモードのビットで多重化するチップ(AY8930の拡張モード等)は、ドライバが内部シャドウでバンクを分けて持ち、`ISoundDevice::getBankedRegisterView()`でレジスタダンプへ渡す(`docs/design/chip-notes.md`の「PSG系」)。

---

## 同種デバイスの束ね

**プロファイルに同一`VoicePatchType`・同一物理接続種別・同一パン設定のデバイスが
複数存在する場合、`CSpanDevice`で1つの論理デバイスに自動統合する。** 判定基準は
`deviceType`の一致ではなく**`VoicePatchType`の一致**で、たとえば`DEVICE_OPNA`と
`DEVICE_2610B`のFM部分はどちらも`VOICE_PATCH_OPN2`なので束ねられる。

`Config::mergeSpannableDevices()`が`buildDevice()`完了後（**サブデバイスの展開の後**）に
1回実行され、統合されたエントリは`devices_[]`から削除される。実際のマルチチップ
`ISoundDevice`生成は`CFITOM::initDevices()`側が担当し、束ね候補ポートそれぞれに
`DeviceFactory::create()`を呼んだ上で`CSpanDevice`にラップする。

```
グループ化キー: (VoicePatchType, IPort::getInterfaceDesc(), IPort::getPanpot())
```

- **サブチップごとの`deviceType`/`rhythm_mode`/クロックの換算比は独立している**: 束ねられる複数のデバイスエントリは、`deviceType`(実装クラス。例: `DEVICE_OPNB`はch0/ch3を無効にする)や`rhythm_mode`(OPL/OPLL系のch6-8を内蔵リズム専用にする)、クロックの換算比(OPL3のFM部は÷4、OPL4のFM部は×2÷19で、どちらも`DEVICE_OPL3`/`DEVICE_OPL3_2`)がそれぞれ異なりうる。そのため`PortGroup`(`Config.h`)はこれらをサブチップごと(ステレオペアならR側の換算比も)に保持し、`CFITOM::initDevices()`のサブチップ生成ループは代表の値ではなく`config_->getDeviceSpanGroupDeviceType(i,k)`/`getDeviceSpanGroupRhythmMode(i,k)`/`getDeviceSpanGroupClockDivider(i,k)`等を使う
- **`allocCh()`のチップ試行順序はローテーションする**: `CSpanDevice::allocCh()`は、mode=1(奪取なし)→0(奪取あり)を試す前段階で、どのチップから先に空きchを探すかを`nextChipIdx_`(割り当て成功のたびに1つ進める)で毎回ずらす。常に`chips_[0]`から試すと、1台目に`Releasing`(ソフトウェア上は解放済みだが実際にはまだ減衰中)のchが1つでもある限り1台目だけが選ばれ続け、同じ物理chの再利用が早まって、まだ減衰中のノートを切ってしまう。各チップ固有の`queryCh`の制約(OPMのノイズはch7固定等)には手を加えず、チップの試行順序だけを変える
- **束ねたADPCM-Bのバンクは、サブチップごとの`deviceType`で解決する**: OPNA/OPNB/OPNBB/Y8950のADPCM-Bは同一VoicePatchType(`VOICE_PATCH_ADPCMB`)なので1つの`CSpanDevice`へ束ねられるが、波形の配置(オフセットテーブル)はチップごとに違う。`CMultiDevice::setPcmRegistry()`は、各サブチップについてそのサブチップ自身の`getDeviceType()`で`findBankNoForDeviceType()`を引き直し、見つかればそれを使う(見つからなければ代表デバイス基準の`bankNo`を使う)

---

## リニアステレオ化

**物理的にL/Rへ固定配線された同一チップ2台を、1つのステレオデバイスとして
束ねる機構**。OPLL/OPL/PSG系のようにチップ自体が
モノラル出力しか持たない音源で、2台を左右に振り分けてパンポットを表現する
ための構成を想定している。`CLinearPanDevice`(`MultiDevice.h`)は`CUnison`
(全チップへ同一chで同時発音)を基底とし、`setVolume`/`setPanpot`だけを
オーバーライドして、パンポットに応じた等パワーパンニング(`lgain=cos`,
`rgain=sin`)でL側チップとR側チップの音量をクロスフェードする。両チップは
常に同時に鳴り続け、「左右どちらか一方だけを鳴らす」切り替えは行わない。

**自動検出はしない。** プロファイルの`devices[]`エントリ**両方**に
`stereo_pair`を明示指定した場合にのみ発動する
(`config_schema/profile.schema.json`)。`Config::mergeStereoPairDevices()`が
`buildDevice()`完了後・`mergeSpannableDevices()`より**前**に1回実行され、
R側エントリは`devices_[]`から削除された上で、L側エントリの`stereoPairPort`に
統合される。実際の`ISoundDevice`生成は`CFITOM::createLeveledDevice()`が担当し、
L/R各ポートに`DeviceFactory::create()`を呼んだ上で`CLinearPanDevice`にラップする。

```
ペアリング条件: (deviceType, IPort::getInterfaceDesc()) が一致し、
                L側エントリとR側エントリの組になっていること
```

判定基準が`CSpanDevice`側(`VoicePatchType`)より厳しい**`deviceType`一致**なのは、`CLinearPanDevice`が「物理的に同一のチップ2台」を前提とする構成だからである。`VoicePatchType`基準にすると、専用の`VoicePatchType`を持たない内蔵リズムのサブデバイス(`COPLLRhythm`/`COPNARhythm`は`VOICE_PATCH_NONE`)が常に対象外となり、R側だけが孤立して残る。

### 左右をどこで分離するか

**L/R役割の宣言方法は2通りあり、それが同時に「左右をどこで分離するか」も
決める**(`StereoSide`)。

| 指定 | L/R役割の判定 | 左右の分離を行う場所 | プラグイン側の`pan` |
|---|---|---|---|
| `"stereo_pair": true` | `IPort::getPanpot()`の1(L)/2(R) | **プラグインの出力ルーティング** | `1`(L) / `2`(R) が必須 |
| `"stereo_pair": "L"` / `"R"` | この値で明示 | **チップ自身のL/R出力ビット** | `0`(Stereo)のままでよい |

後者(チップ内L/R分離方式)では、`CLinearPanDevice`が束ねた2チップ自身の
`ChState::panpot`を左右端(-64 / +63)へ固定し、各ドライバの`updatePanpot()`が
チップのL/R出力ビットを書く(「パンポットの向き」節の規約)。
対応の可否は`FITOMConfig::deviceHasChipLevelPanpot()`が`deviceType`側の索引
として持つ(真の情報源は各ドライバの`updatePanpot()`実装であり、
`updatePanpot()`を実装・変更したら必ずこの一覧も見直すこと)。非対応チップに
`"L"`/`"R"`を指定した場合は読み込み時に警告を出す(モノラルチップでは
`updatePanpot()`が no-op のため、黙って左右に分離されないという静かな失敗に
なるのを防ぐ)。

対応チップ: **OPM/OPP・OPZ/OPZ2・OPL3/OPL3_2(=OPL4のFM部)・OPN2/OPNA/OPNB/
OPNBB系とそのADPCM-A/ADPCM-Bサブデバイス**。OPLL系・OPL/OPL2・SSG/PSG系・
YM2203は非対応(モノラル出力)。

なお片側だけ`"L"`/`"R"`・もう片側は`true`という混在指定の場合は、プラグイン側の
ルーティングが絡んで意図が確定しないため、従来方式(プラグイン側で分離)に倒す。

この方式の利点は、**プラグイン側の`pan`を`0`(Stereo)に保てる**こと。同一物理
チップ上の他のサブデバイス(OPL4のAWM部等)が、自前のハードウェアパンポットを
潰されずにフル活用できる。

制約として、OPNAのSSG部のように**チップ内にL/R出力の分離手段が無いサブ
デバイス**は、この方式でも左右に分かれない(音量クロスフェードだけが効くため、
パンを振っても定位ではなく音量だけが変化する)。これは実機の制約なので、
該当サブデバイスは`INFO`でその旨を記録するに留め、**ペア内の全サブデバイスが
非対応だった場合にのみ`WARN`**を出す(指定が完全に無効なケース)。

### サブデバイスを持つチップのステレオ化

**サブデバイスの自動生成で展開されたチップにも対応する。** 展開されたエントリ群は
同一の物理ポートを共有するため、**サブデバイス単位ではなく`compositeGroup`
単位**でペアリングする。L側グループとR側グループの
**「`stereo_pair`対象サブデバイスの部分集合」**が同数で、かつ`deviceType`が
1対1で対応できる場合に限り、対応するサブデバイス同士をまとめてステレオ化
する(サブデバイスごとに独立してマッチさせると、別チップのグループと
サブデバイス単位で混線した組み合わせになりうるため、グループ単位で判定して
から確定する)。

**ハードウェアパンポットを持つサブデバイスは対象外**:
`FITOMConfig::subDeviceAcceptsStereoPair()`が`false`を返すサブデバイスには
`stereo_pair`を伝播しない。対象は**OPL4のAWM/PCM部
(`DEVICE_OPL4AWM`、YMF278B)**、SSGS系(YMZ705/YMZ732/YMZ771)のSSG部・ADPCM部、
Y8960のOPL2EX/OPLLEX(FM部・ADPCM部・内蔵リズム)。これらはチャンネルごとのパンポットを
ハードウェアで持っており、`CLinearPanDevice`の束ね(モノラル出力しか持たない
音源に対する代替手段)を適用すると、本来のハードウェアパンポットを潰した上で
2倍のチップを消費することになるため。

判定対象がグループ全体ではなく部分集合なのはこのためで、これにより
**「OPL4のFM部」と「OPL3」をL/Rペアとしてステレオ化する**用法が成立する
(OPL4の展開は`DEVICE_OPL3`+`DEVICE_OPL3_2`+`DEVICE_OPL4AWM`、
OPL3は`DEVICE_OPL3`+`DEVICE_OPL3_2`であり、AWM部を除いた部分集合の
`deviceType`が一致する)。このときAWM部は独立したモノラルデバイスとして
`devices_[]`に残る。

ペアの2チップは`deviceType`が同じでも、クロックの換算比が違うことがある
(OPL3のFM部は÷4、OPL4のFM部は×2÷19)。L側エントリはR側エントリの換算比を
`stereoPairClockDivider`/`stereoPairClockMultiplier`に写し、`CFITOM::createLeveledDevice()`は
R側のチップをこの値で作る。L側の値で作ると、R側の音程がクロックの比の分だけずれる。

### 動的ボイス割当とデチューン

**動的ボイス割当(DVA)**: `CLinearPanDevice`は`CUnison`を基底とするため、
`queryCh()`は代表チップ(`chips_[0]`)の判断を採用し、他チップは同じchが同じ
条件で使えるかだけを確認する。**この確認の基準は`mode`と一致させること**
(mode=1→`Empty`または`Releasing`、mode=0→`Disabled`でなければ可)。
`findBestCh()`はmode=1で`Releasing`を、mode=0では`Running`(強制奪取)を返す
設計なので、ここで一律に`isEmpty()`を要求すると代表チップが正当に返したchを
ほぼ必ず弾いてしまい、「全チップが完全に`Empty`」のときしか発音できなくなる。

**デチューンを適用しないこと**: `CUnison`はユニゾン/デチューン用途では
`noteOn()`時にチップ間へデチューンを与えるが、`CLinearPanDevice`は
「同じ音を同じ音程で左右に鳴らす」構成のため、コンストラクタで
デチューン量0を渡している。左右で音程がずれると定位ではなくビート(うなり)に
なる。

### チャンネルレベルメーターでの扱い

L/R分割表示(バー1本を左右half-widthの2本に分けて描く)の有無は、**ビューの単位に従って決まる**。

| ビュー | リニアステレオ化ペア | チップ自身がL/R出力を持つ場合 |
|---|---|---|
| 物理チップ単位 | L側・R側それぞれ**独立したモノラルバンド**(統合しない) | 左右分割表示 |
| 論理チップ単位 | 1つのステレオデバイスとして**左右分割表示** | 左右分割表示 |

物理ビューで統合しないのは、OPLLのようなモノラル出力チップを2台使う構成に
おいて「物理チップ1台＝モノラル」が事実だからで、右ペインのレジスタダンプ
一覧(L側・R側が「(stereo pair)」付きラベルで別項目)とも一対一で対応する。
ステレオなのは束ねた結果できる**論理デバイス**の方であり、その定位は
`ISoundDevice::getStereoGains()`(`CLinearPanDevice`が実装、
`applyLinearPan()`と同じ等パワーパンニングの係数を正規化して返す)から得る。

チップ自身がL/R出力を持つ場合(`FITOMConfig::getChipPanType()`が`Mono`以外を
返すチップ)は、物理・論理どちらのビューでも左右分割表示になる。こちらの定位
係数は`ChState::panpot`から求め、`ThreeWay`(L/Rイネーブルビットのみ)は各
ドライバの`updatePanpot()`と同じ閾値で、`Continuous`(OPL4 AWM・YMZ280B・
SAA1099・SSGS系・Y8960の拡張ブロック)は等パワーパンニングの式で算出する。

なお`stereo_pair:"L"/"R"`(チップ内L/R分離方式)では、各物理チップのパンポットが
左右端に固定されるため、物理ビューではL側チップの全バーが左半分だけ、R側
チップの全バーが右半分だけ点灯する(実際にハードパンされていることが見える)。

---

## VoicePatchType 対応表

`Config::deviceTypeToVoicePatchType()`が`deviceType`(DEVICE_*)→`VoicePatchType`
(VOICE_PATCH_*)を変換する。サブデバイスの自動生成・同種デバイスの束ねは全てこの値を
基準に動作する。パッチの解決での使われ方は`docs/design/patch-resolution.md`参照。

| VoicePatchType | 対応する deviceType | 生成クラス | オペレータ数 |
|---|---|---|---|
| `VOICE_PATCH_OPN`(0x10) | OPN, OPNC | `COPN` | 4 |
| `VOICE_PATCH_OPN2`(0x11) | OPN2, OPN2C, OPN2L, OPNA, OPN3L, OPNB, 2610B, F286, OPN3 | `COPNA` / `COPN2` / `COPNB` | 4 |
| `VOICE_PATCH_OPM`(0x19) | OPM, OPP | `COPM` / `COPP` | 4 |
| `VOICE_PATCH_OPZ`(0x1a) | OPZ | `COPZ` | 4 |
| `VOICE_PATCH_OPZ2`(0x1b) | OPZ2 | `COPZ`（共用） | 4 |
| `VOICE_PATCH_OPL`(0x20) | OPL, Y8950 | `COPL` | 2 |
| `VOICE_PATCH_OPL2`(0x21) | OPL2, OPL2EX | `COPL2` / `COPL2EX` | 2 |
| `VOICE_PATCH_OPL3_2`(0x22) | OPL3_2（OPL3の2opモード） | `COPL3_2` | 2 |
| `VOICE_PATCH_OPL_RHY`(0x23) | OPL_RHY（OPL系内蔵リズムチャンネル）, OPL2EX_RHY | `COPLRhythm` / `COPL2EXRhythm` | 1または2(楽器により異なる、混在可) |
| `VOICE_PATCH_OPLL`(0x28) | OPLL, OPLL2 | `COPLL` / `COPLL2` | 2 |
| `VOICE_PATCH_OPLLP`(0x29) | OPLLP | `COPLLP` | 2 |
| `VOICE_PATCH_OPLLX`(0x2a) | OPLLX | `COPLLX` | 2 |
| `VOICE_PATCH_VRC7`(0x2b) | VRC7 | `CVRC7` | 2 |
| `VOICE_PATCH_OPLLEX`(0x2c) | OPLLEX | `COPLLEX` | 2 |
| `VOICE_PATCH_OPL3`(0x30) | OPL3, OPN3_L3 | `COPL3`（4OPモード専用） | 4 |
| `VOICE_PATCH_SD1`(0x38) / `MA3`(0x39) / `MA5`(0x3a) / `MA7`(0x3b) | (ドライバ未実装) | - | 未定 |
| `VOICE_PATCH_SSG`(0x40) | SSG, PSG, SSGL, SSGLP, **SSGS**, **SSGS2**, **SSGS3**, **SSGEX** | `CSSG` / `CSSGS` / `CSSGS3` / `CSSGEX` | 1 |
| `VOICE_PATCH_EPSG`(0x41) | EPSG | `CEPSG` | 1 |
| `VOICE_PATCH_DCSG`(0x42) | DCSG | `CDCSG` | 1 |
| `VOICE_PATCH_SAA`(0x43) | SAA | `CSAA1099` | 1 |
| `VOICE_PATCH_DSG`(0x44) | DSG | `CDSG` | HwPatch対象外(ROM固定音色、暗黙のバンク) |
| `VOICE_PATCH_SCC`(0x48) | SCC, SCCP, SCCEX | `CSCC` | 1 |
| `VOICE_PATCH_ADPCMB`(0x51) | ADPCMB, **ADPCMB_OPNA**, **ADPCMB_Y8950**, **ADPCMB_OPL2EX** | `CYmDelta` / `CY8960Delta` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_ADPCMA`(0x52) | ADPCMA | `CAdPcm2610A` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_PCMD8`(0x53) | PCMD8 | `CAdPcmZ280` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_AWM`(0x54) | OPL4AWM | `COPL4AWM` | HwPatch対象外(SampleZonePatch使用) |
| `VOICE_PATCH_SSGS_ADPCM`(0x55) | SSGS_ADPCM (YMZ705/YMZ732共通) | `CSSGSAdPcm` | HwPatch対象外(SampleZonePatch使用) |
| なし(`VOICE_PATCH_NONE`) | OPNA_RHY, OPLL_RHY, OPLLEX_RHY, DSG_RHY 等の内蔵リズムデバイス | `COPNARhythm` / `COPLLRhythm` / `COPLLEXRhythm` / `CDSGRhythm` | HwPatch対象外(内蔵リズム、ダミーHwPatch使用) |

太字は複数の`deviceType`が同じ`VoicePatchType`に統合されている箇所（同種デバイスの束ねや
サブデバイスの生成の都合で意図的に統合したもの）。

「オペレータ数」列は、`hwbank.schema.json`の`ops`配列の実際の要素数
(1〜4で可変)、および`PatchManager::hwPatchToJson()`がHwBank保存時に
書き出すべき要素数を判定するための、正式な参照情報として使う。
「HwPatch対象外」の行は、そもそもHwPatchでは
なくSampleZonePatch(またはダミーHwPatch)を使うため、
オペレータ数の概念自体が適用されない。

CC#0の`0x70`(`VOICE_PATCH_BUILTIN_RHYTHM`)と`0x7F`(`VOICE_PATCH_SILENCE`)はデバイスに対応しないモード選択子で、この表には載らない(`docs/design/midi-processing.md`の「Bank Select 方式」)。

## 特殊ルーティングの判断基準

新しいデバイス種別を追加する際、`resolveTriple()`の通常経路
(`voicePatchType`→`HwBankRegistry`検索→`findDeviceIndexByVoicePatchType`)
にそのまま乗せられるか、`0x70`(ビルトインリズム)のような専用バイパス
や、`isSampleBasedVoicePatchType`のような個別分岐が必要になるかは、
「リズム専用かどうか」ではなく、**そのデバイスの音色データが、標準の
`HwPatch`という形状でソフトウェア側に実在するかどうか**で決まる。

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

## フォールバック

パッチの`VoicePatchType`と完全に一致するデバイスが無いとき、同じチップファミリーの別のチップで鳴らせるかを`DeviceFactory::acceptsFallback`(各チップの`*AcceptsFallback`関数群)が判定する。パッチの実際の内容(`hwOp[].WS`等、実際にそのチップ固有の拡張機能を使っているか)を見て可否を決める、内容駆動の設計。

| ファミリー | 判定 |
|---|---|
| OPN/OPN2、OPM/OPZ/OPZ2、SSG/EPSG | 下位のチップで表せる内容か(上位専用の機能を使っていないか)を見る |
| OPL/OPL2/OPL3_2 | OPL2は、OPLの音色を常に、OPL3_2の音色を全オペレータのWSが4未満のとき受け入れる。OPLは、OPL2/OPL3_2の音色を全オペレータのWSが0のとき受け入れる |
| OPLL/OPLLP/OPLLX/VRC7/OPLLEX | ユーザー音色(`ext.ALG_EXT`のbit0=0)なら相互に許可する。ROMプリセットは実機ごとにROMの中身が違うので許可しない |

この判定を前提に、バンクを書く人には「同じファミリーの中では、最も機能の多いチップの`VoicePatchType`で宣言する」ことを勧めている(利用者向けの説明は`docs/design/patch-resolution.md`)。
