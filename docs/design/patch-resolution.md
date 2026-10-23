# パッチの解決

**対象読者**: FITOM_X本体の開発者・AI  
**関連文書**:
- バンクファイルの書き方(利用者向け): `docs/manuals/bank-reference.md`
- CC#0の値の割り当て: `docs/design/midi-processing.md`の「Bank Select 方式」
- VoicePatchTypeの一覧・フォールバック・特殊ルーティングの判断基準: `docs/design/chip-drivers.md`
- 音色データの構成: `docs/design/voice-processor.md`

この文書は、MIDIで選ばれたバンク・プログラムから、発音するデバイスと音色データを決める処理(`PatchManager`)を書く。

---

## データ構造

```
PatchBank (*.patchbank.json)
  └── Patch [prog 0..127]
        ├── ToneLayer [0..3]          ← 発音レイヤー（最大4チップ）
        │   ├── voicePatchType        → VOICE_PATCH_*
        │   ├── hw_bank / hw_prog     → HwPatch(またはSampleZonePatch)への参照キー
        │   └── note_range / transpose / volume_offset / pan_offset
        └── poly
```

```
HwBank (*.hwbank.json)  ← (VoicePatchType, バンク番号) で登録
  └── HwPatch [prog 0..127]
        ├── hw / hwOp[4] / ext
        ├── builtin             ← ROM/ビルトイン音色への参照(メタバンクのみ)
        └── sw_bank / sw_prog   → SwPatch への参照キー (-1=参照なし)

SampleZoneBank (*.samplezonebank.json、または *.pcmbank.json から自動作成)
  └── SampleZonePatch [prog 0..127]
        └── zones[] (key/vel範囲, wave_index, root_note, sw_bank/sw_prog, AWMの校正値)

SwBank (*.swbank.json)  ← チップ族共通・1セット
  └── SwPatch [prog 0..127] (sw / swOp[4] / fine_transpose)

DrumBank (番号0固定。drum_banks[]の1ファイル = 1プログラム)
  └── DrumPatch [prog 0..127]
        ├── notes (DrumNote: voice_patch_type / patch_bank / patch_prog / play_note / sw_bank / sw_prog …)
        └── chokeGroups
```

**SwPatchの決まり方**: 発音に使うSwPatchは、次の順に決まる。DrumNote側の解決失敗は「無指定」と同じに扱い、HwPatch側へ進むのを妨げない。

1. DrumNoteの`sw_bank`/`sw_prog`が指定され、解決できればそれ
2. そうでなければ、HwPatch(またはSampleZonePatchのゾーン)自身の`sw_bank`/`sw_prog`
3. どちらも無ければSwPatch無し

演奏中はさらに、NRPN 96,2/96,3による上書きと、プライベートSysExのSwPatchオーバーライドが掛かる(`docs/design/midi-processing.md`)。

## VoicePatchType と deviceType

1台のデバイスは、2つの別々の分類軸を持つ。

| 分類軸 | 用途 | 値の例 |
|---|---|---|
| `deviceType` (`DEVICE_*`) | `DeviceFactory::create()` でどのチップドライバクラスを生成するか | `DEVICE_OPNA`=4 |
| `voicePatchType` (`VOICE_PATCH_*`) | どの音色データと互換性があるか | `VOICE_PATCH_OPN2`=0x11 |

`Config::getVoicePatchType(deviceIndex)`が`deviceType → voicePatchType`を変換する(`Config::deviceTypeToVoicePatchType()`、一覧は`docs/design/chip-drivers.md`の「VoicePatchType 対応表」)。`VOICE_PATCH_*`は、CC#0の直接モードのチップ選択にそのまま使うため、`0x01`-`0x6F`に収める。

`voicePatchTypeToVoiceGroup()`が返す`VoiceGroup`(OPNA/OPM/OPL2/OPL3/OPLL/PSG/PCM等の粗い分類)は、チップ族の括りを問う用途(フォールバック候補の探索、ログの表示)にだけ使い、バンクの検索キーには使わない。特にサンプル系はADPCM-B/ADPCM-A/PCM-D8/AWM/SSGS-ADPCMがすべて`VOICE_GROUP_PCM`になるので、`VoiceGroup`をキーにすると、チップ族の違うバンクどうしが番号を共有して上書きし合う。

