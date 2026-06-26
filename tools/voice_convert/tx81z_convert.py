#!/usr/bin/env python3
"""
Yamaha TX81Z 32-Voice VMEM SysEx → FITOM_X hwbank.json (OPMグループ) 変換ツール

フォーマット (Image4より):
  F0 43 0n 04 10 00 [4096 bytes] CS F7
  32音色 × 128バイト 拡張VMEM (VCED+ACED)

1音色 128バイト構成:
  addr  0-9:   OP4 VCED (→ OPM M1 → ops[0])
  addr 10-19:  OP2 VCED (→ OPM C1 → ops[1])
  addr 20-29:  OP3 VCED (→ OPM M2 → ops[2])
  addr 30-39:  OP1 VCED (→ OPM C2 → ops[3])
  addr 40:     [SY:1][FBL:3][ALG:3]
  addr 41:     LFS (LFO speed)
  addr 42:     LFD (LFO delay)
  addr 43:     PMD
  addr 44:     AMD
  addr 45:     [PMS:3][AMS:2][LFW:2]
  addr 46:     TRPS (transpose 0-48, 中央=24)
  addr 47:     [PBR:4] pitch bend range
  addr 48:     [MO:1][SU:1][PO:1][PM:1][CH:2]
  addr 49:     PORT
  addr 50-56:  FC VOL, MW PITCH/AMPLI, BC PITCH/AMPLI/P.BIAS/E.BIAS
  addr 57-66:  VOICE NAME (10文字 ASCII)
  addr 67-69:  PEG PR1-3
  addr 70-72:  PEG PL1-3
  addr 73-74:  OP4 ACED拡張
  addr 75-76:  OP2 ACED拡張
  addr 77-78:  OP3 ACED拡張
  addr 79-80:  OP1 ACED拡張
  addr 81:     REV
  addr 82:     FC PITCH
  addr 83:     FC AMPLI
  addr 84-127: padding (0)

OP VCED 10バイト:
  +0: [b4:0]=AR(0-31)
  +1: [b4:0]=D1R(0-31)
  +2: [b4:0]=D2R(0-31)
  +3: [b3:0]=RR(0-15)
  +4: [b3:0]=D1L(0-15)
  +5: LS(0-99) keyboard level scaling
  +6: [AME:1][EBS:3][KVS:3]
  +7: OUT(0-99) → TL = 127 - round(OUT×127/99)
  +8: F [b5:2]=MUL(4bit), [b1:0]=DT2(2bit)
  +9: [RS:2][DBT:4]  DBT: 0-14(中央7) → DT1

OP ACED 2バイト:
  byte0: [EGSFT:3][FIX:1][FIXRG:4]
  byte1: [OPW:3][FINE:4]

TX81Z固有拡張 (FITOM_X OPZ対応):
  OPW: 波形選択 0-7 (OPM拡張 = FmHwOp::WS に格納)
  FIX: 固定周波数モード
  FIXRG: 固定周波数レンジ (0-7)
  FINE: 微調整 (0-15)
  EGSFT: EG shift (0-7)
"""

import json, argparse, struct
from pathlib import Path

# VMEM OP格納順 → FITOM_X ops[M1,C1,M2,C2]
# addr0=OP4=M1, addr10=OP2=C1, addr20=OP3=M2, addr30=OP1=C2
VCED_BASES = [0, 10, 20, 30]
ACED_BASES = [73, 75, 77, 79]

def out_to_tl(out):
    """OUT (0-99) → OPM TL (0-127)"""
    return 127 - round(out * 127.0 / 99.0)

def dbt_to_dt1(dbt):
    """DBT/DETUNE (0-14, 中央=7) → OPM DT1 (0-7)"""
    diff = dbt - 7
    if diff == 0:   return 0
    elif diff > 0:  return min(3, diff)
    else:           return min(3, -diff) + 4

def parse_op(vp, ap):
    """VCED 10バイト + ACED 2バイト → FmHwOp辞書"""
    mul = (vp[8] >> 2) & 0xF
    dt2 =  vp[8] & 0x03
    dt1 = dbt_to_dt1(vp[9] & 0x0F)
    rs  = (vp[9] >> 4) & 0x03

    opw   = (ap[1] >> 4) & 0x07   # TX81Z wave shape → WS
    fine  =  ap[1] & 0x0F
    fix   = (ap[0] >> 4) & 0x01
    fixrg =  ap[0] & 0x0F
    egsft = (ap[0] >> 5) & 0x07

    return {
        # OPMレジスタ値
        "AR":  vp[0] & 0x1F,
        "D1R": vp[1] & 0x1F,
        "D2R": vp[2] & 0x1F,
        "RR":  vp[3] & 0x0F,
        "D1L": vp[4] & 0x0F,
        "TL":  out_to_tl(vp[7]),
        "MUL": mul,
        "DT1": dt1,
        "DT2": dt2,
        "KS":  rs,
        "AM":  (vp[6] >> 7) & 1,
        "WS":  opw,               # TX81Z波形 (OPZ拡張)
        # TX81Z固有拡張
        "FIX":   fix,
        "FIXRG": fixrg,
        "FINE":  fine,
        "EGSFT": egsft,
        # ソフトパラメータ
        "EBS": (vp[6] >> 3) & 7,  # EG Bias Sensitivity
        "KVS":  vp[6] & 7,         # Key Velocity Sensitivity
        "LS":   vp[5],             # Level Scaling
    }

