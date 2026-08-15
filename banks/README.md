# Preset Banks (フォーマット例示用)

**このディレクトリは `hwbank.json` / `swbank.json` のフォーマット例示専用です。**
演奏に使う実際のバンクセットはこのリポジトリでは管理しません(ステージング
リポジトリ側が正)。ここに置くのはチップ族ごとに1ファイルずつの代表例だけで、
バンクの追加・更新をこのリポジトリへ持ち込む必要はありません。

`config/profiles/` のプロファイルも同じ方針で、フォーマットと配線の例示が目的です。

## ディレクトリ構成

```
banks/
├── OPN/gm/necopn_gm.hwbank.json          128音色 GM配列 (N88-BASIC/OPNA driver)
├── OPM/fb01/rom1.hwbank.json             16音色 (FB-01 ROM ダンプ変換)
├── OPZ/tx81z/tx81z_1.hwbank.json         32音色 (TX81Z VMEM SysEx 変換、OPZ波形拡張付き)
├── OPL2/alsa/std_opl2.hwbank.json        128音色 (ALSA sbiload std.sb)
├── OPL3/alsa/std_opl3.hwbank.json        128音色 (ALSA sbiload std.o3)
├── drums/OPL2/alsa_drums.hwbank.json     ドラムバンク (MIDI note番号がprog番号)
└── sw/default_32.swbank.json             32音色バンク向けSwBank (sw/README.md参照)
```

## プロファイルからの参照方法

`*.profile.json` の `banks` セクションでバンクファイルを指定します。
パスはプロファイルファイルからの相対パスまたは絶対パスで指定します。

```json
{
  "banks": {
    "hw_banks": [
      { "group": "OPM", "bank": 0, "file": "banks/OPM/fb01/rom1.hwbank.json" },
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

### bank_overrides による部分上書き・追加(2026年7月〜)

`banks`(共通プリセットバンクセット、外部参照でもインライン直書きでも可)
に対し、プロファイルごとに一部のバンクだけを差し替えたり追加したりしたい
場合は、`banks` と同じレベル(トップレベル)に `bank_overrides` を置きます。
スキーマは `banks` と全く同一(オブジェクト直書き、または外部参照ファイル
パスの文字列のいずれか)です。

各配列の要素は、以下の「識別キー」が一致すれば`banks`側のエントリを置き
換え、一致しなければ配列末尾へ追加されます(削除はできません)。

| 配列 | 識別キー |
|---|---|
| `hw_banks` | `group` + `bank` (バンク番号はチップ族ごとに独立した名前空間のため) |
| `drum_banks` | `prog` (ドラムバンクは`bank`を持たず常に固定バンク0) |
| `pcm_banks` | `bank` + `chip` (同一bank番号を複数の物理チップ向けに使い分けられるため) |
| `sf2_banks` / `sw_banks` / `patch_banks` / `scc_wave_banks` | `bank` |

```json
{
  "banks": "common.bankset.json",
  "bank_overrides": {
    "hw_banks": [
      { "group": "OPN", "bank": 0, "file": "profile_specific/opn_bank0_variant.hwbank.json" }
    ]
  }
}
```

上記の例では、`common.bankset.json` に定義された `OPN`/`bank 0` だけを
このプロファイル専用のファイルへ差し替え、他のバンク(`OPM`等)は
共通セットのまま使われます。デバイス構成に含まれないチップ向けの
バンクエントリは単に発音しないだけなので、デバイス構成が異なる複数の
プロファイル間でも `banks`/`bank_overrides` の組み合わせをそのまま
使い回せます。

パッチバンク構成(bank/prog番号の割り当て)はデバイス構成
(`devices`/`hw_plugins`)に一切依存しないため、デバイス構成が異なる複数の
プロファイル間で同じバンク構成ファイルを共有できます。デバイス構成に
含まれないチップ向けのバンクエントリは単に発音しないだけであり、一部の
デバイス構成が変わってもbank/prog番号の一貫性は保たれます。

## 例示ファイル一覧

| グループ | ファイル | 音色数 | 出典 |
|---|---|---|---|
| OPN  | `OPN/gm/necopn_gm.hwbank.json`       | 128 | N88-BASIC OPNA driver preset |
| OPM  | `OPM/fb01/rom1.hwbank.json`          | 16  | FB-01 ROM ダンプ |
| OPZ  | `OPZ/tx81z/tx81z_1.hwbank.json`      | 32  | TX81Z (波形拡張付き) |
| OPL2 | `OPL2/alsa/std_opl2.hwbank.json`     | 128 | ALSA sbiload std.sb |
| OPL3 | `OPL3/alsa/std_opl3.hwbank.json`     | 128 | ALSA sbiload std.o3 |
| drums| `drums/OPL2/alsa_drums.hwbank.json`  | -   | ALSA std drums |
