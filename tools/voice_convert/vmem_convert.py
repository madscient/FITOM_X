#!/usr/bin/env python3
"""
DX27/DX100 VMEM SysEx → FITOM_X hwbank.json (OPMグループ) 変換ツール

対応フォーマット:
  F0 43 0n 04 20 00 [4096 bytes] CS F7
  32音色 × 128バイト VMEM形式

VMEM構造 (128バイト/音色):
  P0-9:   OP4パラメータ
  P10-19: OP2パラメータ
  P20-29: OP3パラメータ
  P30-39: OP1パラメータ
  各OP10バイト:
    P+0: ATTACK RATE (0-31)
    P+1: DECAY 1 RATE (0-31)
    P+2: DECAY 2 RATE (0-31)
    P+3: RELEASE RATE (0-15)
    P+4: DECAY 1 LEVEL (0-15)
    P+5: KEYBOARD SCALING LEVEL (0-99, SW) ← ソフトパラメータ
    P+6: [AM:1][EG_BIAS_SENS:3][KEY_VEL:2][?:2]
    P+7: OUTPUT LEVEL (0-99)
    P+8: [FIXED:1][COARSE:6][?:1]
    P+9: [KSR:2][DETUNE:4]  DETUNE=0-14(中央7)
  P40: [LFO_SYNC:1][FB:4][ALG:3]
  P41: LFO SPEED (0-99)
  P42: LFO DELAY (0-99)
  P43: PITCH MOD DEPTH (0-99)
  P44: AMP MOD DEPTH (0-99)
  P45: [PMS:3][AMS:2][LFO_WAVE:2]
  P46: TRANSPOSE (0-48, 中央=24)
  P47: PITCH BEND RANGE (0-12)
  P57-66: VOICE NAME (10文字 ASCII)

パラメータ変換 (VMEM → OPM YM2151):
  AR:   0-31 → OPM AR 0-31 (直接)
  D1R:  0-31 → OPM D1R 0-31 (直接)
  D2R:  0-31 → OPM D2R 0-31 (直接)
  RR:   0-15 → OPM RR 0-15 (直接)
  D1L:  0-15 → OPM D1L 0-15 (直接)
  OL:   0-99 → OPM TL = 127 - round(OL*127/99)
  COARSE: P8[5:2] (4bit) → OPM MUL = 直接 (0-15)
  DT2:   P8[1:0] (2bit) → OPM DT2 = 直接 (0-3)
  DETUNE: 0-14(中央7) → OPM DT1
    DETUNE=7→DT1=0, 8-10→1-3(+), 4-6→5-7(-), 0-3→7(-), 11-14→3(+)
  KSR:  0-3 → OPM KS 0-3 (直接)
  AM:   0-1 → OPM AM 0-1 (直接)
  FB:   0-7 → OPM FB 0-7 (直接)
  ALG:  0-7 → OPM CON 0-7 (直接)
  PMS:  0-7 → OPM PMS 0-7 (直接, B4h上位)
  AMS:  0-3 → OPM AMS 0-3 (直接)
"""

import json, sys, argparse, struct
from pathlib import Path

# VMEM OP格納順とOPMレジスタの対応:
#   VMEM P0-9  (OP4) → OPM M1 → ops[0]
#   VMEM P10-19(OP2) → OPM C1 → ops[1]
#   VMEM P20-29(OP3) → OPM M2 → ops[2]
#   VMEM P30-39(OP1) → OPM C2 → ops[3]
# → VMEM格納順がそのままM1,C1,M2,C2順に対応するため並び替え不要

VMEM_OP_BASES = [0, 10, 20, 30]   # P0, P10, P20, P30

def ol_to_tl(ol):
    """OUTPUT LEVEL (0-99) → OPM TL (0-127, 0=最大)"""
    return 127 - round(ol * 127.0 / 99.0)

def coarse_to_mul(coarse):
    """FREQUENCY COARSE (0-63) → OPM MUL (0-15)"""
    return min(15, coarse)

def detune_to_dt1(detune):
    """DETUNE (0-14, 中央=7) → OPM DT1 (0-7)
    OPM DT1: 0=0, 1=+1, 2=+2, 3=+3, 4=-0(≒0), 5=-1, 6=-2, 7=-3
    """
    diff = detune - 7
    if diff == 0:
        return 0
    elif diff > 0:
        return min(3, diff)        # +1〜+3
    else:
        return min(3, -diff) + 4   # 5〜7 (-1〜-3)

