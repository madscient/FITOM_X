# FITOM_X バンクファイル リファレンス

`docs/design/patch-resolution.md`から、バンクファイルを書く人向けの節を写した(書き直す前の版)。

---

### バンク設計の推奨方針: 常に最上位チップ型で宣言する

同一チップファミリー内でパッチを作成する際、`voicePatchType`は
**そのファミリーの最上位(最も機能が豊富な)チップ**として宣言することを
推奨する。

| ファミリー | 推奨する宣言値 |
|---|---|
| OPN系 | `VOICE_PATCH_OPN2` |
| OPL系 | `VOICE_PATCH_OPL3_2` |
| OPM系 | `VOICE_PATCH_OPZ2` |
| SSG系 | `VOICE_PATCH_EPSG` |

理由: `DeviceFactory::acceptsFallback`(各チップの`*AcceptsFallback`関数)
が、パッチの実際の内容(`hwOp[].WS`等、そのチップ固有の拡張機能を
実際に使っているか)を見て、下位チップでも安全に再生できるかを自動判定
する設計になっている。そのため、上位型で宣言しておけば:

- 下位チップしか無い環境でも、パッチが実際に上位専用機能を使っていない
  限り、自動的にフォールバックされて正しく発音する
- 「下位チップ用バンク」「上位チップ用バンク」を重複して用意する必要が
  なくなる(両方が混在する環境でも、1つのバンクファイルで完結する)

逆に、あるパッチが実際に上位チップ専用の機能(非サイン波WS等)を意図的に
使う場合は、それを承知の上で宣言している以上、下位チップ環境では
そのレイヤーが鳴らない(スキップされる)のは想定通りの挙動である。

**OPLL系のvoicePatchTypeで`hwBank==0`を指定した場合、通常の
HwBankRegistry検索(JSON)を経由せず、`hwProg`の値から直接HwPatchを
機械的に合成する。バンク0はROM音色専用の予約領域であり、JSON
(プリセット)による定義は不可**(たとえバンク0にJSONバンクを
ロードしても、`resolveTriple()`が常にこちらを優先するため参照
されない)。

```
hwProg (Program Change の値、7bit) の内訳:
  上位3bit (hwProg >> 4): 使用するOPLL系チップ種別を選択
    0 = OPLL (OPLL2も含む)   1 = OPLLX
    2 = OPLLP                3 = VRC7
    4-7 = 未定義 (無音)
  下位4bit (hwProg & 0xF): ROM音色インデックス
    0    = 無音 (ユーザー音色(INST=0)との衝突を避けるため意図的に予約)
    1-15 = ROM音色インデックス (実機のINSTナンバーに直接対応)
```

```
① profile.jsonのhw_banks[]に、role="builtin_swpatch_meta"を持つ
   エントリを追加する(group/bankは意味を持たないが、スキーマ上は
   引き続き必須のまま)。
   { "group": "OPLL", "bank": 0, "file": "banks/OPLL/rom_sw_meta.hwbank.json",
     "role": "builtin_swpatch_meta" }
② このファイル内の各パッチエントリは、通常のops[](FMオペレータ
   パラメータ)ではなく、builtinフィールドで対象のROM音色を指定する
   (ops/builtinはoneOfで排他、どちらか一方が必須)。
   { "prog": 0, "builtin": {"patch_type": "OPLL", "patch_no": 3},
     "sw_bank": 2, "sw_prog": 5 }
   ("patch_type": "OPLL"の"patch_no": 3 = "Piano"にsw_bank=2/prog=5
   のSwPatchを紐づける、という意味。prog番号(この例では0)自体は
   ファイル内での一意性のためだけに使われ、検索キーにはならない)
③ PatchManager::resolveOpllRomVoice()が、(variantSel, instIndex)を
   HwBank::findByBuiltinRef()で線形探索し、一致するエントリが
   あればそのsw_bank/sw_progでSwPatchを解決する。
   DSGは PatchManager::resolveDsgBuiltinVoice() が
   (BUILTIN_TYPE_DSG, hwProg)で同じ探索を行う。
```

`patch_type`に指定できる値と、対応する`patch_no`の値域:

