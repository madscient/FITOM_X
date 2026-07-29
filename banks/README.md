# Preset Banks

FITOM_X プリセット音色バンク。各ファイルは `hwbank.json` フォーマットです。

## ディレクトリ構成

```
banks/
├── OPN/
│   └── gm/
│       └── necopn_gm.hwbank.json       128音色 GM配列 (N88-BASIC/OPNA driver)
├── OPM/
│   ├── dx27_dx100/                     DX27/DX100 VMEM SysEx 変換
│   │   ├── dx100_1〜8.hwbank.json      各24音色 (DX100 プリセット)
│   │   └── dx21_a〜d.hwbank.json       各32音色 (DX21 プリセット)
│   └── fb01/                           FB-01 ROM ダンプ変換
│       └── rom1〜5.hwbank.json         各16音色
├── OPZ/
│   └── tx81z/                          TX81Z VMEM SysEx 変換 (OPZ波形拡張付き)
│       └── tx81z_1〜4.hwbank.json      各32音色
├── OPL2/
│   ├── alsa/
│   │   └── std_opl2.hwbank.json        128音色 (ALSA sbiload std.sb)
│   └── ma2_vma/                        MA-2 VMA 変換
│       ├── 01〜06_*.hwbank.json        各128音色 GM分類バンク
│       ├── Preset2OP.hwbank.json       128音色
│       └── *NormalBank*.hwbank.json    各128音色 (機種別プリセット)
├── OPL3/
│   ├── alsa/
│   │   └── std_opl3.hwbank.json        128音色 (ALSA sbiload std.o3)
│   └── ma2_vma/
│       ├── GMmapFM4op.hwbank.json      128音色 GM配列 4OP
│       └── Preset4OP.hwbank.json       128音色
└── drums/
    ├── OPL2/                           OPL2 ドラムバンク (MIDI note 番号がprog番号)
    │   ├── alsa_drums.hwbank.json      (ALSA std drums)
    │   └── *DrumBank.hwbank.json       (MA-2 各機種ドラム)
    └── OPL3/
        └── alsa_drums.hwbank.json      (ALSA OPL3 drums)
```

## プロファイルからの参照方法

`*.profile.json` の `banks` セクションでバンクファイルを指定します。
パスはプロファイルファイルからの相対パスまたは絶対パスで指定します。

```json
{
  "banks": {
    "hw_banks": [
      { "group": "OPM", "bank": 0, "file": "banks/OPM/dx27_dx100/dx100_1.hwbank.json" },
      { "group": "OPM", "bank": 1, "file": "banks/OPM/fb01/rom1.hwbank.json" },
      { "group": "OPZ", "bank": 0, "file": "banks/OPZ/tx81z/tx81z_1.hwbank.json" },
      { "group": "OPN", "bank": 0, "file": "banks/OPN/gm/necopn_gm.hwbank.json" },
      { "group": "OPL2","bank": 0, "file": "banks/OPL2/alsa/std_opl2.hwbank.json" },
      { "group": "OPL3","bank": 0, "file": "banks/OPL3/alsa/std_opl3.hwbank.json" }
    ],
    "drum_banks": [
      { "group": "OPL2","bank": 0, "file": "banks/drums/OPL2/alsa_drums.hwbank.json" }
    ]
  }
}
```

### banksセクションの外部ファイル分離(2026年7月〜)

`banks` に文字列を指定すると、外部ファイルへのパスとして扱われます。
参照先JSONオブジェクト(`hw_banks`/`sw_banks`/`patch_banks`/`drum_banks`/
`scc_wave_banks`/`pcm_banks`/`sf2_banks`を持つオブジェクト)が、その位置に
そのまま埋め込まれたものとして読み込まれます(パス解決はプロファイル
ファイル自身のディレクトリが基点、`banks.*[].file`と同じ規則)。

```json
{
  "banks": "bank.profile.json"
}
```

```json
// bank.profile.json (上記から参照される側。中身は banks オブジェクトと同一形式)
{
  "hw_banks": [
    { "group": "OPN", "bank": 0, "file": "banks/OPN/gm/necopn_gm.hwbank.json" }
  ],
  "drum_banks": [
    { "group": "OPL2","bank": 0, "file": "banks/drums/OPL2/alsa_drums.hwbank.json" }
  ]
}
```

パッチバンク構成(bank/prog番号の割り当て)はデバイス構成
(`devices`/`hw_plugins`)に一切依存しないため、デバイス構成が異なる複数の
プロファイル間で同じバンク構成ファイルを共有できます。デバイス構成に
含まれないチップ向けのバンクエントリは単に発音しないだけであり、一部の
デバイス構成が変わってもbank/prog番号の一貫性は保たれます。

## バンク数・音色数サマリー

| グループ | バンク数 | 総音色数 | 主な出典 |
|---|---|---|---|
| OPN  | 1  | 128  | N88-BASIC OPNA driver preset |
| OPM  | 17 | 364  | DX27/DX100, FB-01 |
| OPZ  | 4  | 128  | TX81Z (波形拡張付き) |
| OPL2 | 20 | 2560 | ALSA sbiload, MA-2 VMA |
| OPL3 | 3  | 384  | ALSA sbiload, MA-2 VMA |
| drums(OPL2) | 8 | 376 | ALSA, MA-2 各機種 |
| drums(OPL3) | 1 | 128  | ALSA OPL3 |