パンに対応するかどうかは`VoicePatchType`の単位でも一律ではないので、FITOM_Xはパッチの解決でパン対応を判定しない。

## resolveTriple

`voicePatchType + hwBank + hwProg`の3つ組から、発音するデバイスと音色(HwPatchまたはSampleZonePatch)を得る処理は、`PatchManager::resolveTriple()`1か所にまとめてある。次の2つの経路が共通で使う。

- `resolve()`: `Patch`内の各`ToneLayer`が持つ`(voicePatchType, hwBank, hwProg)`
- `resolveDirect()`: 直接モード(CC#0=type, CC#32=hwBank, ProgChg=hwProg)とDrumNoteが持つ同じ3つ組

- **`voicePatchType == VOICE_PATCH_NONE`(0)は常に失敗として扱う。** 0はCC#0では「通常モード」(PatchBank参照)を意味する。`resolveTriple()`がPatchBank参照も扱えるようにすると、`ToneLayer`どうしが互いを参照する循環(Patch AのレイヤーがPatch Bを参照し、その逆も、という無限再帰)を開いてしまうので、入口で禁止している
- **`voicePatchType == VOICE_PATCH_SILENCE`(0x7F)は常に成功として扱い、`ResolvedTriple::silent`だけを立てて返す**(`docs/design/midi-processing.md`の「無音バンク」)
- **`voicePatchType == VOICE_PATCH_BUILTIN_RHYTHM`(0x70)**は内蔵リズム音源専用バンクへ分岐する(「内蔵リズム音源専用バンク」節)
- OPLL系で`hwBank==0`のときはROM音色へ分岐する(「OPLL系のROM音色」節)
- サンプル系の`voicePatchType`はSampleZoneBankへ分岐する(「サンプル系」節)
- それ以外は、HwBankから音色を引き、`Config::findDeviceIndexByVoicePatchType()`で完全に一致するデバイスを探す。無ければ`findFallbackDeviceIndex()`で、同じファミリーのデバイスのうち`acceptsFallback`がその音色を受け入れるものを探す(`docs/design/chip-drivers.md`の「フォールバック」)。PSG系だけは別の手順を取る(「PSG系共有バンク」節)

## 解決の3つのモード

### 通常モード（CC#0=0）

```
Program Change 受信
  → PatchManager::resolve(bankSelL_, prog, config)
      → PatchBank[bankSelL_].patches[prog] を取得  (CC#32=PatchBank番号)
      → 各 ToneLayer について resolveTriple(layer.voicePatchType, layer.hwBank, layer.hwProg)
          解決できなければそのレイヤーだけスキップ（Patch全体は無効にしない）
          VOICE_PATCH_SILENCEのレイヤーは警告なしでスキップ
```

### 直接モード（CC#0=0x01-0x6F）

```
bankSelM_ (CC#0) が 0x01-0x6F のとき:
  voicePatchType = bankSelM_        （CC#0の値そのものがVOICE_PATCH_*定数）
  hwBank         = bankSelL_        （CC#32の値）
  hwProg         = prog             （Program Changeの値）

PatchManager::resolveDirect(voicePatchType, hwBank, hwProg, config, storage)
  → PatchBank/ToneLayerを経由せず、Patch::fromSingleLayer()で単層Patchを構築
    （ノート範囲フル、transpose/offsetなし）
  → SwPatchは、解決されたHwPatch自身のsw_bank/sw_progから
```

### リズムチャンネル

`CRhythmCh`はCC#0(`0x79`によるメロディ復帰を除く)/CC#32を使わない。ドラムバンクは常に番号0を使い、Program Changeだけがドラムキットを選ぶ。

```
Program Change 受信 (CRhythmCh::progChange)
  → PatchManager::resolveDrum(0, prog)
      → DrumBankRegistry[0].patches[prog] (= DrumPatch) を取得
      → NoteOn受信時、DrumPatch::getNote(note) で DrumNote を取得
          → DrumNote.voicePatchTypeでモード判定 (CC#0と同じ意味。CRhythmCh::resolveNote):
              VOICE_PATCH_NONE(0): 通常モード。patchBank/patchProgをPatchBank番号/Progとして
                PatchManager::resolve()で解決 (マルチレイヤー)
              0x01-0x6F / 0x70 / 0x7F: patchBank/patchProgをhwBank/hwProgとして
                PatchManager::resolveDirect()で解決
          → DrumNote.playNoteを絶対ノート番号として発音
```

`"direct"`型のドラムキットは、ロード時(`PatchManager::loadDrumKitJson()`)に`[note_min, note_max]`の各ノートの`DrumNote`へ展開するので、`CRhythmCh`は`"routed"`型と同じ経路で動く。`play_note`は受信ノート番号そのまま。

`drum_banks[]`は、Program Change1つごとに独立したファイルを割り当てる方式で、1ファイルに全prog分を詰め込む方式は採らない(ファイルが肥大化するため)。

## HwBankの検索キー

`HwBank`は`(voicePatchType, バンク番号)`をキーに`HwBankRegistry`へ登録する。`voicePatchType`はバンク単位で1つで、プロファイルの`hw_banks[].group`の文字列から`FITOMConfig::stringToVoicePatchType()`で決まる(`Config::loadBanks()`→`PatchManager::loadHwBankJson()`)。バンクファイル自身はチップ種別を持たない。

バンク番号の名前空間はチップ種別ごとに独立している。`group`だけを変えて同じファイルを複数回登録すれば、同じ音色を複数のチップ種別から引ける。

PSG系だけは検索の前に`PatchManager::hwBankLookupVoicePatchType()`がキーを`VOICE_PATCH_SSG`へ寄せる(「PSG系共有バンク」節)。登録側は寄せないので、`group`に`"EPSG"`/`"DCSG"`/`"SAA"`/`"SCC"`を指定して登録したバンクは検索で見つからない(STATUS 4節)。

## OPLL系のROM音色

OPLL系チップ(OPLL/OPLLP/OPLLX/VRC7/OPLLEX。OPLL2は`VOICE_PATCH_OPLL`を共有する)のROMプリセット音色は、チップごとに実データが違い互換性が無いので、`acceptsFallback`はプリセット音色(`ext.ALG_EXT`のbit0=1)のフォールバックを拒否する(`opllFamilyAcceptsFallback`)。

ROM音色はチップに固定された既知のデータ(プリセットフラグ1bit + INSTナンバー4bit)なので、JSONのバンクを用意せず、**OPLL系の`voicePatchType`で`hwBank==0`のとき、HwBankRegistryを経由せずに`hwProg`から`HwPatch`を機械的に作る**(`PatchManager::resolveOpllRomVoice()`)。バンク0にJSONのバンクを登録しても、`resolveTriple()`が常にこちらを優先するので参照されない。

- `hwProg`の上位3bitがチップ種別(0=OPLL/1=OPLLX/2=OPLLP/3=VRC7)、下位4bitがROM音色の番号(0=無音、1-15)。実際のチップ種別は上位3bitで決まり、CC#0の値には依存しない
- 作る`HwPatch`は、`PatchManager`が持つ固定サイズのキャッシュ(`opllRomPatches_[4][16]`)を指す。各エントリは`ext.ALG_EXT`(bit2-1にROMバンクの値を焼き込み済み)・`hw.ALG`・`id`・`name`を持つ。音色名の出典は[plgDavid/misc wiki](https://github.com/plgDavid/misc/wiki/Copyright-free-OPLL(x)-ROM-patches)(耳コピによる非公式な近似データ。著作権フリーとして公開されているが、正確な公式名称ではない可能性がある。OPLLX/OPLLPは元資料に複数の情報が併記されているため、代表的な楽器名だけを採った)
- 要求されたチップが無ければ、OPLLEXが接続されていればそちらで鳴らす(OPLLEXは4種類のROMをすべて持ち、キャッシュの`HwPatch`に既にROMバンクの値が入っているので、デバイスを差し替えるだけでよい)。それ以外のチップへは代わりに鳴らさない
- **GUIのパッチピッカーでの列挙**: バンク0はHwBankRegistryを経由しないので、`PatchManager::getOpllRomPatches(voicePatchType)`(カテゴリ単位で16音色[添字0=無音]を返す)と`getOpllRomPatchByProg(hwProg)`(`resolveOpllRomVoice()`と同じ規則でhwProgから1件を返す、MIDIモニターのパッチ名表示用)を、`FITOMBridge::getHwBankList()`/`getHwBankPatches()`/`getChannelMonitors()`が「ROM Preset」として合成表示する。ピッカーが送るProgram Changeの値は、配列の添字ではなく`HwPatch::id`(`variantSel<<4 | instIndex`)をそのまま使うこと(添字を使うと、OPLL以外のカテゴリで`variantSel`がずれて別のチップの音色が鳴る)

## ROM/ビルトイン音色へのパフォーマンスパッチ紐づけ

OPLL系ROM音色(`opllRomPatches_`)とDSGのビルトイン音色(`dsgBuiltinPatches_`)は内部で機械的に作るので、`HwPatch::swBank`/`swProg`は常に-1のまま。これらにSwPatchを紐づけるために、パッチを書く人が明示的に指定する識別子(`BuiltinRef`)で紐づける専用のメタバンクを持つ(書き方は`docs/manuals/bank-reference.md`)。

- プロファイルの`hw_banks[]`で`role="builtin_swpatch_meta"`を指定したファイルは、通常のHwBankRegistryには登録せず、`PatchManager::builtinMetaBank_`という専用の保持スロットに入れる(`loadBuiltinMetaBankJson()`)。スロットは1つで、複数指定すると後勝ち
- `PatchManager::resolveOpllRomVoice()`は`(variantSel, instIndex)`、`resolveDsgBuiltinVoice()`は`(BUILTIN_TYPE_DSG, hwProg)`で、`HwBank::findByBuiltinRef()`を線形探索し、一致するエントリの`sw_bank`/`sw_prog`でSwPatchを解決する
- `BuiltinRef`の未設定の印は`0`ではなく`-1`(`isValid()`は`patchNo >= 0`を要求する)。DSGは`patch_no: 0`が正規の音色なので、ここを`>= 1`にするとそのエントリだけが黙って一致しなくなる
- `HwBank::patches`は128要素の配列1本で、`patch_type`ごとの名前空間ではないので、`prog`(格納位置)が重なると後勝ちで上書きされる。`loadBuiltinMetaBankJson()`は`prog`の重複・範囲外と、`builtin`を欠くエントリ(`findByBuiltinRef()`で永久に一致しない)をログに警告する
- `hwbank.schema.json`は、`prog`の必須/任意を`ops`/`builtin`の`oneOf`の分岐で切り替える。通常の音色(`ops`)の`prog`はProgram Change番号そのもので、`loadHwBankJson()`は`prog`の無いエントリを読み飛ばすので必須。`builtin`のエントリの`prog`は格納位置にすぎず、ローダーが自動採番するので任意(スキーマで一律必須にすると、エントリを生成する側に使いもしない`prog`を採番させ、かえって衝突を招く)
- OPL4 AWMは対象外。AWMのROM音色(YRW801)は通常のバンクファイル(`*.samplezonebank.json`)として扱っており、ゾーンが`sw_bank`/`sw_prog`を持てるため

## サンプル系

`voicePatchType`が`isSampleBasedVoicePatchType()`に当たる場合(ADPCM-B/ADPCM-A/PCM-D8/AWM/SSGS-ADPCM)、`resolveTriple()`はHwBankRegistryではなく、独立した`SampleZoneBankRegistry`を検索する。

```
resolveTriple(voicePatchType, hwBank, hwProg, ...)
  isSampleBasedVoicePatchType(voicePatchType) == true の場合:
    → SampleZoneBankRegistry::resolve(voicePatchType, hwBank, hwProg)
        で SampleZonePatch を取得 (HwBankRegistryは参照しない)
    → 見つからなければ失敗 (フォールバック判定はHwPatchの内容を見るが、
      SampleZonePatchにはその概念が無いため)
    → Config::findDeviceIndexByVoicePatchType(voicePatchType)でデバイスを解決
    → ResolvedTriple::samplePatch に結果を格納
```

`SampleZoneBankRegistry`は`(voicePatchType, bankNo)`をキーにする(`VoiceGroup`をキーにしない理由は「VoicePatchType と deviceType」節)。

**SampleZonePatchのSwPatch**: `HwPatch`のSwPatchはProgram Change時点(`resolveTriple()`)で解決できるが、`SampleZone`はノートとベロシティでどのゾーンを使うかが決まるので、NoteOn時点(`CInstCh::noteOn`/`CRhythmCh::applyNoteOn`)で`SampleZonePatch::resolveZone(note, vel)`により一致するゾーンを求めてから、そのゾーンの`swBank`/`swProg`を解決する。該当するゾーンが無ければ`zones[0]`を使う。

`CSoundDevice::assignCh()`の`samplePatch`分岐は、`usesVoiceProcessorForSamplePatch()`(既定false)がtrueのチップでだけ`VoiceProcessor::onNoteOn()`を呼ぶ。

| チップ | SwPatchの対応範囲 | 理由 |
|---|---|---|
| ADPCM-B / PCM-D8 | 全機能(チャンネルLFO/トレモロ/ベロシティ感度/fine_transpose) | `updateVolExp()`が`effectiveTL(0)`だけで音量を決める |
| ADPCM-A / SSGS-ADPCM | ベロシティ感度・トレモロのみ | `updateFreq()`がno-opで音程を変えられないため、チャンネルLFO/fine_transposeは効かない。ADPCM-Aの音量はチャンネルレジスタ(vel×exp)と全ch共通の総合音量レジスタ(vol)に分かれているので、`onNoteOn()`へは常にvol=127を渡し、チャンネルレジスタ側だけを`effectiveTL(0)`から計算する |
| AWM | 参照は解決するが音に反映しない | 実機がトレモロ・ビブラートをデバイスの機能として持ち(レジスタ`0x80-0x97`のLFO/VIBビット)、波形側にその設定が含まれる。チャンネルLFOの値をFnumberに足すだけの汎用の経路をつなぐと実機の仕組みと整合しないので、対象外にしている(対応するなら、実機のレジスタと波形側の設定を統合する設計が要る) |

**`pcm_banks[].group`**: 指定すると、
1. `CFITOM::initDevices()`が、そのVoicePatchTypeに一致するPCMデバイスへこのバンク番号を`setPcmRegistry()`で割り当てる(`PcmBankRegistry::findBankNoForVoicePatchType()`。`chip`の指定があれば`findBankNoForDeviceType()`を先に試す。`docs/design/chip-notes.md`の「ADPCM系」)
2. `PatchManager::loadPcmBankJson()`が、`entries[]`から`prog`=エントリ番号の単一ゾーンの`SampleZonePatch`を作り、`SampleZoneBankRegistry`へ登録する。`swpatches[]`があれば、そのゾーンの`swBank`/`swProg`に反映する。`offsets_only`のバンクでは作らない

`group`を省略したバンクは、波形データ(バイナリ+エントリのオフセット)の登録だけを行う。

## 内蔵リズム音源専用バンク（CC#0=0x70）

OPNA/OPLL/DSGの内蔵リズムユニット(`COPNARhythm`/`COPLLRhythm`/`CDSGRhythm`)は、`deviceTypeToVoicePatchType()`が`VOICE_PATCH_NONE`を返す特殊デバイスで、通常の`VoicePatchType`ベースの経路では到達できない。ROM固定の音で、ソフトウェア側に音色データが無いので、`HwPatch`の解決そのものが要らない。CC#0=`0x70`をこれらへの専用の経路にしている。「音色がデバイスを選択する」原則の例外で、音色データではなくチップの選択とチャンネル番号がデバイスを選ぶ。

```
CC#0/#32 → PatchManager::resolveTriple(0x70, chipSel, hwProg, ...)
  → PatchManager::resolveBuiltinRhythm(chipSel, hwProg, ...)
      chipSel(CC#32相当): 対象チップ。既存のVOICE_PATCH_*定数を再利用する
        (VOICE_PATCH_OPN2=OPNA、VOICE_PATCH_OPLL=OPLL、VOICE_PATCH_DSG=DSG)
        OPLLを指定してDEVICE_OPLL_RHYが無ければDEVICE_OPLLEX_RHYで鳴らす
      hwProg(ProgChg相当): そのチップの楽器(=デバイスチャンネル)番号
      → デバイスとチャンネル(ResolvedLayer::forcedCh)の両方を決める
```

**「楽器番号=物理チャンネル番号」というハードウェアの制約があるため、`hwProg`をそのままチャンネル番号として解釈する。** `resolveBuiltinRhythm()`がhwProgを検証して`ResolvedLayer::forcedCh`を決め、呼び出し元(`CRhythmCh::applyNoteOn`/`CInstCh::noteOn`)は`forcedCh>=0`なら`assignCh(forcedCh, ...)`でそのチャンネルに割り当てる。通常のProgram Changeの経路で楽器を選べるので、`CInstCh`の通常のチャンネルからも内蔵リズムの個別の楽器を鳴らせる。

| チップ | hwProgの範囲 | 対応楽器(参考) |
|---|---|---|
| COPNARhythm(OPNA) | 0-5 (6ch) | 実機レジスタ0x10のビット0-5に対応 |
| COPLLRhythm(OPLL) | 0-4 (5ch) | 0=HH, 1=CYM, 2=TOM, 3=SD, 4=BD |
| CDSGRhythm(DSG) | 0-4 (5パート) | 0=BD, 1=HC, 2=SDN, 3=HHO, 4=HHD |

範囲外のhwProgは解決自体を失敗させる(警告ログを出し、`ResolvedTriple::isValid()`がfalseになる)。**動的チャンネル割り当て(`allocCh`)へは逃がさない**——誤った楽器が鳴るより、鳴らない方が安全という判断。

**GUIのパッチピッカーでの列挙**: この経路もHwBankRegistry/SampleZoneBankRegistryを経由しないので、`FITOMBridge`(`getHwBankList()`/`getHwBankPatches()`/`getChannelMonitors()`)が、CC#0のカテゴリに`0x70`を、バンクの階層に対象チップを、プログラムの階層に各チップの内蔵リズムのパート名(チャンネル番号順)を固定の表として列挙する(`docs/design/chip-notes.md`の「DSG」の「GUIへの列挙」)。パート数がチップごとに違うので、チップごとに別々の配列にしている(`DeviceFactory::defaultChCount()`が返すチャンネル数と一致させる)。

## OPL系の内蔵リズム

OPL系の内蔵リズム(`COPLRhythm`)は`0x70`の経路を使わない。OPLのリズム音はBD(2op)以外が1オペレータの実際のFM音色パラメータを要求し、標準の`HwPatch`の形に収まるので、`VOICE_PATCH_OPL_RHY`(0x23)という通常の`VoicePatchType`を持ち、他の直接モードのチップと同じ経路(`resolveTriple`→`HwBankRegistry`)で音色を解決する(`docs/design/chip-drivers.md`の「特殊ルーティングの判断基準」)。HwBankの名前空間も`VOICE_PATCH_OPL_RHY`で独立している。

「チャンネル番号=楽器」という制約は`0x70`と同じだが、OPL_RHYは任意の音色をHwBankから選べる設計なので、`hwProg`(格納位置)をそのままチャンネル番号には使えない(1チャンネルにつき音色を1つしか持てなくなる)。代わりに、**HwPatch自身が持つ`ext.rhythm_ch`**(0=HH/1=CYM/2=TOM/3=SD/4=BD)を`resolveTriple()`が参照し、`ResolvedTriple::forcedCh`に設定する。未設定(255)または範囲外は解決失敗(無音、警告ログ)。`hwProg`(どの音色か)と`ext.rhythm_ch`(どの楽器向けか)は独立した軸なので、同じ楽器の音色のバリエーションを別のスロットに置ける(各スロットの`rhythm_ch`を正しく設定するのは音色を書く側の責任)。

## チョークグループと発音の参照

ドラムキットの`chokeGroups`(`DrumPatch::chokeGroups`、MIDIノート番号の組のリスト)は、`CRhythmCh::applyNoteOn`(MIDIを受ける層)で実現する。同じ組の他のノートに対して`NoteSlots::choke()`(そのノートが使う全ToneLayerを`ISoundDevice::forceDamp()`で強制的に消音する)を呼ぶだけで、ノートが使うデバイスやチャンネルには関知しない。DVA(`allocCh`/`queryCh`)には手を加えない。チョークは「新しいノートを鳴らす前に、古いノートを能動的に止める」というMIDIチャンネル/ノート単位の制御で、デバイス側のチャンネル資源の管理とは別の関心事だから。ノート番号の組なので、各ノートが別のデバイス・別のPatchを使っていても働き、全レイヤーを止める。

`ResolvedLayer::forcedCh`(内蔵リズムのハードウェア上の制約)と`chokeGroups`(ノート間の相互の消音)は、どちらも「チャンネル」に関わるが、出自も実現する層も別物。

### 発音の参照の寿命

`LayerSlot`は、「発音の管理としてのアクティブ状態(`active`)」と、「直前の発音への物理チャンネルの参照(`dev`/`devCh`/`noteOnSeq`)」を分けて持つ。ドラムのゲートタイムはごく短く、ゲートが満了した時点ではチップ側でまだ音が鳴っている(FMのリリース中、ADPCMのサンプル再生中)のが普通なので、参照はノートオフ・ゲート満了の後も持ち続ける。参照の用途は2つ。

- **同じノートの再打鍵**: 参照が有効なら`allocCh()`ではなく`assignCh(devCh, ...)`で**同じ物理チャンネルを取り直す**。`assignCh()`はKey Offと各ドライバの`wasReleasing`による事前のダンプ(RR最大化)を挟んでから再アタックするので、前の音は物理的なリトリガーで自然に切れる(1ノート=1ボイス)
- **チョークグループ**: 別のノートどうしは別のPatch・別のチャンネルなので、同じチャンネルを取り直すことでは代えられず、`forceDamp()`による強制的なダンプが要る

参照が有効かどうかは、`ISoundDevice::isChOwnedBy()`**と**`ChState::noteOnSeq`の一致で判定する。`CRhythmCh`は128ノートをまたぐ**単一の`IMidiCh`**なので、`isChOwnedBy()`だけでは、同じ`CRhythmCh`の別のドラムノートへ再配分された`devCh`も「自分の所有」と判定され、無関係なドラム音を誤って止めて(あるいは奪って)しまう。`noteOnSeq`はノートオンのたびに増えるので、そのチャンネルが一度でも再ノートオンされていればO(1)で分かる。無効なら通常の`allocCh()`を使う。

**参照は自動的に失効する。** デバイス側の`ChState`が`Releasing`→`Empty`へ落ちた時点で`isChOwnedBy()`がfalseになるので、`kReleasingHoldMs`(2秒)がチョークできる時間の窓になる。2秒を超えて鳴り続けるサンプルはチョークできない。サンプルの実際の再生長を解析・管理する方式は手間に見合わないので採らない。この制限が問題になったら、`kReleasingHoldMs`をデバイスごとに上書きできるようにする程度の調整で足りる。

### NoteOffで止まらないデバイスの`forceDamp()`

| デバイス | キーオン/ダンプのレジスタ | `forceDamp()` |
|---|---|---|
| `CAdPcm2610A` (ADPCM-A) | `$00` (bit7=DM) | `0x80\|(1<<ch)`を書く |
| `COPNARhythm` (OPNA内蔵リズム) | `$10` (bit7=ダンプ) | `0x80\|(1<<ch)`を書く |

この2つは`updateKey(ch, false)`が何も書かない。ワンショットのドラム音は短いゲートタイムで途中から切るより最後まで鳴らすほうが自然なので、意図してそうしている。そのため、NoteOffとは別の強制ダンプの経路(`forceDamp()`)が要る。

両者は`updateKey(ch, true)`でも**キーオンの前にダンプを書く**。NoteOffで止まらない以上、同じチャンネルを再び割り当てる時点で前のサンプルがまだ鳴っている可能性が常にあり、それを挟まないと「同じノートの再打鍵=同じ物理チャンネル」が確実なリトリガーにならない。FM系ドライバが`wasReleasing`を見て行う事前のダンプと同じ役割だが、こちらは純粋な停止の指示で何度書いても同じなので、条件を付けず常に書く(`ChState`が`Empty`に落ちた後もまだ鳴っている可能性があるため)。

`CRhythmCh::allSoundOff()`(CC#120)も同じ`choke()`で実装している。`CYmDelta`(ADPCM-B)・`CAdPcmZ280`(YMZ280B)・`COPLLRhythm`・`COPLRhythm`・`COPL4AWM`はキーオフを書くので、`forceDamp()`は既定の実装のままでよい。

## PSG系共有バンク

SSG/EPSG/DCSG/SAA/SCCは、1つの入口(`VOICE_PATCH_SSG`=0x40)とHwBankの名前空間を共有する。「音色データがデバイスを選択する」原則を、**バンクの検索**と**デバイスの解決**の2段階に分けることで、名前空間の共有と、正確なデバイスの選択(SSGとSCCを誤って束ねない)を両立している。

```
① バンク検索: PatchManager::hwBankLookupVoicePatchType()がPSG系のCC#0の値を
   VOICE_PATCH_SSG(0x40)へ寄せ、共有バンクを引く。
② 音色の取得: hwBank/hwProgで個別のHwPatchを取得する。
③ デバイス解決: HwPatch::ext.targetVoicePatchTypeを読み、実際に対象とする
   チップ(VOICE_PATCH_SSG/EPSG/DCSG/SAA/SCCのいずれか)で
   findDeviceIndexByVoicePatchType()を呼ぶ。未設定(0)なら
   FITOMConfig::getPsgFallbackChip()(プロファイルのpsg_fallback_chip、既定SSG)
   を使う。
```

同じ共有バンクに、SSG専用の音色(HWエンベロープを使う)とSCC専用の音色(波形メモリを使う)が混ざってよい。デバイスの解決は音色ごとに行うので、SSGとSCCが`mergeSpannableDevices`の対象になることはない(①では両者とも0x40として扱うが、③で分かれる)。

PSG系の共有バンクには、`findFallbackDeviceIndex`(内容を見てチップ間の互換性を判定する仕組み)を使わない。チップ間の機能の差が大きく、単純な互換性の判定が難しいので、明示的な`targetVoicePatchType`の指定と、プロファイル単位の`psg_fallback_chip`の切り替えだけで対応する。

## 演奏中の呼び出し順の前提

- `CSoundDevice::assignCh()`は`updateVoice()`の前に`VoiceProcessor::onNoteOn()`を呼ぶ(`docs/design/voice-processor.md`の「呼び出しの順序」)。`allocCh`/`assignCh`はベロシティと`swPatch`を引数で直接受け取る
- レガートは`assignCh`を通らない。割り当て済みのチャンネルに`setVelocity()`(TLだけを更新し、`onNoteOn()`は呼ばない)を呼ぶ。ポルタメントは音程(Fnumber)の計算と反映のタイミングにしか関わらない

## 相対パスの解決基点

`banks.*[].file`(hw_banks/sw_banks/patch_banks/drum_banks/scc_wave_banks/pcm_banks/sf2_banks)の相対パスは、プロファイルファイルのディレクトリを起点に解決する(起動時のカレントディレクトリは関係しない)。`*.pcmbank.json`の中の`adpcm_json`/`bin_file`は、その`*.pcmbank.json`のディレクトリを起点に解決する(`PatchManager::loadPcmBankJson()`)。どちらも「参照元のファイルの置き場所を起点にする」という同じ考え方。`hw_plugins[].dll`/`midi_backend.dll`は実行ファイルのディレクトリが起点で、解決基点が違う(`docs/design/config-loading.md`)。