| `patch_type` | `patch_no` | 対象 |
|---|---|---|
| `OPLL` / `OPLLX` / `OPLLP` / `VRC7` | 1〜15 | OPLL系ROM音色(`initOpllRomPatches()`)。0はユーザー音色との衝突回避のため無音として予約 |
| `DSG` | 0〜19 | DSGビルトイン音色(`initDsgBuiltinPatches()`)。`prog = 波形*4 + エンベロープ`で、**0も正規の音色** |

`BuiltinRef`の未設定センチネルは`0`ではなく`-1`であることに注意
(`isValid()`は`patchNo >= 0`を要求する)。DSGは`patch_no: 0`
(`St.Percussive`)が正規の音色のため、ここを`>= 1`にすると
そのエントリだけが黙って一致しなくなる。

**`prog`は識別子ではなく「バンク内の格納スロット番号」**である点に注意。
検索は`(patch_type, patch_no)`だけで行われ、`prog`は一切参照されない。
`HwBank::patches`は128要素の配列1本しか無く、`patch_type`ごとに
分かれた名前空間ではないため、**OPLL系とDSGのエントリが同じ`prog`を
使うと後勝ちで上書きされ、片方が黙って消える**。

| フィールド | 役割 | OPLL系とDSGの関係 |
|---|---|---|
| `patch_type` | 対象チップの識別 | 区別される |
| `patch_no` | チップ内の音色番号 | それぞれ独立した値域 |
| `prog` | ファイル内の格納スロット | **共有(衝突すると後勝ち)** |

`prog`は省略でき、その場合は直前のエントリ+1で自動採番されるため
衝突しない(手書き・自動生成いずれもこちらが安全)。明示的に振る場合は
チップごとに範囲を分けること(例: OPLL系 0〜59、DSG 60〜79。
容量はOPLL系が最大60エントリ[4バリアント×15]、DSGが20エントリで、
128スロットに収まる)。

`hwbank.schema.json`側も、`prog`の必須/任意を`ops`/`builtin`の`oneOf`
分岐で切り替える(`prog`はitems直下の`required`には置かない):

- 通常のパッチ(`ops`)は`prog`が**必須**。こちらの`prog`はProgram Change
  番号そのもので、`loadHwBankJson()`も`entry.value("prog", -1)`で未指定の
  エントリを読み飛ばすため、省略を許すと黙って消えるエントリが生まれる。
- `builtin`参照エントリは`prog`が**任意**。格納スロット番号にすぎず、
  ローダ側が自動採番できるため。

(スキーマ側だけ一律必須にしていると、エントリを動的に生成する運用で
使いもしない`prog`を採番させられ、かえって衝突事故を招く)
`loadBuiltinMetaBankJson()`は`prog`の重複・範囲外、および
`builtin`参照を欠くエントリ(=`findByBuiltinRef()`で永久に一致しない)を
`FITOM_LOG_WARN`で報告する。

このメタデータバンクは通常のHwBankRegistryには登録されず、
`PatchManager::builtinMetaBank_`という専用の保持スロットに
直接格納される(`loadBuiltinMetaBankJson()`)。1つの共有ファイルに
OPLL/OPLLX/OPLLP/VRC7の全バリアントとDSGのエントリをまとめて記述できる
(`patch_type`フィールドで対象チップを区別するため)。保持スロットは
1つしか無く、`role="builtin_swpatch_meta"`を複数指定すると後勝ちで
上書きされるため、必ず1ファイルにまとめること。

### サンプルベース音源系(ADPCM-B/A/PCMD8/AWM)へのパフォーマンスパッチ紐づけ(2026年7月新設)

`SampleZonePatch`(`SampleZone`単位)に`swBank`/`swProg`フィールドを
追加し、`HwPatch`と全く同じソフトフォールバック規約(-1=参照なし、
解決失敗はソフトな失敗)で紐づけられるようにした。

```
SampleZone::swBank/swProg → SwPatch への参照(HwPatch::swBank/swProgと同じ規約)
```

**入力経路は2つ**:

- `*.samplezonebank.json`(手書き、現状は事実上AWM専用): `zones[]`の
  各要素に`sw_bank`/`sw_prog`を直接記述する
  (`config_schema/samplezonebank.schema.json`参照)。