def parse_vmem_voice(vbytes):
    """128バイトのVMEMデータを解析してFITOM_X HwPatchデータに変換"""
    # ops[0]=M1, ops[1]=C1, ops[2]=M2, ops[3]=C2
    ops = []
    for op_base in VMEM_OP_BASES:
        p = vbytes[op_base:op_base+10]
        ar   = p[0] & 0x1F
        d1r  = p[1] & 0x1F
        d2r  = p[2] & 0x1F
        rr   = p[3] & 0x0F
        d1l  = p[4] & 0x0F
        ksl  = p[5]              # Keyboard Scaling Level (ソフトパラメータ)
        am   = (p[6] >> 7) & 1
        eg_bias = (p[6] >> 4) & 7
        key_vel  = (p[6] >> 2) & 3
        ol   = p[7] & 0x7F
        # P8: [7]=FIXED, [6]=未使用, [5:2]=MUL(4bit), [1:0]=DT2(2bit)
        fixed  = (p[8] >> 7) & 1
        mul    = (p[8] >> 2) & 0xF    # bit5-2: MUL 0-15
        dt2    =  p[8] & 0x03         # bit1-0: DT2 0-3
        ksr    = (p[9] >> 4) & 3
        detune =  p[9] & 0x0F

        # OPM変換
        tl  = ol_to_tl(ol)
        dt1 = detune_to_dt1(detune)

        ops.append({
            # OPMレジスタ値 (FmHwOp互換)
            "AR":  ar,
            "D1R": d1r,
            "D2R": d2r,
            "RR":  rr,
            "D1L": d1l,
            "TL":  tl,
            "MUL": mul,
            "DT1": dt1,
            "DT2": dt2,
            "KS":  ksr,
            "AM":  am,
            # ソフトパラメータ
            "KSL":     ksl,
            "EG_BIAS": eg_bias,
            "KEY_VEL": key_vel,
            "FIXED":   fixed,
        })

    # 共通パラメータ
    p40 = vbytes[40]
    lfo_sync = (p40 >> 7) & 1
    fb       = (p40 >> 3) & 0xF
    alg      =  p40 & 7

    p45 = vbytes[45]
    pms = (p45 >> 4) & 7
    ams = (p45 >> 2) & 3
    lfo_wave = p45 & 3

    transpose = vbytes[46] - 24   # 0-48 → -24〜+24 半音

    name = ''.join(
        chr(vbytes[57+i]) if 32 <= vbytes[57+i] <= 126 else ' '
        for i in range(10)
    ).rstrip()

    return {
        "name":     name,
        "hw": {
            "ALG": alg,
            "FB":  fb,
            "PMS": pms,
            "AMS": ams,
            "LFO_WAVE": lfo_wave,
            "LFO_SYNC": lfo_sync,
        },
        "ops": ops,
        "sw": {
            "transpose":    transpose,
            "lfo_speed":    vbytes[41],
            "lfo_delay":    vbytes[42],
            "pmd":          vbytes[43],   # Pitch Mod Depth
            "amd":          vbytes[44],   # Amp Mod Depth
            "pitch_bend":   vbytes[47],
        },
    }

def convert_syx(src_path, dst_path, bank_no=0):
    data = Path(src_path).read_bytes()

    # SysEx検証
    if data[0] != 0xF0 or data[1] != 0x43:
        print(f"SKIP {src_path}: not Yamaha SysEx")
        return False
    if data[3] != 0x04:
        print(f"SKIP {src_path}: format={data[3]:02x} (expected 0x04 = 32-voice)")
        return False
    if data[-1] != 0xF7:
        print(f"WARN {src_path}: no EOX at end")

    voice_data = data[6:-2]  # ヘッダ6byte + チェックサム1 + F7 1を除く
    if len(voice_data) != 4096:
        print(f"SKIP {src_path}: voice_data length={len(voice_data)} (expected 4096)")
        return False

    src_name = Path(src_path).stem
    patches = []
    valid_count = 0

    for i in range(32):
        vbytes = voice_data[i*128:(i+1)*128]
        voice  = parse_vmem_voice(vbytes)

        # INIT VOICEはスキップしない（prog番号は維持）
        is_init = (voice["name"] == "INIT VOICE" or voice["name"] == "")
        if not is_init:
            valid_count += 1

        patch = {
            "prog": i,
            "name": voice["name"] if voice["name"] else f"Voice {i}",
            "hw":   voice["hw"],
            "ops":  voice["ops"],
            "sw":   voice["sw"],
        }
        patches.append(patch)

    out = {
        "name":    src_name,
        "group":   "OPM",
        "bank":    bank_no,
        "op_count": 4,
        "source":  f"{Path(src_path).name} (DX27/DX100 VMEM SysEx)",
        "note":    "OPMグループ(YM2151互換)。ops[]=[M1,C1,M2,C2]順。VMEM格納順(OP4,OP2,OP3,OP1)がそのままこの順に対応。",
        "patches": patches,
    }
    Path(dst_path).write_text(json.dumps(out, indent=2, ensure_ascii=False))
    print(f"OK {src_name}: {valid_count}/32音色 OPM(4OP) → {dst_path}")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="DX27/DX100 VMEM SysEx → FITOM_X hwbank.json (OPMグループ)")
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
            convert_syx(str(f), str(out), args.bank)
    else:
        if dst.is_dir():
            dst = dst / (src.stem + ".hwbank.json")
        convert_syx(str(src), str(dst), args.bank)
