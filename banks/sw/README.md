# Software Patch Banks (swbank.json)

SwBank は HwPatch（チップレジスタ直接値）に対するソフトウェアパラメータを定義する。
ベロシティ感度・オペレータ LFO（トレモロ）を格納する。

## ファイル一覧

| ファイル | 音色数 | VTL | 用途 |
|---|---|---|---|
| `default_gm.swbank.json` | 128 | 64 | GM配列バンク向けデフォルト |
| `default_32.swbank.json` | 32  | 64 | 32音色バンク向けデフォルト |
| `compat_zero.swbank.json` | 128 | 0  | 全パラメータ0・旧FITOM完全互換 |

## パラメータ

### ベロシティ感度

| パラメータ | 効果 | 最大補正幅 |
|---|---|---|
| VTL | 低vel → TL 増加（音量減） | hwTL + (127-vel)×VTL/127 |
| VAR | 低vel → AR 減少（遅いアタック） | ±8 (AR レンジ/4) |
| VDR | 低vel → DR 減少（遅いディケイ） | ±8 |
| VSL | 低vel → SL 増加（浅いサステイン） | ±4 (SL レンジ/4) |
| VSR | 低vel → SR 減少 | ±8 |
| VRR | 低vel → RR 減少（長いリリース） | ±4 (RR レンジ/4) |

### プロファイルでの参照

```json
"banks": {
  "hw_banks": [
    { "group": "OPM", "bank": 0, "file": "banks/OPM/dx27_dx100/dx100_1.hwbank.json" }
  ],
  "sw_banks": [
    { "bank": 0, "file": "banks/sw/default_32.swbank.json" }
  ]
}
```

HwPatch の `sw_bank` / `sw_prog` フィールドで対応する SwPatch を参照する。
`sw_bank` 未指定の場合はすべての感度パラメータが 0（無感度）となる。