- `*.pcmbank.json`(ADPCM-B/A/PCMD8): トップレベルに`swpatches[]`配列
  (`{entry_no, sw_bank, sw_prog}`)を新設し、`entries[]`(`adpcm_json`
  由来・直接記述いずれも)の各サンプルへ`entry_no`で対応づける。
  `PatchManager::loadPcmBankJson()`がentries[]から自動合成する
  単一ゾーンの`SampleZonePatch`(前掲「PCMバンク複数併用対応」節参照)へ、
  このswBank/swProgがそのまま反映される
  (`config_schema/pcmbank.schema.json`参照)。

**DrumNoteによる上書き**: `DrumNote::swBank/swProg`(既存)は、解決対象が
`HwPatch`(直接モード)か`SampleZonePatch`(direct型drumkit経由のAWM/ADPCM系)
かに関わらず同じ優先順位で効く(①DrumNote指定が解決できればそれを使う
→②無指定/解決失敗ならそのゾーン自身の参照にフォールバック→③それも
無ければ無し)。

### チョークグループ（ハイハット等の相互ダンプ、2026年7月新設）

ハイハットのクローズ/オープンのように、一方が鳴ると他方が即座に
消音される「チョーク」挙動が必要な楽器がある。旧実装では
`DrumNote::fixed_ch`で複数ノートに同一のデバイスチャンネル番号を
指定し、同一物理チャンネルへの再アサインが起こす物理的retriggerを
利用してこれを実現していた(`ISoundDevice::assignCh()`が同一chへの
再割り当て時に強制的に発音を切り替える副作用に依存)。しかしこの
方式は、対象デバイスのチャンネル構成という実装詳細にドラムキットの
設計が直接依存してしまい、ハードウェア構成を変更するとキット定義
まで壊れる、という問題があった。

2026年7月、これをハードウェア非依存な設計に置き換えた。

```
DrumPatch::chokeGroups: vector<vector<uint8_t>>  // MIDIノート番号のグループのリスト
```

`*.drumkit.json`(routed形式)のトップレベルに`choke_groups`として
記述する。ノートごとの個別フィールドではなくキット単位のリストに
した理由は、チョークが必要なノートはキット全体のごく一部
(ハイハット程度)であり、大半のノートには不要な情報だから。またIDの
採番(例: `choke_group: 2`のような整数値)をキット設計者が管理する
負担も避けられる — グループはノート番号を直接列挙するだけでよい。

```json
{
  "type": "routed", "name": "Standard Kit",
  "choke_groups": [[42, 44, 46]],
  "notes": [ ... ]
}
```

**参照は自動的に失効する。** デバイス側の`ChState`が
`Releasing`→`Empty`へ落ちた時点で`isChOwnedBy()`がfalseになるためで、
`kReleasingHoldMs`(2秒)がチョーク可能な時間窓になる。ドラム用途では
概ね十分だが、**2秒を超えて鳴り続けるサンプルはチョークできない**という
既知の制限がある。サンプルの実再生長を解析・管理する方式は費用対効果が
見合わないため採らない。将来この制限が問題になった場合は、
`kReleasingHoldMs`をデバイス別に上書き可能にする程度の調整で足りる。

### PSG系共有バンク（voicePatchType=0x40固定、2026年7月〜）