def parse_voice(vbytes):
    """128バイトの拡張VMEMデータを解析"""
    name = ''.join(
        chr(vbytes[57+i]) if 32 <= vbytes[57+i] <= 126 else ' '
        for i in range(10)
    ).rstrip()

    # ops: [M1(OP4), C1(OP2), M2(OP3), C2(OP1)]
    ops = [
        parse_op(vbytes[vb:vb+10], vbytes[ab:ab+2])
        for vb, ab in zip(VCED_BASES, ACED_BASES)
    ]

    b40 = vbytes[40]
    b45 = vbytes[45]
    b48 = vbytes[48]

    return {
        "name": name,
        "hw": {
            "ALG": b40 & 7,
            "FB":  (b40 >> 3) & 7,
            "SY":  (b40 >> 7) & 1,   # LFO sync
            "PMS": (b45 >> 4) & 7,
            "AMS": (b45 >> 2) & 3,
            "LFW": b45 & 3,
            "REV": vbytes[81],
        },
        "ops": ops,
        "sw": {
            "lfo_speed":  vbytes[41],
            "lfo_delay":  vbytes[42],
            "pmd":        vbytes[43],
            "amd":        vbytes[44],
            "transpose":  vbytes[46] - 24,  # 0-48 → -24〜+24
            "pitch_bend": vbytes[47] & 0xF,
            "port_time":  vbytes[49],
            "peg_pr":     [vbytes[67], vbytes[68], vbytes[69]],
            "peg_pl":     [vbytes[70], vbytes[71], vbytes[72]],
            "fc_pitch":   vbytes[82],
            "fc_ampli":   vbytes[83],
        },
    }

def convert(src_path, dst_path, bank_no=0):
    data = Path(src_path).read_bytes()

    # SysEx検証
    assert data[0] == 0xF0 and data[1] == 0x43, "Not Yamaha SysEx"
    assert data[3] == 0x04, f"Format={data[3]:02x} (expected 0x04)"
    data_size = (data[4] << 7) | data[5]   # MIDI 7bit encoding
    assert data_size == 4096, f"Data size={data_size} (expected 4096)"
    assert data[-1] == 0xF7, "No EOX"

    voice_data = data[6:-2]
    assert len(voice_data) == 4096, f"Voice data length={len(voice_data)}"

    src_name = Path(src_path).stem
    patches  = []

    for i in range(32):
        vbytes = voice_data[i*128:(i+1)*128]
        voice  = parse_voice(vbytes)
        patches.append({
            "prog": i,
            "name": voice["name"],
            "hw":   voice["hw"],
            "ops":  voice["ops"],
            "sw":   voice["sw"],
        })

    out = {
        "name":     src_name,
        "group":    "OPM",          # TX81Zは OPZ(YM2414) だが OPMグループ互換
        "chip":     "OPZ",          # TX81Z固有拡張を示す
        "bank":     bank_no,
        "op_count": 4,
        "source":   f"{Path(src_path).name} (TX81Z 32-Voice VMEM SysEx)",
        "note":     "ops[]=[M1,C1,M2,C2]順。WS=OPZ波形(0-7)、FIX/FIXRG/FINE/EGSFTはOPZ拡張。",
        "patches":  patches,
    }
    Path(dst_path).write_text(json.dumps(out, indent=2, ensure_ascii=False))
    print(f"OK {src_name}: 32音色 OPZ(4OP) → {dst_path}")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="TX81Z 32-Voice VMEM SysEx → FITOM_X hwbank.json")
    parser.add_argument("input",  help="*.syx ファイル (またはディレクトリ)")
    parser.add_argument("output", help="出力先ファイル or ディレクトリ")
    parser.add_argument("--bank", type=int, default=0)
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)

    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for f in sorted(src.glob("*.syx")):
            out = dst / (f.stem + ".hwbank.json")
            convert(str(f), str(out), args.bank)
    else:
        if dst.is_dir():
            dst = dst / (src.stem + ".hwbank.json")
        convert(str(src), str(dst), args.bank)
