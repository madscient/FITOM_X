# パッチ構造設計書

## 概要: 3層分離 + VoicePatchType による解決

```
PatchBank (*.patchbank.json)
  └── Patch [prog 0..127]
        ├── ToneLayer [0..3]          ← 発音レイヤー（最大4チップ）
        │   ├── voicePatchType        → デバイスタイプID (VOICE_PATCH_*, 0x10-0x54)
        │   ├── hw_bank / hw_prog     → HwPatch への参照キー
        │   └── note_range / transpose / volume_offset / pan_offset
        ├── sw_bank / sw_prog         → SwPatch への参照キー（全レイヤー共通）
        └── poly
```

```
HwBank (*.hwbank.json)  ← VoiceGroup (粗い分類, 9種) ごとに登録
  ├── voicePatchType     ← バンク全体で共通の細分類タグ (整合性検証用)
  └── HwPatch [prog 0..127]
        ├── FmHwVoice hw    (FB / ALG / AMS / PMS / NFQ)
        ├── FmHwOp hwOp[4]  (AR/DR/SL/.../TL/KSR/.../MUL/DT1/DT2/AM/VIB/EGT/WS)
        └── FmChipExt ext   (REV / EGS / DM0 / DT3 / ALG_EXT / HWEP)
```

```
SwBank (*.swbank.json)  ← チップ族共通・1セット
  └── SwPatch [prog 0..127]
        ├── FmSwVoice sw    (チャンネルLFO / ビブラート)
        └── FmSwOp swOp[4]  (ベロシティ感度 / トレモロLFO)
```

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
| 0x50-54 | ADPCMB/ADPCMA/PCMD8/AWM | Y8950/YM2608/YM2610/YMZ280 等 | PCM |

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
          layer.voicePatchType == VOICE_PATCH_AWM の場合のみ、HwBankでは
          なくSampleZoneBankRegistryを検索し、ResolvedLayer::samplePatch
          に結果を格納する（HwPatchとは別のスキーマ、詳細は後述）
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
  → PatchBank/ToneLayer/SwPatchを経由せず、単層Patchを直接構築
  → SwPatch は常に nullptr（ベロシティ感度・ソフトLFOは無効）
  → Patch::fromSingleLayer() を使用（ノート範囲フル、transpose/offsetなし）
```

`VOICE_PATCH_*`の値は全て`0x01`-`0x6F`の範囲に収まるよう採番されている
(`0x70`-`0x7F`はGM2リズム/メロディ切替(`0x78`/`0x79`)専用および将来予約)。
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
                patchProg, config, storage)で解決 (単層Patch、SwPatchなし)
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