SSG(YM2149/AY-3-8910)/EPSG(AY8930)/DCSG(SN76489)/SAA(SAA1099)/
SCC(K051649/K052539)は、実機のハードウェアケーパビリティに大きな差が
ある一方(SSGはHWエンベロープ+ノイズ、SCCは波形メモリ、DCSGは単純な
矩形波のみ等)、音色定義の幅が狭く、1チップ専用のバンクでは名前空間
(CC#0×Bank×Prog)を音楽的に意味のあるバリエーションで埋めることが
難しい。このため、PSG系チップは全て共通の入口(`VOICE_PATCH_SSG`
=0x40固定)/HwBank名前空間を共有する設計に変更した。

「音色データがデバイスを選択する」原則(`docs/design/chip-drivers.md`
参照)を、**バンク検索**と**デバイス解決**の2段階に分離することで、
名前空間の共有と、正確なデバイス選択(誤ったspanning防止)を両立して
いる。

```
① バンク検索: voicePatchType=VOICE_PATCH_SSG(0x40固定)で共有バンクを
   引く(PatchManager::hwBankLookupVoicePatchType()が、PSG系のCC#0値を
   この入口へ寄せる)。他のチップ族と異なり、CC#0=EPSG/DCSG/SAA/SCCを
   指定してもバンク自体は常に0x40の名前空間から引かれる。
② パッチ取得: hwBank/hwProgで個別のHwPatchを取得(既存の仕組み通り)。
③ デバイス解決(★新設): HwPatch::ext.targetVoicePatchTypeを読み、
   実際に対象とするチップ(VOICE_PATCH_SSG/EPSG/DCSG/SAA/SCCの
   いずれかの実際の値)でfindDeviceIndexByVoicePatchType()を呼ぶ。
   未設定(0)の場合はFITOMConfig::getPsgFallbackChip()
   (プロファイルのpsg_fallback_chip、デフォルトSSG)にフォールバック
   する。
```

同じ共有バンクの中に、SSG専用パッチ(HWエンベロープ使用)とSCC専用
パッチ(波形メモリ参照)が混在してよい。デバイス解決はパッチ単位で
行われるため、SSGとSCCが誤って`mergeSpannableDevices`の対象に
なることはない(①のバンク検索段階では両者とも同じ0x40として扱われる
が、③のデバイス解決段階で正しく分岐する)。

フォールバック機構(`findFallbackDeviceIndex`、チップ間の内容互換性を
HwPatchの内容から判定する仕組み)はPSG系共有バンクでは使わない。
チップ間のケーパビリティ差が大きく、単純な互換性判定が難しいため、
明示的な`targetVoicePatchType`指定と、プロファイル単位の
フォールバック先切り替え(`psg_fallback_chip`)のみで対応する。

### リズムチャンネル: ドラムマップによる解決

`CRhythmCh`はCC#0(`0x79`によるメロディ復帰を除く)/CC#32を一切使わない。
ドラムバンクは常に固定バンク番号(`0`)を使い、Program Change の値だけが
「どのドラムキットを使うか」を選択する。

```
Program Change 受信 (CRhythmCh::progChange)
  → PatchManager::resolveDrum(0, prog)             // バンク番号は常に0固定
      → DrumBankRegistry[0].patches[prog] (= DrumPatch) を取得
      → NoteOn受信時、DrumPatch::getNote(note) で DrumNote を取得
          → DrumNote.voicePatchTypeでモード判定 (CC#0と全く同じ
            セマンティクス。CRhythmCh::resolveNote参照):
              voicePatchType==VOICE_PATCH_NONE(0、省略時デフォルト):
                通常モード。patchBank/patchProgをPatchBank番号/Progとして
                PatchManager::resolve(patchBank, patchProg, config)で解決
                (＝CInstChの通常モードと全く同じ経路。マルチレイヤー・
                 SwPatch適用あり)
              voicePatchType==0x01-0x6F: 直接モード。この値自体が
                VOICE_PATCH_*定数となり、patchBank/patchProgはHwBank
                インデックス/HwProgとして読み替わる。
                PatchManager::resolveDirect(voicePatchType, patchBank,
                patchProg, config, storage)で解決 (単層Patch。SwPatchは
                HwPatch自身の参照があれば適用される。DrumNote.sw_bank/
                sw_progでの上書きも可能)
          → DrumNote.playNoteを絶対ノート番号として発音
```

DrumNoteが直接モードを選べることで、ドラムキットの1音ごとにチップを
直接指定したい場合(例: キック=OPN、スネア=OPL、ハイハット=PSGという
混在構成)、そのためだけにPatchBank/Patchを個別に用意する必要がなくなる。

**プロファイル側の`drum_banks[]`は「プログラムチェンジ1つごとに独立した
ファイル」を割り当てる方式。**1ファイルに全prog分を詰め込む方式は、
ファイルが肥大化するため採用していない。

```json
"drum_banks": [
  { "prog": 0, "name": "OPL4 AWM GM std kit", "file": "opl4awm.drumkit.json" },
  { "prog": 1, "name": "OPNA/B GM std kit",    "file": "opnAdpcm.drumkit.json" }
]
```

各`*.drumkit.json`ファイルは`"type"`フィールドで2種類の記述形式を持てる
(詳細は`config_schema/drumkit.schema.json`参照):

- **`"routed"`**: ノートごとに任意のPatch(bank/prog)へ個別にルーティングする、
  従来型の`notes[]`配列形式。FM合成系チップのように、ドラム音1つ1つが
  別々のPatchとして定義されている場合に使う。
- **`"direct"`**: 単一のPatch(bank/prog)への圧縮パススルー形式。
  OPL4 AWMのように、チップ自身が内蔵のキーゾーン切り替えを持つ場合
  (`SampleZonePatch`が自身の`zones[]`でノートごとの波形選択を完結させる)
  に使う。`[note_min, note_max]`の範囲に自動展開され、`play_note`は
  受信ノート番号そのまま渡される。ノート数ぶんの重複記述を書かずに済む。
  ロード時(`PatchManager::loadDrumKitJson`)に`DrumNote`配列へ展開する
  ため、`CRhythmCh`側のランタイムコードは`"routed"`と全く同じ経路で動く。

例 (`"direct"`型、OPL4 AWMの内蔵GM標準ドラムキットをそのまま使う場合):
```json
{
  "type": "direct",
  "name": "OPL4 AWM GM std kit",
  "patch_bank": 10,
  "patch_prog": 0,
  "note_min": 27,
  "note_max": 87
}
```
(`patch_bank=10`は、`voice_patch_type: VOICE_PATCH_AWM`のToneLayerを持つ
通常のPatchBankを指す。そのPatchのHwBank/HwProgが、OPL4 AWM用の
`SampleZoneBankRegistry`に登録されたYRW801内蔵ドラムテーブルを参照する)

## ToneLayer 記述例（通常モード）

```json
{
  "prog": 1,
  "name": "Strings 2 (Split: OPM + DCSG bass)",
  "layers": [
    {
      "voice_patch_type": 25,
      "hw_bank": 0, "hw_prog": 1,
      "note_range_lo": 48, "note_range_hi": 127,
      "transpose": 0, "enabled": true
    },
    {
      "voice_patch_type": 66,
      "hw_bank": 0, "hw_prog": 0,
      "note_range_lo": 0, "note_range_hi": 47,
      "transpose": -12, "enabled": true
    }
  ]
}
```

`voice_patch_type: 25` = `0x19` (OPM)、`voice_patch_type: 66` = `0x42` (DCSG)。
この例では:
- OPM (0x19): ノート 48〜127 を担当
- DCSG (0x42): ノート 0〜47 を担当、1オクターブ下げて発音

## HwBank 側のタグ付けルール

**同一 HwBank 内には同一 VoicePatchType のみを列挙する**というルールを設ける。
`HwBank::voicePatchType` はバンク単位で1つだけ保持し（パッチ単位ではない）、
`PatchManager::loadHwBankJson()` の呼び出し時（`Config::loadBanks()`）に
プロファイルの `hw_banks[].group` 文字列から `Config::stringToVoicePatchType()`
で自動導出される。

```json
// profile.json の hw_banks[]
{ "group": "OPZ", "bank": 0, "file": "banks/OPZ/tx81z/tx81z_1.hwbank.json" }
```

`group: "OPZ"` → `voicePatchType = VOICE_PATCH_OPZ(0x1a)`
→ `HwBankRegistry` の検索キーは `(0x1a, bank=0)`

バンク番号の名前空間はチップ (VoicePatchType) ごとに独立しており、
`hw_banks[]` に `group` だけ変えた同じバンク番号のエントリを並べれば、
同一ファイルを複数のチップから引けるようになる（例: 同じOPM系バンクを
`group:"OPM"` と `group:"OPZ"` の両方に登録すると、CC#0=25/26のどちらでも
選択できる）。VoiceGroup（粗い分類、9種）はチップ族の括りを問う用途
（フォールバック候補の探索やログの診断表示など）にのみ使い、バンクの
検索キーには使わない。

PSG系だけは例外で、族全体で1つのバンク名前空間 (`VOICE_PATCH_SSG`) を
共有する（本書「PSG系共有バンク」節参照）。

### pcm_banks の group (2026年7月新設)

ADPCM-B/ADPCM-A/PCM-D8 (`VOICE_GROUP_PCM`) は `isSampleBasedVoicePatchType()`
の対象であり、AWMと同じ`SampleZonePatch`スキーマ(HwPatch/`ops[]`は使わない、
本書「サンプルベース音源系(0x50-0x54)」節参照)で扱われる。そのため通常の
`hw_banks` + 手書きの `*.hwbank.json` とは別に、`banks.pcm_banks[]` エントリ
だけで完結させることができる。`pcm_banks[].group` (任意、`ADPCMB`/`ADPCMA`/
`PCMD8`) を指定すると:

1. `CFITOM::initDevices()` が、そのVoicePatchTypeに一致するPCMデバイスへ
   このバンク番号を `setPcmRegistry()` で自動的に割り当てる
   (`PcmBankRegistry::findBankNoForVoicePatchType()`)。以前は全PCM
   デバイスがバンク番号0を決め打ちで共有しており、コーデックの異なる
   複数のPCMバンク(ADPCM-A用/ADPCM-B用等)を同一プロファイル内で
   併用できなかった。
2. `PatchManager::loadPcmBankJson()` が、参照先の `*.pcmbank.json` が
   持つ `entries[]`(adpcm_packer出力由来、各エントリに`name`と
   `root_note`を持つ)から、`prog`=エントリ番号のnamed patch
   (`zones`は`key_min/max`・`vel_min/max`ともフル範囲の1件のみ、
   `wave_index`=同じエントリ番号、`root_note`=エントリの`root_note`、
   省略時69)を自動合成し、`*.samplezonebank.json`経由で読み込んだ場合と
   同じ`SampleZoneBankRegistry`(`(voicePatchType, bankNo)`をキーにする
   ため、AWM等の他チップ族と同じバンク番号を使っても衝突しない)へ登録
   する。これにより、`*.samplezonebank.json`を別途手書きしなくても、
   パッチピッカー等から個々のPCMサンプルを通常のProgram Change経由で
   選択できる。

`group`を省略した場合は波形データ(バイナリ+エントリオフセット)の登録
のみを行い、バンク番号0への決め打ち割り当て・named patch自動合成の
どちらも行わない(既存プロファイルとの後方互換)。

```json
// profile.json の pcm_banks[]
{ "group": "ADPCMA", "bank": 1, "file": "banks/PCM/pss680/pss680_opnb.pcmbank.json" }
```

## バンクファイル管理の推奨ディレクトリ構成

```
banks/
  OPM/
    dx27_dx100/00_default.hwbank.json
  OPZ/
    tx81z/tx81z_1.hwbank.json
  OPL3/
    alsa/std_opl3.hwbank.json
  sw/
    default_gm.swbank.json
    compat_zero.swbank.json
  patches/
    00_general.patchbank.json
```

### 相対パスの解決基点

`banks.*[].file`(hw_banks/sw_banks/patch_banks/drum_banks/scc_wave_banks/
pcm_banks/sf2_banks)に書く相対パスは、**そのプロファイルファイル自身が
置かれているディレクトリ**を起点に解決される(起動時のカレントワーキング
ディレクトリは無関係)。プロファイルとバンク一式をどこに配置しても、起動時
のCWDに依存せず常に同じ結果になる。

(`sf2_banks`はスキーマ(`config_schema/profile.schema.json`)のみ追加済みで、
ローダー(`Sf2BankRegistry`、`docs/design/sf2-direct-path.md`参照)は
本書執筆時点(2026年7月)ではまだ未実装。実装時にはこの規約に従うことを
設計上決めているため、先行してここに記載している。)

例えば `config/profiles/preset_opn.profile.json` からリポジトリルート直下の
`banks/` を参照する場合は `"../../banks/OPN/gm/necopn_gm.hwbank.json"` の
ように書く(`config/profiles/` は `banks/` の兄弟ではなく、2階層下にある
ため)。プロファイルと同じディレクトリに `banks/` を置く運用であれば、
単に `"banks/OPN/gm/xxx.hwbank.json"` のように書けばよい。

なお `pcm_banks[].file` が指す `*.pcmbank.json` ファイル自身が持つ
`adpcm_json`/`bin_file` 参照は、既に**そのpcmbankファイル自身の親
ディレクトリ**を起点に解決される(`PatchManager::loadPcmBankJson()`)。
今回の変更はこれと同じ「参照元ファイルの置き場所を起点にする」考え方を
profile側の `banks.*[].file` 全種別にも揃えたものであり、`hw_plugins[].dll`
/`midi_backend.dll`(実行ファイルのディレクトリが起点。`docs/design/config-loading.md`
参照)とは解決基点が異なる点に注意。
