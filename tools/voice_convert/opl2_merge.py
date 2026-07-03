#!/usr/bin/env python3
"""
OPL2 (2OP) バンク 2本 → OPL3 (4OP) バンク 1本 合成ツール

2つの OPL2 hwbank.json を組み合わせて OPL3 の 4OP 音色を生成します。
Bank-A の Mod/Car (M1/C1) と Bank-B の Mod/Car (M2/C2) を
指定した CON4 (4OP 接続モード) と FB でひとつの 4OP バンクに合成します。

使用例:
  python3 opl2_merge.py \\
      MicroComputerNormalBank.hwbank.json \\
      DigitalNormalBank.hwbank.json \\
      MicroComputer_x_Digital.hwbank.json \\
      --con4 1 --fb-mode per-patch

OPL3 4OP 接続モード (CON4):
  0: M1→C1→M2→C2  完全直列 (最大変調深度)
  1: (M1→C1) + (M2→C2)  2OP×2 独立並列 ← バンク合成時の推奨
  2: M1→(C1 + M2→C2)  C1と第2ペアが並列合流
  3: M1→C1 + M2 + M2→C2  3出力混合

FITOM_X hw フィールド対応:
  hw.ALG   : 3bit統合 (bit0=CON1前半ペア接続, bit1=CON2後半ペア接続,
             bit2=ConnectionSEL(常に1、4OPモード有効化))
  hw.FB    : 1stペア(M1C1)独立FB (Bank-A から取得)
  hw.FB2   : 2ndペア(M2C2)独立FB (Bank-B から取得)
  (実機OPL3は前半・後半ペアそれぞれ独立したFBレジスタを持つため、
   FBを6bitに詰め込む旧方式ではなく、hw.FB/hw.FB2の2フィールドに
   素直に対応させる。core/src/OPL_new.cpp のCOPL3実装に完全準拠)
"""

import json, argparse, sys
from pathlib import Path

# -----------------------------------------------------------------------
# デフォルト CON4 設定
# CON4=1: (M1→C1) + (M2→C2) 独立並列 → 2バンク合成時に最も自然
DEFAULT_CON4 = 1

def load_opl2_bank(path):
    d = json.load(open(path))
    if d.get('op_count', 2) != 2:
        raise ValueError(f"{path}: OPL2 (op_count=2) バンクが必要です (got op_count={d.get('op_count')})")
    if d.get('group') not in ('OPL2', 'OPL3'):
        raise ValueError(f"{path}: OPL2 グループバンクが必要です (got group={d.get('group')})")
    return d

def merge_banks(bank_a, bank_b, con4, alg_a, alg_b, bank_no, src_name):
    """
    bank_a (M1/C1ペア) と bank_b (M2/C2ペア) を合成する。

    プログラム番号の対応:
      両バンクの同一プログラム番号を対応させる。
      一方のバンクにプログラムが存在しない場合はスキップ。
    """
    patches_a = {p['prog']: p for p in bank_a['patches']}
    patches_b = {p['prog']: p for p in bank_b['patches']}

    progs = sorted(set(patches_a.keys()) & set(patches_b.keys()))
    if not progs:
        raise ValueError("共通するプログラム番号がありません")

    patches_out = []
    for prog in progs:
        pa = patches_a[prog]
        pb = patches_b[prog]

        hw_a = pa['hw']
        hw_b = pb['hw']
        ops_a = pa['ops']  # [Mod, Car]
        ops_b = pb['ops']  # [Mod, Car]

        fb_a = hw_a.get('FB', 0)   # 1stペア FB (Bank-A)
        fb_b = hw_b.get('FB', 0)   # 2ndペア FB (Bank-B)

        # 音色名: A×B または A のみ
        name_a = pa.get('name', f'Prog{prog}')
        name_b = pb.get('name', f'Prog{prog}')
        name = f"{name_a[:8].rstrip()}x{name_b[:8].rstrip()}" if name_a != name_b \
               else name_a

        # hw.ALG(3bit): bit0=CON1(前半ペア接続), bit1=CON2(後半ペア接続),
        #               bit2=ConnectionSEL(4OP結合有効化、常に1)
        # (COPL3実装 core/src/OPL_new.cpp に完全準拠。旧仕様のFB 6bitパック
        #  方式は、実機OPL3が前半/後半ペアそれぞれ独立したFBレジスタを
        #  持つことが判明したため廃止し、hw.FB/hw.FB2に分離した)
        cnt0 = alg_a & 1
        cnt1 = alg_b & 1
        alg_enc = (1 << 2) | (cnt1 << 1) | cnt0

        patch = {
            "prog": prog,
            "name": name[:16],
            "hw": {
                "ALG": alg_enc,  # (conn_sel<<2)|(cnt1<<1)|cnt0
                "FB":  fb_a & 7, # 前半ペア(M1/C1)独立FB
                "FB2": fb_b & 7, # 後半ペア(M2/C2)独立FB
            },
            "ops": [
                ops_a[0],        # ops[0] = M1 (Bank-A の Mod)
                ops_a[1],        # ops[1] = C1 (Bank-A の Car)
                ops_b[0],        # ops[2] = M2 (Bank-B の Mod)
                ops_b[1],        # ops[3] = C2 (Bank-B の Car)
            ],
            # ソフトパラメータは Bank-A を優先
            "sw": {**pb.get('sw', {}), **pa.get('sw', {})},
        }
        # midi_note / perc_voc 等のドラム固有フィールドを引き継ぐ
        for key in ('midi_note', 'perc_voc', 'perc_pitch', 'transpose', 'slot'):
            if key in pa:
                patch[key] = pa[key]
        patches_out.append(patch)

    return patches_out

