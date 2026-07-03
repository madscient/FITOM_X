# Voice Conversion Tools

音色データを各シンセサイザーのフォーマットから FITOM_X の `hwbank.json` 形式に変換するツール群です。

## 必要環境

Python 3.8 以上 (標準ライブラリのみ使用)

## 変換ツール一覧

### `necopn_convert.py` — OPN グループ

**対応フォーマット**: N88-BASIC(86) OPNA/OPN ドライバ音色データ (`necopn.bin`)

```bash
python3 necopn_convert.py necopn.bin output.hwbank.json --bank 0 --group OPN
```

**フォーマット概要**:
- 128音色 × 64バイト (実データ50バイト + パディング14バイト)
- 各OP 6バイト: DT1/MUL, TL, KSR/AR, AM/DR, SR, SL/RR
- 格納順: M1(OP1) → C1(OP2) → M2(OP3) → C2(OP4) → FB/ALG → AMS/FMS
- 変換後 ops[] 順: [M1, C1, M2, C2]

---

### `vma_convert.py` — OPL2 / OPL3 グループ

**対応フォーマット**: MA-2 VMA ファイル (`*.vma`)

```bash
python3 vma_convert.py input.vma output.hwbank.json [--bank 0]
python3 vma_convert.py /path/to/vma/dir/ /path/to/out/ [--bank 0]
```

**フォーマット概要**:
- マジック `FM  ` + サイズ(big-endian) + 名前部(N×16B) + パラメータ部(N×26B)
- N=128(メロディ) or N=79(ドラム, MIDI note 27-105)
- byte1=0 → 2OP(OPL2), byte1=1 → 4OP(OPL3)
- パラメータはMA-2レジスタ形式(ビット位置がOPLと異なる)
- `ADP  ` マジックの ADPCM ファイルはスキップ

---

### `alsa_convert.py` — OPL2 / OPL3 グループ

**対応フォーマット**: ALSA sbiload 音色バンク (`.sb`, `.o3`)

```bash
python3 alsa_convert.py std.sb std_opl2.hwbank.json
python3 alsa_convert.py std.o3 std_opl3.hwbank.json
python3 alsa_convert.py /path/to/dir/ /path/to/out/
```

**フォーマット概要**:
- `.sb` (OPL2): 128音色 × 52バイト, マジック `SBI\x1A` or `2OP\x1A`
- `.o3` (OPL3): 128音色 × 60バイト, マジック `4OP\x1A`、2OP×2の構造
- パラメータはOPLレジスタ直接値(変換不要)
- ドラムバンク: プログラム番号 = MIDI ノート番号 (35-81 に実データ)
- SBTimbre 拡張 (PercVoc, Transpos, PercPitch) をソフトパラメータとして保存

---

### `vmem_convert.py` — OPM グループ

**対応フォーマット**: DX27 / DX100 VMEM 32-Voice SysEx (`.syx`)

```bash
python3 vmem_convert.py input.syx output.hwbank.json [--bank 0]
python3 vmem_convert.py /path/to/syx/ /path/to/out/
```

**SysEx フォーマット**: `F0 43 0n 04 20 00 [4096バイト] CS F7`

**VMEM 1音色 (128バイト)**:
- P0-9: OP4 → M1 → ops[0]
- P10-19: OP2 → C1 → ops[1]
- P20-29: OP3 → M2 → ops[2]
- P30-39: OP1 → C2 → ops[3]
- P40-72: 共通パラメータ / 音色名 (10文字)

**主要変換**:

| VMEM パラメータ | 範囲 | OPM レジスタ | 変換式 |
|---|---|---|---|
| OUTPUT LEVEL | 0-99 | TL (0-127) | `127 - round(OL×127/99)` |
| FREQUENCY COARSE | P8[5:2] | MUL (0-15) | 直接 |
| DT2 | P8[1:0] | DT2 (0-3) | 直接 |
| DETUNE | 0-14 (中央7) | DT1 (0-7) | 差分→OPM符号付き変換 |

---

### `fb01_convert.py` — OPM グループ

**対応フォーマット**: Yamaha FB-01 ROM ダンプ (`.dmp`)

```bash
python3 fb01_convert.py rom1.dmp rom1.hwbank.json [--bank 0]
python3 fb01_convert.py /path/to/dmp/ /path/to/out/
```

**ファイル構造**: 32バイトヘッダ + 64スロット × 48バイト

