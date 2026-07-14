# パッチ構造設計書

## 概要: 3層分離 + VoicePatchType による解決

```
PatchBank (*.patchbank.json)
  └── Patch [prog 0..127]
        ├── ToneLayer [0..3]          ← 発音レイヤー（最大4チップ）
        │   ├── voicePatchType        → デバイスタイプID (VOICE_PATCH_*, 0x10-0x54)
        │   ├── hw_bank / hw_prog     → HwPatch への参照キー
        │   └── note_range / transpose / volume_offset / pan_offset
        └── poly
```

```
HwBank (*.hwbank.json)  ← VoiceGroup (粗い分類, 9種) ごとに登録
  ├── voicePatchType     ← バンク全体で共通の細分類タグ (整合性検証用)
  └── HwPatch [prog 0..127]
        ├── FmHwVoice hw    (FB / ALG / AMS / PMS / NFQ)
        ├── FmHwOp hwOp[4]  (AR/DR/SL/.../TL/KSR/.../MUL/DT1/DT2/AM/VIB/EGT/WS)
        ├── FmChipExt ext   (REV / EGS / DM0 / DT3 / ALG_EXT / HWEP)
        └── sw_bank / sw_prog → SwPatch への参照キー (2026年7月〜、-1=参照なし)
```

```
SwBank (*.swbank.json)  ← チップ族共通・1セット
  └── SwPatch [prog 0..127]
        ├── FmSwVoice sw          (チャンネルLFO / ビブラート)
        ├── FmSwOp swOp[4]        (ベロシティ感度 / トレモロLFO)
        └── fine_transpose        (固定トランスポーズ[セント、-1200〜+1200]、2026年7月〜)
```

**SwPatchへの参照は、以前はPatch単位(全ToneLayerで共有)だったが、
2026年7月にHwPatch単位(レイヤーごとに異なりうる)へ変更した。** これにより:

- 直接モード(CC#0=0x01-0x6F)やビルトインリズム音源(CC#0=0x70)等、
  従来「SwPatch適用対象外」だった経路でも、HwPatch自身が参照を
  持てばパフォーマンスパッチが効くようになった
- `DrumNote`にも`sw_bank`/`sw_prog`の個別上書きフィールドが追加され、
  ビルトインリズム音源のように「HwPatchが楽器を区別できない」場合
  でも、楽器ごとに異なるパフォーマンスパッチを指定できる
  (優先順位: ①DrumNote指定が解決できればそれを使う → ②DrumNote無指定
  (-1)、または指定されたが解決に失敗した場合は、"無指定だった場合と
  同じ扱い"としてHwPatch自身の参照にフォールバックする → ③それも
  無ければパフォーマンスパッチ無し。DrumNote側の解決失敗は「無指定」
  と等価に扱われ、HwPatch側へのフォールバックを妨げない点に注意)

---

## VoicePatchType: 音色パッチ互換性分類

**DeviceFactory の `DEVICE_*`（チップドライバ生成用ID）とは独立した分類軸。**
「ボイスパラメータ・ハードウェア機能が一致するチップ」だけをまとめたもので、
`FITOMdefine.h` に `VOICE_PATCH_*` として定義されている（0x10〜0x54、詳細は後述。CC#0直接モードで使うため0x6F以下に収める）。

1台の物理/エミュレーターデバイスは以下の2つの異なる分類軸を同時に持つ:

| 分類軸 | 用途 | 値の例 |
|---|---|---|
| `deviceType` (`DEVICE_*`) | `DeviceFactory::create()` でどのチップドライバクラスを生成するか | `DEVICE_OPNA`=4 |
| `voicePatchType` (`VOICE_PATCH_*`) | どの音色パッチデータと互換性があるか | `VOICE_PATCH_OPN2`=0x11 |

`Config::getVoicePatchType(deviceIndex)` が `deviceType → voicePatchType` を変換する
（`Config::deviceTypeToVoicePatchType()`、静的関数）。

### VoicePatchType 一覧

| ID | 名称 | 対応チップ例 | VoiceGroup |
|---|---|---|---|
| 0x10 | OPN | YM2203, YMF264 | OPNA |
| 0x11 | OPN2 | YM2612/YM3438/YMF276/YM2608/YM2610系 (統合) | OPNA |
| 0x19 | OPM | YM2151, YM2164 | OPM |
| 0x1a | OPZ | YM2414 | OPM |
| 0x1b | OPZ2 | YM2424 | OPM |
| 0x20 | OPL | YM3526, YM3801 | OPL2 |
| 0x21 | OPL2 | YM3812, YMF264/289/278-2OP | OPL2 |
| 0x22 | OPL3_2 | YMF262(2OPモード)、YMF264/289/278-2OP | OPL3 |
| 0x28 | OPLL | YM2413, YM2420 | OPLL |
| 0x29 | OPLLP | YMF281 | OPLL |
| 0x2a | OPLLX | YM2423 | OPLL |
| 0x2b | VRC7 | FS1001 (OPLLからリズムCH削除、楽音6chのみ) | OPLL |
| 0x30 | OPL3 | YMF264/289/278-4OP | OPL3 |
| 0x38-3b | SD-1/MA-3/MA-5/MA-7 | YMF825 等 | MA3（未実装、値のみ予約） |
| 0x40 | SSG | YM2149, AY-3-8910 | PSG(無波形) |
| 0x41 | EPSG | AY8930 | PSG(無波形) |
| 0x42 | DCSG | SN76489 | PSG(無波形) |
| 0x43 | SAA | SAA1099 | PSG(無波形) |
| 0x48 | SCC | SCC, SCCP | PSG(波形ROM) |

サンプルベース音源系(0x50-0x54)は、`VOICE_PATCH_*`をBankSel.MSB
(CC#0)による直接モードのチップ選択IDとしてそのまま使うため、
`0x01`-`0x6F`の範囲に収める必要がある(`0x70`-`0x7F`は内蔵リズム音源
専用バンク(`0x70`)およびGM2リズム/メロディ切替(`0x78`/`0x79`)専用の
予約領域)。旧`0x70`-`0x74`から`0x50`-`0x54`へ変更した経緯がある。

| 0x50 | ADPCMB_Y8950 | Y8950 | PCM |
| 0x51 | ADPCMB | YM2608 | PCM |
| 0x52 | ADPCMA | YM2610 | PCM |
| 0x53 | PCMD8 | YMZ280(B) | PCM |
| 0x54 | AWM | YMF278-AWM+YRW801 | PCM |

パン対応可否は VoicePatchType 単位でも一律ではない（例: 同じPSG無波形グループでも
SAAは per-channel ステレオパン対応、SSG/EPSG/DCSGは非対応）。
**このためFITOM_Xはパン対応可否を内部で判定・管理しない。**
パンを使いたい場合はエンドユーザーが対応デバイスのVoicePatchTypeを
自己責任で選択する（バンクセレクトLSB直接指定モード参照）。

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

### 解決アルゴリズムの内部統一: `PatchManager::resolveTriple()`

`voicePatchType + hwBank + hwProg` の3つ組から実際に発音可能な
(device, HwPatch または SamplePatch) を得るロジックは、以下の2箇所で
使われる、本質的に同一の処理である:

- `resolve()`: `Patch`内の各`ToneLayer`が持つ`(voicePatchType, hwBank, hwProg)`
- `resolveDirect()`: リアルタイムのBank Select直接モード(CC#0=type,
  CC#32=hwBank, ProgChg=hwProg)が持つ同じ3つ組

旧実装ではこの2箇所にほぼ同一のロジック(`acceptsFallback`呼び出し・
mismatchチェック・デバイス検索)が重複していたため、`PatchManager::
resolveTriple()`という共通のprivateメソッドに統一した。これにより、
片方だけを修正して他方の修正を忘れる、という事故が構造的に起こらなくなる
(実際、OPL3_2フォールバックの穴を発見・修正した際、この重複が
問題を複雑にしていた)。

**`resolveTriple()`は`voicePatchType == VOICE_PATCH_NONE`(0)を常に
失敗として扱う。** これは、CC#0の実時間セマンティクスにおける
「通常モード」(PatchBank参照)に相当する値であり、もし将来
`resolveTriple()`がPatchBank参照も扱えるよう拡張された場合、
`ToneLayer`同士が循環参照する経路(Patch AのレイヤーがPatch Bを参照し、
Patch BのレイヤーがPatch Aを参照する、といった無限再帰)を開いて
しまうため、あらかじめこの入口で構造的に禁止してある。

---

## 3つの解決モード（メロディチャンネル: 通常/直接、リズムチャンネル: ドラムマップ）

CC#0(Bank Select MSB)の値が、通常モード/直接モード/GM2リズム切替という
3種類の意味を排他的に切り替えるモード選択子になっている。詳細は
`docs/midi-implementation-status.md`の「Bank Select 方式」を参照。

### 通常モード（CC#0=0）: PatchBank/Patch/ToneLayer による多層解決

```
Program Change 受信
  → PatchManager::resolve(bankSelL_, prog, config)
      → PatchBank[bankSelL_].patches[prog] を取得  (CC#32=PatchBank番号)
      → 各 ToneLayer について:
          Config::findDeviceIndexByVoicePatchType(layer.voicePatchType)
            → 一致するデバイスを devices[] から線形探索（完全一致のみ）
          見つからなければそのレイヤーだけスキップ（Patch全体は無効にしない）
          見つかれば HwBank から layer.hwBank/hwProg で HwPatch を解決
            → バンクの voicePatchType タグと layer.voicePatchType が
              不一致なら同様にスキップ
          isSampleBasedVoicePatchType(layer.voicePatchType) の場合のみ、HwBankでは
          なくSampleZoneBankRegistryを検索し、ResolvedLayer::samplePatch
          に結果を格納する（HwPatchとは別のスキーマ、詳細は後述の
          「サンプルベース音源系の解決」節参照）
```

**Program Change は発音中のノートに一切作用しない。**
新しいパッチは次の NoteOn から適用される（全モード共通の仕様）。

### 直接モード（CC#0=0x01-0x6F）: HwBank直接指定

```
bankSelM_ (CC#0) が 0x01-0x6F のとき:
  voicePatchType = bankSelM_        （CC#0の値そのものがVOICE_PATCH_*定数）
  hwBank         = bankSelL_        （CC#32の値、そのVoicePatchType用に
                                       プロファイル登録されたHwBankのインデックス）
  hwProg         = prog             （Program Changeの値）

PatchManager::resolveDirect(voicePatchType, hwBank, hwProg, config, storage)
  → PatchBank/ToneLayerを経由せず、単層Patchを直接構築
  → SwPatchは、解決されたHwPatch自身がsw_bank/sw_progを持っていれば
    適用される (2026年7月〜。以前は常にnullptrだったが、SwPatch参照が
    HwPatch単位になったことで直接モードでも効くようになった)
  → Patch::fromSingleLayer() を使用（ノート範囲フル、transpose/offsetなし）
```

`VOICE_PATCH_*`の値は全て`0x01`-`0x6F`の範囲に収まるよう採番されている
(`0x70`は内蔵リズム音源専用バンク(詳細は`docs/terminology.md`参照)、
`0x71`-`0x7F`はGM2リズム/メロディ切替(`0x78`/`0x79`)専用および将来予約)。
`bankSelM_==0`は通常モードのトリガーであり、直接モードには使えない。

### OPLL系ROM音色専用バンク（hwBank=0の特別扱い）

OPLL系チップ(OPLL/OPLLP/OPLLX/VRC7。OPLL2は`VOICE_PATCH_OPLL`を共有する
ため区別不要)は、実機に焼き込まれた15種類のROMプリセット音色を持つ。
**これらのROM音色はチップごとに実データが全く異なり、相互に互換性が
無い**ため、`acceptsFallback`はプリセット音色(`HwPatch::ext.ALG_EXT`
のbit0が1)のフォールバックを拒否する
(`opllFamilyAcceptsFallback`参照)。

一方で、ROM音色はチップのハードウェアに固定された既知のデータ
(実質「プリセットフラグ1bit + INSTナンバー4bit」の5bitのみ)であり、
チップ種別ごとに手作業でJSONバンクファイルを用意する必要は無い。
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

実装: `PatchManager::resolveOpllRomVoice()`。`resolveTriple()`(直接
モード・ToneLayer解決の共通処理)の入口付近で、`voicePatchType`が
OPLL系かつ`hwBank==0`の場合にこの専用ロジックへ分岐する。生成される
`HwPatch`は`PatchManager`が保持する固定サイズのキャッシュ
(`opllRomPatches_[4][16]`)を参照する。各エントリは`ext.ALG_EXT`と
`hw.ALG`(発音に必要な情報)に加え、`id`(isValid()判定用)と`name`
(表示名)も設定済み。音色名の出典は
[plgDavid/misc wiki](https://github.com/plgDavid/misc/wiki/Copyright-free-OPLL(x)-ROM-patches)
(耳コピによる非公式な近似データ。著作権フリーとして公開されているが、
正確な公式名称ではない可能性がある点に留意)。OPLLX/OPLLPは元資料の
表記が「番号→名前→説明(近似音色名を含む)」「名前A ~~ 名前B 説明」
のように複数の情報が併記されているため、代表的な楽器名のみを抽出して
採用した。

要求されたチップ種別が接続されていない場合、フォールバックは行わず
無音になる(ROM音色は相互互換性が無いため)。

### OPLL ROM音色へのパフォーマンスパッチ紐づけ（builtin参照、2026年7月新設）

ROM音色は`opllRomPatches_`内で機械的に合成されるため(前節参照)、
本来の`HwPatch::swBank`/`swProg`フィールドは常に未設定(-1)のまま
であり、通常の仕組みではパフォーマンスパッチ(SwPatch)を紐づけられ
なかった。この制約を解消するため、`hwProg`番号による機械的な対応
ではなく、**パッチ設計者が明示的に指定するuser specificな識別子**
(`BuiltinRef`)で紐づける、専用の仕組みを新設した。

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
```

このメタデータバンクは通常のHwBankRegistryには登録されず、
`PatchManager::opllBuiltinMetaBank_`という専用の保持スロットに
直接格納される(`loadOpllBuiltinMetaBankJson()`)。1つの共有ファイルに
OPLL/OPLLX/OPLLP/VRC7全バリアント分のエントリをまとめて記述できる
(`patch_type`フィールドでバリアントを区別するため)。

未設定・未一致の場合は、SwPatchが適用されないだけで、ROM音色自体の
発音は妨げられない(既存の設計方針と同じ、ソフトな失敗)。

`OPL4AWM`は対象外とした。AWMのROM音色(YRW801 GM)は`opllRomPatches_`
のようなC++内部ハードコードではなく、既に通常のバンクファイル
(`SampleZonePatch`、`*.samplezonebank.json`)として実装・配布されて
いるため、この専用の紐づけ機構自体が不要。もしAWMにパフォーマンス
パッチを適用したい場合は、`SampleZonePatch`構造体自体に`swBank`/
`swProg`フィールドを追加し、通常のバンクファイルへ直接記述する方が
自然(未実装、別タスク)。

### サンプルベース音源系（ADPCM-B/ADPCM-A/PCMD8/AWM）の解決

Y8950(ADPCM部)/YM2608(ADPCM-B)/YM2610・YM2610B(ADPCM-A)/YMZ280B(PCMD8)/
YMF278(AWM)は、通常のFMオペレータ型`HwPatch`ではなく、キーゾーン+
ベロシティレイヤー+波形/サンプル参照を持つ`SampleZonePatch`という、
全く別のデータ形式でパッチを表現する。`voicePatchType`が
`isSampleBasedVoicePatchType()`(`0x50`-`0x54`の範囲、連続した値として
意図的に採番されている)に該当する場合、`resolveTriple()`は通常の
`HwBankRegistry`ではなく、独立した`SampleZoneBankRegistry`を検索する。

```
resolveTriple(voicePatchType, hwBank, hwProg, ...)
  isSampleBasedVoicePatchType(voicePatchType) == true の場合:
    → SampleZoneBankRegistry::resolve(hwBank, hwProg)
        で SampleZonePatch を取得 (HwBankRegistryは一切参照しない)
    → 見つからなければ即座に失敗 (フォールバック機構は使わない。
      フォールバック判定にはHwPatchの内容参照が前提だが、
      SampleZonePatchにはその概念が無いため。サンプルベース音源系は
      現状フォールバック元/先になる想定がなく、実用上の制約は小さい)
    → Config::findDeviceIndexByVoicePatchType(voicePatchType)で
      デバイスを解決 (通常のHwPatch系と同じ経路)
    → ResolvedTriple::samplePatch に結果を格納
      (ResolvedTriple::hwPatchではなく、別のポインタフィールド)
```

通常モード(`resolve()`)・直接モード(`resolveDirect()`)いずれの経路でも、
最終的に`resolveTriple()`を通るため、この分岐は両モードに自動的に
適用される。ドラムキットでの利用例(`direct`型ドラムキット、OPL4 AWM
内蔵GM標準ドラムキットの参照)は前掲の「リズムチャンネル」節を参照。

対応するHwBank用スキーマは無く、`SampleZonePatch`専用のバンクファイル
形式(サンプルバンクJSON)で管理される。バンク番号の名前空間は、通常の
`VoicePatchType`と同じ`voicePatchTypeToVoiceGroup()`変換を経由するため、
`ADPCM-B`/`ADPCM-A`等、チップごとに独立している(詳細な種別と対応チップの
一覧は`docs/plugin-hwif.md`の「PCM/ADPCM メモリイメージの扱い」参照。
なお、この節で扱うのはFITOM_X内部の`SampleZonePatch`(音色メタデータ、
キーゾーン等)であり、`plugin-hwif.md`が扱うPCMメモリイメージ(実際の
波形データファイル)とは別レイヤーの話である点に注意)。

### 内蔵リズム音源専用バンク（CC#0=0x70、OPNA/OPLLのROM固定リズム）

YM2608(OPNA)/YM2413(OPLL)系は、通常のFM/PSGチャンネルとは別に、専用の
固定ドラム音を持つ内蔵リズムユニット(`COPNARhythm`/`COPLLRhythm`)を
持つ。これらは`deviceTypeToVoicePatchType()`が`VOICE_PATCH_NONE`を
返す特殊デバイスであり、通常の`VoicePatchType`ベースルーティング
(`resolve()`/`resolveDirect()`)では到達できないため、CC#0=`0x70`が
専用のアクセス経路として予約されている。ROM音色はソフトウェア側に
実データが存在しない(チップROM内蔵の固定音)ため、`HwPatch`の解決
自体が不要で、`PatchManager::resolveBuiltinRhythm()`がデバイスの
特定に加え、`hwProg`(Program Change相当)をそのままチャンネル番号
として検証・解決する(2026年7月、`DrumNote::fixed_ch`依存を廃止)。
これにより楽器選択は通常のBank/Prog指定と同じ経路(Program Change)
で行えるようになり、`CInstCh`の通常チャンネルからも内蔵リズムの
個別楽器を選択できる。「音色がデバイスを選択する」原則の例外である
点(音色データではなくチップ選択+チャンネル番号がデバイスを選ぶ)は
変わらない(詳細な経路・レジスタ構造は`docs/terminology.md`の「内蔵
リズム音源専用バンク」節を参照)。

**OPL系内蔵リズムチャンネル(`COPLRhythm`)は、この`0x70`経路を使わない**
点に注意。OPLのリズム音はBD以外1オペレータのみの実際のFM音色パラメータ
(`HwPatch`)を要求するため、`VOICE_PATCH_OPL_RHY`(0x23)という通常の
`VoicePatchType`を持ち、他の直接モードチップと全く同じ経路
(`resolveTriple`→`HwBankRegistry`)でパッチを解決する。「音色データが
標準の`HwPatch`形状でソフトウェア管理されているかどうか」が、特殊
ルーティングの要否を分ける基準になっている
(`docs/chip-driver-architecture.md`の「デバイスに特殊ルーティングが
必要かどうかの判断基準」参照)。

ただし「チャンネル番号=楽器」というハードウェア制約自体は`0x70`と
共通のため、`resolveTriple()`はHwBankRegistryから実際のHwPatchを
解決した後、`hwProg`をそのままチャンネル番号として`forcedCh`に設定
する(2026年7月、`DrumNote::fixed_ch`廃止に伴い`0x70`と設計を統一)。
詳細は`docs/terminology.md`の該当節を参照。

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

実現レイヤーは`CRhythmCh::applyNoteOn`(MIDIレシーバー)。ノートが
使う特定のデバイス/チャンネルには一切関知せず、`NoteSlots::stopAll()`
(そのノートが使う全ToneLayerを`noteOff()`+`releaseCh()`で停止する、
既存の「同一ノート上書き」処理と同じ仕組み)を、同じチョークグループの
他のノートに対しても呼び出すだけで実現する。DVA
(`allocCh`/`queryCh`によるチャンネル割り当て・スティール判定)には
一切手を加えない — チョークは「新しいノートの発音を始める前に、
古いノートを能動的に止める」というMIDIチャンネル/ノート単位の制御に
過ぎず、デバイス側のチャンネル資源管理とは独立した関心事だからである。

旧`fixed_ch`方式との違い:
- **層(レイヤー)の扱い**: 旧方式は`layer[0]`のみが強制チャンネル化の
  対象だったが、`chokeGroups`は`stopAll()`経由でそのノートの全レイヤー
  を止めるため、複数チップに跨るマルチレイヤードラム音でも正しく
  チョークできる。
- **ハードウェア依存性**: 旧方式は対象ノート同士が同じデバイス・
  同じチャンネル数を持つことを前提にしていたが、`chokeGroups`は
  ノート番号の集合でしかなく、各ノートが全く異なるデバイス/Patchを
  参照していても機能する。

なお`DrumNote::fixed_ch`自体は2026年7月に完全に削除した。ビルトイン
リズム音源(前節)の楽器選択は`ResolvedLayer::forcedCh`(`hwProg`由来)、
OPL系内蔵リズム(`COPLRhythm`、後述)の楽器選択も同じ`forcedCh`機構、
ノート間のチョークは本節の`chokeGroups`と、旧`fixed_ch`が担っていた
3つの役割はすべて専用の仕組みに置き換わった。

### PSG系共有バンク（voicePatchType=0x40固定、2026年7月〜）

SSG(YM2149/AY-3-8910)/EPSG(AY8930)/DCSG(SN76489)/SAA(SAA1099)/
SCC(K051649/K052539)は、実機のハードウェアケーパビリティに大きな差が
ある一方(SSGはHWエンベロープ+ノイズ、SCCは波形メモリ、DCSGは単純な
矩形波のみ等)、音色定義の幅が狭く、1チップ専用のバンクでは名前空間
(CC#0×Bank×Prog)を音楽的に意味のあるバリエーションで埋めることが
難しい。このため、PSG系チップは全て共通の入口(`VOICE_PATCH_SSG`
=0x40固定)/HwBank名前空間を共有する設計に変更した。

「音色データがデバイスを選択する」原則(`docs/chip-driver-architecture.md`
参照)を、**バンク検索**と**デバイス解決**の2段階に分離することで、
名前空間の共有と、正確なデバイス選択(誤ったspanning防止)を両立して
いる。

```
① バンク検索: voicePatchType=VOICE_PATCH_SSG(0x40固定)で共有バンクを
   引く。HwBank::voicePatchTypeも常に0x40("SSG"としてロードされた
   バンク)であるため、既存の厳密一致チェック(bank->voicePatchType
   != voicePatchType)はそのまま機能する(変更不要)。
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

---

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

---

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
→ `voiceGroup = VOICE_GROUP_OPM`（`HwBankRegistry`検索キー）

VoiceGroup（粗い分類、9種）は `HwBankRegistry` の検索キーとして維持し、
データフォーマット・パラメータ範囲の分類に使う。VoicePatchType（27種）は
バンク単位のタグとして、実際に解決されたデバイスとの整合性検証に使う。

---

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

---

## 旧 CFMBank / FMVOICE からの移行

既存の INI バンクファイルは `loadHwBankLegacy()` で読み込み、
内部で `HwPatch::fromLegacy(fmvoice, bank, prog)` に変換して格納する。

`Patch::fromSingleLayer()` は直接モードの単層パッチ構築だけでなく、
旧FMVOICE 1本からのシングルレイヤーパッチ作成にも使われる共用ヘルパー。

---

## 実装過程で発見・修正した既存バグ (2026年7月、SwPatchスキーマ変更時)

SwPatchスキーマ変更(HwPatch単位への参照移行)の実機検証中、既存の
`CSoundDevice::noteOn()`に、CInstCh/CRhythmChが直前に適用した
SwPatchの計算結果を、直後に無条件で上書きしてしまう潜在バグが
あることが判明した。

```cpp
// 修正前(SoundDevImpl.cpp): コメントと実装が矛盾していた
// SwPatch は CInstCh 側が VoiceProcessor に適用済み
s.proc.onNoteOn(s.volume, s.expression, vel, dummy); // dummyはSwPatch無し
```

`CInstCh::noteOn`/`CRhythmCh::applyNoteOn`が`VoiceProcessor::onNoteOn()`
を直接呼んでSwPatchを正しく適用した直後、`dev->noteOn(devCh, vel)`の
内部で`CSoundDevice::noteOn()`が、SwPatchを含まない`dummy`
(FmVoice)で`onNoteOn()`を再度呼んでしまい、直前の計算結果を丸ごと
上書きしていた。これは今回のスキーマ変更以前から存在していた
潜在バグで、旧仕様(Patch単位でのSwPatch共有)でも同様に発生していた
と考えられる(発見が遅れていた)。

**当時の修正**: `ChState`に`pendingSwPatch`フィールドを新設し、
`CInstCh`/`CRhythmCh`はここに適用すべきSwPatchをセットするだけに
留めるよう変更した。実際の`VoiceProcessor::onNoteOn()`呼び出しは
`CSoundDevice::noteOn()`に一本化し、`pendingSwPatch`があればそれを
`dummy`に反映してから呼ぶ。これにより二重呼び出しが解消され、
`CInstCh::noteOn`/`CRhythmCh::applyNoteOn`側のコードも簡潔になった。

**その後の展開**: 下記「`updateVoice()`/`onNoteOn()`呼び出し順序の
根本的欠陥」の修正により、`pendingSwPatch`という遅延設定機構自体が
2026年7月に不要となり削除された(`assignCh()`が`vel`/`swPatch`を
直接受け取れるようになったため)。

---

## `updateVoice()`/`onNoteOn()`呼び出し順序の根本的欠陥 (2026年7月発見・修正)

OPL系内蔵リズムチャンネル(`COPLRhythm`)のSR/EGT制御(パーカッシブ/
サステインモード切替)を検証する過程で、より根本的な、システム全体に
影響する欠陥を発見した。

### 発見の経緯

`COPLRhythm::writeOperatorRegs`が、キャリアオペレータ側で`SR`
(Sustain Rate)フィールドを無視し、常に`proc.velRR(0)`(ベロシティ
補正されたRR)だけを使っていた不具合を修正する過程で、修正後も
実機テストで正しい値(`ar4(velSR)`)が反映されないことが判明した。
調査の結果、`proc.velSR(0)`自体が常に`0`(未計算)を返していることが
分かった。

### 根本原因

`assignCh()`は`updateVoice()`を、`VoiceProcessor::onNoteOn()`
(ベロシティ補正値`velAR`/`velDR`/`velSL`/`velRR`/`velSR`の計算)が
呼ばれる**前**に実行していた。

```
allocChWithFallback（velを引数に持たない）
  → assignCh → updateVoice（★ここでcarrier側ベロシティ補正値を参照するが、まだ計算前）
→ setVolume/setExpression/setSustain/setPanpot（ChState更新のみ）
→ setNoteFine
→ dev->noteOn(devCh, vel)
    → s.proc.onNoteOn(...)（★ここで初めてvelAR/velDR/velSL/velRR/velSRが計算される）
    → updateVolExp（TLのみ再書き込み）
    → updateKey（キーオンビットのみ）
```

`updateKey`は純粋仮想関数で、各チップドライバの実装はキーオンビット
制御のみを行い、AR/DR/SL/RR等の再書き込みは一切行わない
(`COPLLRhythm::updateKey`のような、キーオン/キーオフごとの動的な
EGT/RR切り替えを持つ一部の例外を除く)。そのため、**`updateVoice`内で
`carrier`側が参照するベロシティ補正値は、常に未計算(デフォルト0)の
まま実機へ送信されていた**。これは`COPLRhythm`だけでなく、`COPL`/
`COPN`を含む、ほぼ全てのチップドライバに共通する欠陥だった
(`COPN`も`car ? s.proc.velAR(op) : ...`という同型のコードを持つ)。

`docs/voice-data-design.md`の「フェーズ6（チップドライバ移行）の
具体的な手順」には、当初から正しい設計が明記されていた
(`3. SetVoice() で proc_[ch].onNoteOn(...) を呼ぶように変更`)。
しかし実際の実装では、`onNoteOn`が`SetVoice`相当の`assignCh`ではなく、
別の`noteOn`関数(`assignCh`の"後"に呼ばれる)に実装されており、
ドキュメント化された正しい設計から逸脱していた。

### 修正

1. `ISoundDevice::allocCh`/`assignCh`に`vel`(ベロシティ)と`swPatch`
   (パフォーマンスパッチ、直接受け渡し)を引数として追加。
2. `CSoundDevice::assignCh()`内、`updateVoice()`を呼ぶ**前**に
   `s.proc.onNoteOn(...)`を呼ぶよう変更(ドキュメント通りの設計に復元)。
3. `CSoundDevice::noteOn()`からは、重複していた`onNoteOn()`呼び出しを
   削除。
4. `pendingSwPatch`機構(前節参照)を削除し、`swPatch`は`assignCh`に
   直接渡すよう単純化。
5. `MidiCh.cpp`の全呼び出し元(`allocChWithFallback`、`CInstCh::noteOn`、
   `CRhythmCh::applyNoteOn`)、および`MultiDevice.h`の`CSpanDevice`/
   `CUnison`(複数チップ束ね)を、新しいシグネチャに合わせて更新。

### レガート/ポルタメントへの影響

無し。レガートは`assignCh`を経由せず、既存の割り当て済みチャンネルに
対して`setVelocity()`(TLのみ更新、`onNoteOn`は呼ばない)を呼ぶだけで
あり、今回の修正対象外。ポルタメントもピッチ(Fnumber)の計算・反映
タイミングのみに関わる、独立した機能であり影響しない。

### 検証

`COPN`・`COPLRhythm`双方で、`noteOn`後に`velAR`等が正しく計算済みの
値になることを実機テストで確認した(修正前は常に`0`)。

## 既知の技術的負債 (2026年7月時点)

- `PatchManager::loadDrumBankJson()`(`Config.cpp`からは呼ばれておらず、
  実際に使われているのは`loadDrumKitJson()`)はデッドコードの可能性が
  高い。今回のSwPatchスキーマ変更(`sw_bank`/`sw_prog`追加)は、実際に
  使われている`loadDrumKitJson()`にのみ適用した。`loadDrumBankJson()`
  自体の要否は別途確認が必要。