def run(args):
    bank_a = load_opl2_bank(args.bank_a)
    bank_b = load_opl2_bank(args.bank_b)

    name_a  = Path(args.bank_a).stem
    name_b  = Path(args.bank_b).stem
    src_name = args.name or f"{name_a} x {name_b}"

    # alg_a: 1stペア内接続 (デフォルト 0=FM、Bank-A のALGを引き継ぐか?)
    # ユーザー指定がなければ Bank-A の ALG を使う
    # ただし OPL2 の ALG は0か1なのでそのまま使える
    alg_a_default = bank_a['patches'][0]['hw'].get('ALG', 0) if bank_a['patches'] else 0
    alg_a = args.alg_a if args.alg_a is not None else alg_a_default

    # 2ndペア (M2/C2) の内部接続: デフォルトは Bank-B の ALG を引き継ぐ
    alg_b_default = bank_b['patches'][0]['hw'].get('ALG', 0) if bank_b['patches'] else 0
    alg_b = args.alg_b if args.alg_b is not None else alg_b_default

    patches = merge_banks(bank_a, bank_b, args.con4, alg_a, alg_b, args.bank, src_name)

    out = {
        "name":       src_name,
        "group":      "OPL3",
        "bank":       args.bank,
        "op_count":   4,
        "source":     (f"{Path(args.bank_a).name} [M1/C1]"
                       f" + {Path(args.bank_b).name} [M2/C2]"),
        "note": (
            f"ALG=(conn_sel=1, cnt1={alg_b & 1}, cnt0={alg_a & 1}), "
            f"FB/FB2は各patchごとにBank-A/Bから独立設定 (パック無し). "
            f"ops[0/1]=Bank-A(M1/C1), ops[2/3]=Bank-B(M2/C2)."
        ),
        "patches": patches,
    }

    dst = Path(args.output)
    if dst.is_dir():
        dst = dst / (src_name.replace(' ', '_') + '.hwbank.json')
    dst.write_text(json.dumps(out, indent=2, ensure_ascii=False))
    print(f"OK: {len(patches)}音色 → {dst}")
    print(f"    M1/C1: {name_a}")
    print(f"    M2/C2: {name_b}")
    print(f"    CON4={args.con4}, 1stペアALG={alg_a}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="OPL2 バンク 2本 → OPL3 4OP バンク 合成",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
例:
  # MicroComputerとDigitalを2OP×2並列(CON4=1)で合成
  python3 opl2_merge.py \\
      banks/OPL2/ma2_vma/MicroComputerNormalBank.hwbank.json \\
      banks/OPL2/ma2_vma/DigitalNormalBank.hwbank.json \\
      banks/OPL3/MicroComputer_x_Digital.hwbank.json

  # ALSAのOPL2とMicroComputerを全直列(CON4=0)で合成
  python3 opl2_merge.py \\
      banks/OPL2/alsa/std_opl2.hwbank.json \\
      banks/OPL2/ma2_vma/MicroComputerNormalBank.hwbank.json \\
      out.hwbank.json \\
      --con4 0 --alg-a 0

CON4 接続モード:
  0 : M1→C1→M2→C2  完全直列
  1 : (M1→C1) + (M2→C2)  独立並列 [デフォルト / バンク合成推奨]
  2 : M1→(C1+M2→C2)  C1とM2→C2の並列合流
  3 : M1→C1 + M2 + M2→C2  3出力混合
        """)
    parser.add_argument("bank_a",  help="Bank-A (M1/C1ペア用) OPL2 hwbank.json")
    parser.add_argument("bank_b",  help="Bank-B (M2/C2ペア用) OPL2 hwbank.json")
    parser.add_argument("output",  help="出力先 .hwbank.json (ディレクトリも可)")
    parser.add_argument("--con4",  type=int, default=DEFAULT_CON4,
                        choices=[0,1,2,3],
                        help=f"4OP 接続モード (デフォルト: {DEFAULT_CON4}=独立並列)")
    parser.add_argument("--alg-a", dest="alg_a", type=int, default=None,
                        choices=[0,1],
                        help="1stペア(M1→C1)の接続 0=FM 1=AM (デフォルト: Bank-Aから引き継ぎ)")
    parser.add_argument("--alg-b", dest="alg_b", type=int, default=None,
                        choices=[0,1],
                        help="2ndペア(M2→C2)の接続 0=FM 1=AM (デフォルト: Bank-Bから引き継ぎ)")
    parser.add_argument("--name",  type=str, default=None,
                        help="出力バンク名 (デフォルト: 'BankA x BankB')")
    parser.add_argument("--bank",  type=int, default=0,
                        help="出力バンク番号 (デフォルト: 0)")
    args = parser.parse_args()
    run(args)
