# Preset Banks (フォーマット例示用)

**このディレクトリは、バンクファイルの書式を示す例示専用です。**
演奏に使う実際のバンクセットはこのリポジトリでは管理しません(ステージング
リポジトリ側が正)。ここに置くのはチップ族ごとに1ファイルずつの代表例だけで、
バンクの追加・更新をこのリポジトリへ持ち込む必要はありません。

`config/profiles/` のプロファイルも同じ方針で、フォーマットと配線の例示が目的です。

バンクファイルの書き方は`docs/manuals/bank-reference.md`、プロファイルからの読み込み方は`docs/manuals/profile-reference.md`を参照してください。

## 例示ファイル一覧

| グループ | ファイル | 音色数 | 出典 |
|---|---|---|---|
| OPN  | `OPN/gm/necopn_gm.hwbank.json`       | 128 | N88-BASIC OPNA driver preset |
| OPM  | `OPM/fb01/rom1.hwbank.json`          | 16  | FB-01 ROM ダンプ |
| OPZ  | `OPZ/tx81z/tx81z_1.hwbank.json`      | 32  | TX81Z VMEM SysEx 変換 (OPZ波形拡張付き) |
| OPL2 | `OPL2/alsa/std_opl2.hwbank.json`     | 128 | ALSA sbiload std.sb |
| OPL3 | `OPL3/alsa/std_opl3.hwbank.json`     | 128 | ALSA sbiload std.o3 |
| drums| `drums/OPL2/alsa_drums.hwbank.json`  | 128 | ALSA std drums (MIDIノート番号がprog番号) |
| SwBank | `sw/default_32.swbank.json`        | 32  | 32音色バンク向けの演奏特性 (VTL=64) |

`alsa_drums.hwbank.json` はドラム音色を集めた通常の音色バンクで、`hw_banks` に登録して使います。
ドラムチャンネルで鳴らす音色の割り当ては、`drum_banks` に1プログラム1ファイルで
ドラムキット(`*.drumkit.json`)を並べて行います(書式の例は
`config/profiles/opnAdpcm.drumkit.json`)。

プロファイルからの参照の例:

```json
{
  "banks": {
    "hw_banks": [
      { "group": "OPM", "bank": 0, "file": "banks/OPM/fb01/rom1.hwbank.json" },
      { "group": "OPZ", "bank": 0, "file": "banks/OPZ/tx81z/tx81z_1.hwbank.json" },
      { "group": "OPN", "bank": 0, "file": "banks/OPN/gm/necopn_gm.hwbank.json" },
      { "group": "OPL2","bank": 0, "file": "banks/OPL2/alsa/std_opl2.hwbank.json" },
      { "group": "OPL3","bank": 0, "file": "banks/OPL3/alsa/std_opl3.hwbank.json" },
      { "group": "OPL2","bank": 1, "file": "banks/drums/OPL2/alsa_drums.hwbank.json" }
    ],
    "sw_banks": [
      { "bank": 0, "file": "banks/sw/default_32.swbank.json" }
    ]
  }
}
```

`file`の相対パスは、プロファイルファイルが置かれたディレクトリを起点に解決されます。