**OP順序**: `OP#0(M1)→ops[0], OP#2(C1)→ops[1], OP#1(M2)→ops[2], OP#3(C2)→ops[3]`
(OP#1 と OP#2 が入れ替わる点に注意)

---

### `tx81z_convert.py` — OPZ グループ (OPM 互換拡張)

**対応フォーマット**: Yamaha TX81Z 32-Voice VMEM SysEx (`.syx`)

```bash
python3 tx81z_convert.py input.syx output.hwbank.json [--bank 0]
python3 tx81z_convert.py /path/to/syx/ /path/to/out/
```

**SysEx フォーマット**: `F0 43 0n 04 [SH] [SL] [4096バイト] CS F7`
サイズは MIDI 7bit エンコード: `(SH<<7)|SL = 4096`

**TX81Z 固有拡張 (ACED)**:

| パラメータ | 意味 | FITOM_X フィールド |
|---|---|---|
| OPW | 波形選択 0-7 | `WS` |
| FIX | 固定周波数モード | `FIX` |
| FIXRG | 固定周波数レンジ 0-7 | `FIXRG` |
| FINE | 微調整 0-15 | `FINE` |
| EGSFT | EG シフト 0-7 | `EGSFT` |

---

## 共通仕様: ops[] の格納順

全グループで ops[] は **[M1, C1, M2, C2]** 順に統一されています。

```
ops[0] = M1 (Modulator 1 / Operator 1)
ops[1] = C1 (Carrier 1   / Operator 2)
ops[2] = M2 (Modulator 2 / Operator 3)
ops[3] = C2 (Carrier 2   / Operator 4)  ← 2OP グループでは使用しない
```

## 出力フォーマット: hwbank.json

```json
{
  "name":     "バンク名",
  "group":    "OPN|OPM|OPZ|OPL2|OPL3",
  "bank":     0,
  "op_count": 2 または 4,
  "source":   "元ファイル名とフォーマット",
  "patches": [
    {
      "prog": 0,
      "name": "音色名",
      "hw":   { "ALG": 2, "FB": 6, ... },
      "ops":  [ { "AR":31, "D1R":8, "TL":0, "MUL":1, ... }, ... ]
    }
  ]
}
```

---

### `opl2_merge.py` — OPL2→OPL3 バンク合成

**用途**: 2つの OPL2 (2OP) hwbank.json を組み合わせて OPL3 (4OP) バンクを生成します。

```bash
# 基本: MicroComputer (M1/C1) + Digital (M2/C2) → CON4=1 (独立並列)
python3 opl2_merge.py \\
    banks/OPL2/ma2_vma/MicroComputerNormalBank.hwbank.json \\
    banks/OPL2/ma2_vma/DigitalNormalBank.hwbank.json \\
    banks/OPL3/MicroComputer_x_Digital.hwbank.json

# CON4=0 (完全直列) で合成
python3 opl2_merge.py BankA.hwbank.json BankB.hwbank.json out.hwbank.json --con4 0

# ドラムバンク合成も同様
python3 opl2_merge.py \\
    banks/drums/OPL2/BasicDrumBank.hwbank.json \\
    banks/drums/OPL2/DigitalDrumBank.hwbank.json \\
    banks/drums/OPL3/Basic_x_Digital.hwbank.json
```

**CON4 (4OP 接続モード)**:

| CON4 | 接続 | 用途 |
|---|---|---|
| 0 | M1→C1→M2→C2 (完全直列) | 最大変調深度 |
| **1** | **(M1→C1) + (M2→C2) (独立並列)** | **バンク合成推奨・デフォルト** |
| 2 | M1→(C1 + M2→C2) | 部分並列 |
| 3 | M1→C1 + M2 + M2→C2 | 3出力混合 |

**FITOM_X フィールド対応**:

| フィールド | 内容 |
|---|---|
| `hw.FB` | Bank-A の FB (M1/C1ペア、独立3bit) |
| `hw.FB2` | Bank-B の FB (M2/C2ペア、独立3bit) |
| `hw.ALG` | 3bit統合 (bit0=CON1前半ペア接続, bit1=CON2後半ペア接続, bit2=ConnectionSEL) |
| `ops[0]` | Bank-A Mod (M1) |
| `ops[1]` | Bank-A Car (C1) |
| `ops[2]` | Bank-B Mod (M2) |
| `ops[3]` | Bank-B Car (C2) |

実機OPL3は前半・後半ペアそれぞれ独立したFBレジスタを持つため、`FB`/`FB2`は
6bitへのパック無しで、それぞれ独立したフィールドとして格納する
(`core/src/OPL_new.cpp` の COPL3 実装に完全準拠)。

`midi_note` 等のドラム固有フィールドは Bank-A から自動的に引き継がれます。
