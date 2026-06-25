# パッチ構造設計書

## 概要: 3層分離

```
PatchBank (*.patchbank.json)
  └── Patch [prog 0..127]
        ├── ToneLayer [0..3]          ← 発音レイヤー（最大4チップ）
        │   ├── device_index          → profile devices[] を指す
        │   ├── hw_bank / hw_prog     → HwPatch への参照キー
        │   └── note_range / transpose / volume_offset / pan_offset
        ├── sw_bank / sw_prog         → SwPatch への参照キー（全レイヤー共通）
        └── poly
```

```
HwBank (*.hwbank.json)  ← チップ族ごとに独立
  └── HwPatch [prog 0..127]
        ├── FmHwVoice hw    (FB / ALG / AMS / PMS / NFQ)
        ├── FmHwOp hwOp[4]  (AR/DR/SL/.../TL/KSR/.../MUL/DT1/DT2/AM/VIB/EGT/WS)
        └── FmChipExt ext   (REV / EGS / DM0 / DT3 — OPZ 等のみ)
```

```
SwBank (*.swbank.json)  ← チップ族共通・1セット
  └── SwPatch [prog 0..127]
        ├── FmSwVoice sw    (チャンネルLFO / ビブラート)
        └── FmSwOp swOp[4]  (ベロシティ感度 / トレモロLFO)
```

---

## ファイル対応と MIDI バンクセレクト

| MIDI CC | 値 | 意味 |
|---|---|---|
| CC#0 (MSB) | 0..127 | HwBank 番号 |
| CC#32 (LSB) | 0..127 | SwBank 番号 |
| Program Change | 0..127 | Patch 番号（PatchBank 内） |

PatchBank ファイルは HwBank / SwBank とは独立して管理する。
「どの HwPatch と SwPatch を組み合わせてパッチを構成するか」は
PatchBank 側に記述する。

---

## ToneLayer によるマルチチップ発音

1パッチに最大4つの ToneLayer を持たせることができる。

```json
{
  "prog": 1,
  "name": "Strings 2 (Split: OPM + DCSG bass)",
  "layers": [
    {
      "device_index": 0,
      "hw_bank": 0, "hw_prog": 1,
      "note_range_lo": 48, "note_range_hi": 127,
      "transpose": 0, "enabled": true
    },
    {
      "device_index": 2,
      "hw_bank": 0, "hw_prog": 0,
      "note_range_lo": 0, "note_range_hi": 47,
      "transpose": -12, "enabled": true
    }
  ]
}
```

この例では:
- device 0 (OPM): ノート 48〜127 を担当
- device 2 (DCSG): ノート 0〜47 を担当、1オクターブ下げて発音

---

## HwPatch と device_type の照合

ToneLayer は `device_index` のみを持ち、チップ族 (VoiceGroup) を直接指定しない。
`ProgChange` 処理時に `device_index → device_type → VoiceGroup` を解決し、
適切な HwBank からHwPatch を引く。

```
ToneLayer.device_index = 0
  → devices[0].device_type = DEVICE_OPM
  → VoiceGroup = VOICE_GROUP_OPM
  → HwBankRegistry.resolve(VOICE_GROUP_OPM, hw_bank, hw_prog)
  → OPM 用の HwPatch を返す
```

同一パッチで OPM と OPN の両方の HwPatch を用意することで、
接続されたデバイス種別に応じて適切な音色パラメータが自動的に選択される。

---

## CInstCh::ProgChange の変更

旧実装:
```cpp
// 旧: 単一FMVOICE、単一デバイス
void CInstCh::ProgChange(uint8_t prog) {
    Parent->GetVoice(&voice, Device->GetDevice(), BankSelL, prog);
    Device->SetVoice(note_ch, &voice);
}
```

新実装（移行後）:
```cpp
void CInstCh::ProgChange(uint8_t prog) {
    // PatchManager で Patch → ResolvedPatch を構築
    auto resolved = patchMgr_.resolve(patchBankNo_, prog, *config_);
    patchResolver_.apply(resolved);

    // 現在発音中のノートに新しい HwPatch を適用
    for (int i = 0; i < patchResolver_.layerCount(); ++i) {
        const auto* rl = patchResolver_.layer(i);
        if (!rl || !rl->hwPatch) continue;
        auto* dev = config_->getDevicePort(rl->deviceIndex);
        if (dev) dev->SetVoice(noteChannel, *rl->hwPatch);
    }
}
```

---

## NoteOn の変更

旧実装:
```cpp
// 旧: 単一デバイスに単一ノート
void CInstCh::NoteOn(uint8_t note, uint8_t vel) {
    uint8_t ch = Device->AllocCh(this, &voice);
    Device->NoteOn(ch, vel);
}
```

新実装（移行後）:
```cpp
void CInstCh::NoteOn(uint8_t note, uint8_t vel) {
    for (int li = 0; li < patchResolver_.layerCount(); ++li) {
        const auto* rl = patchResolver_.layer(li);
        if (!rl || !rl->layer->inRange(note)) continue;

        int transposed = rl->layer->transposedNote(note);
        if (transposed < 0 || transposed > 127) continue;

        auto* dev = getDevice(rl->deviceIndex);
        if (!dev) continue;

        // ベロシティ・音量オフセット適用
        uint8_t adjVel = adjustVelocity(vel, rl->layer->volumeOffset);
        uint8_t ch = dev->AllocCh(this, rl->hwPatch);
        dev->NoteOn(ch, adjVel);

        // ノート履歴に (layerIndex, devCh) を記録
        enterNote(li, ch, static_cast<uint8_t>(transposed));
    }
}
```

---

## バンクファイル管理の推奨ディレクトリ構成

```
banks/
  hw/
    opm/
      00_default.hwbank.json
      01_strings.hwbank.json
    opn/
      00_default.hwbank.json
    opl2/
      00_default.hwbank.json
    psg/
      00_default.hwbank.json
  sw/
    00_default.swbank.json
    01_vibrato.swbank.json
  patches/
    00_general.patchbank.json
    01_special.patchbank.json
```

profile の `banks` セクションで各バンクファイルのパスとバンク番号を指定する。

```json
"banks": {
  "hw_banks": [
    { "group": "OPM",  "bank": 0, "file": "banks/hw/opm/00_default.hwbank.json" },
    { "group": "OPN",  "bank": 0, "file": "banks/hw/opn/00_default.hwbank.json" },
    { "group": "PSG",  "bank": 0, "file": "banks/hw/psg/00_default.hwbank.json" }
  ],
  "sw_banks": [
    { "bank": 0, "file": "banks/sw/00_default.swbank.json" }
  ],
  "patch_banks": [
    { "bank": 0, "file": "banks/patches/00_general.patchbank.json" }
  ]
}
```

---

## 旧 CFMBank / FMVOICE からの移行

既存の INI バンクファイルは `loadHwBankLegacy()` で読み込み、
内部で `HwPatch::fromLegacy(fmvoice, bank, prog)` に変換して格納する。
変換後は JSON 形式で書き出すことで永続化できる。

```
移行ツール (fitom_convert_bank):
  旧 .ini バンク → HwPatch JSON + SwPatch JSON (デフォルト値)
  ユーザーが SwPatch のベロシティ感度・LFO を後から設定する
```

旧 FMVOICE の `SLF/SLW/SLD/SLY/SLR/VTL...VLR` フィールドは
SwPatch の対応フィールドに変換される。
