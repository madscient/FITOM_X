# パッチ構造設計書

## 概要: 3層分離 + VoicePatchType による解決

```
PatchBank (*.patchbank.json)
  └── Patch [prog 0..127]
        ├── ToneLayer [0..3]          ← 発音レイヤー（最大4チップ）
        │   ├── voicePatchType        → デバイスタイプID (VOICE_PATCH_*, 0x10-0x74)
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
`FITOMdefine.h` に `VOICE_PATCH_*` として定義されている（0x10〜0x74、詳細は後述）。

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
| 0x28 | OPLL | YM2413, YM2420 | OPLL |
| 0x29 | OPLLP | YMF281 | OPLL |
| 0x2a | OPLLX | YM2423 | OPLL |
| 0x2b | VRC7 | FS1001 (OPLLからリズムCH削除、楽音6chのみ) | OPLL |
| 0x30 | OPL3 | YMF264/289/278-4OP | OPL3 |
| 0x38-3b | SD-1/MA-3/MA-5/MA-7 | YMF825 等 | MA3（未実装、値のみ予約） |
| 0x40 | SSG | YM2149, AY-3-8910 | PSG(無波形) |
| 0x41 | AY8930 | — | PSG(無波形) |
| 0x42 | DCSG | SN76489 | PSG(無波形) |
| 0x43 | SAA1099 | — | PSG(無波形、未実装・値のみ予約) |
| 0x48 | SCC | SCC, SCCP | PSG(波形ROM) |
| 0x70-74 | ADPCMB/ADPCMA/PCMD8/AWM | Y8950/YM2608/YM2610/YMZ280 等 | PCM |

パン対応可否は VoicePatchType 単位でも一律ではない（例: 同じPSG無波形グループでも
SAA1099は per-channel ステレオパン対応、SSG/AY8930/DCSGは非対応）。
**このためFITOM_Xはパン対応可否を内部で判定・管理しない。**
パンを使いたい場合はエンドユーザーが対応デバイスのVoicePatchTypeを
自己責任で選択する（バンクセレクトLSB直接指定モード参照）。

---

## 2つの解決モード

### 通常モード（バンクセレクトLSB=0）: PatchBank/Patch/ToneLayer による多層解決

```
Program Change 受信
  → PatchManager::resolve(bankSelM_, prog, config)
      → PatchBank[bankSelM_].patches[prog] を取得
      → 各 ToneLayer について:
          Config::findDeviceIndexByVoicePatchType(layer.voicePatchType)
            → 一致するデバイスを devices[] から線形探索（完全一致のみ）
          見つからなければそのレイヤーだけスキップ（Patch全体は無効にしない）
          見つかれば HwBank から layer.hwBank/hwProg で HwPatch を解決
            → バンクの voicePatchType タグと layer.voicePatchType が
              不一致なら同様にスキップ
```

**Program Change は発音中のノートに一切作用しない。**
新しいパッチは次の NoteOn から適用される（全モード共通の仕様）。

### 直接モード（バンクセレクトLSB>0）: HwBank直接指定

```
bankSelL_ (CC#32) > 0 のとき:
  voicePatchType = bankSelL_        （CC#32の値そのものがVOICE_PATCH_*定数）
  hwBank         = bankSelM_        （CC#0の値）
  hwProg         = prog             （Program Changeの値）

PatchManager::resolveDirect(voicePatchType, hwBank, hwProg, config, storage)
  → PatchBank/ToneLayer/SwPatchを経由せず、単層Patchを直接構築
  → SwPatch は常に nullptr（ベロシティ感度・ソフトLFOは無効）
  → Patch::fromSingleLayer() を使用（ノート範囲フル、transpose/offsetなし）
```

`bankSelL_==0`は予約値で、直接モードのトリガーには使えない
（`VOICE_PATCH_NONE=0`、`DEVICE_NONE=0`と対応）。

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
