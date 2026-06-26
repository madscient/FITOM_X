#!/usr/bin/env python3
"""
necopn.bin → FITOM_X hwbank.json 変換ツール

フォーマット（N88-BASIC(86)互換 + 14バイトパディング）:
  1音色 = 64バイト
  [0-5]   OP1(M1): DT1/MUL, TL, KSR/AR, AM/DR, SR, SL/RR
  [6-11]  OP2(C1): 同上
  [12-17] OP3(M2): 同上
  [18-23] OP4(C2): 同上
  [24]    FB/ALG  (D5-3=FB, D2-0=ALG)
  [25]    AMS/FMS (D5-4=AMS, D2-0=FMS/PMS)
  [26-63] パディング (38バイト)

OPNチップのレジスタ順: M1,M2,C1,C2
N88-BASIC(86)の格納順: M1,C1,M2,C2 (OP1,OP2,OP3,OP4)
→ 変換時に並び替えが必要
"""

import struct, json, sys, argparse
from pathlib import Path

# GM楽器名 (prog 0-127)
GM_NAMES = [
    "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano",
    "Honky-tonk Piano","Electric Piano 1","Electric Piano 2",
    "Harpsichord","Clavi","Celesta","Glockenspiel","Music Box",
    "Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer",
    "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ",
    "Reed Organ","Accordion","Harmonica","Tango Accordion",
    "Acoustic Guitar (nylon)","Acoustic Guitar (steel)",
    "Electric Guitar (jazz)","Electric Guitar (clean)",
    "Electric Guitar (muted)","Overdriven Guitar",
    "Distortion Guitar","Guitar harmonics",
    "Acoustic Bass","Electric Bass (finger)","Electric Bass (pick)",
    "Fretless Bass","Slap Bass 1","Slap Bass 2",
    "Synth Bass 1","Synth Bass 2",
    "Violin","Viola","Cello","Contrabass",
    "Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
    "String Ensemble 1","String Ensemble 2","SynthStrings 1","SynthStrings 2",
    "Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit",
    "Trumpet","Trombone","Tuba","Muted Trumpet",
    "French Horn","Brass Section","SynthBrass 1","SynthBrass 2",
    "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax",
    "Oboe","English Horn","Bassoon","Clarinet",
    "Piccolo","Flute","Recorder","Pan Flute",
    "Blown Bottle","Shakuhachi","Whistle","Ocarina",
    "Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)",
    "Lead 4 (chiff)","Lead 5 (charang)","Lead 6 (voice)",
    "Lead 7 (fifths)","Lead 8 (bass + lead)",
    "Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)",
    "Pad 4 (choir)","Pad 5 (bowed)","Pad 6 (metallic)",
    "Pad 7 (halo)","Pad 8 (sweep)",
    "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)",
    "FX 4 (atmosphere)","FX 5 (brightness)","FX 6 (goblins)",
    "FX 7 (echoes)","FX 8 (sci-fi)",
    "Sitar","Banjo","Shamisen","Koto","Kalimba",
    "Bag pipe","Fiddle","Shanai",
    "Tinkle Bell","Agogo","Steel Drums","Woodblock",
    "Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
    "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet",
    "Telephone Ring","Helicopter","Applause","Gunshot",
]

def parse_op(data, base):
    """1OPのパラメータをバイト列から解析"""
    return {
        "DT1": (data[base+0] >> 4) & 0x07,
        "MUL":  data[base+0] & 0x0F,
        "TL":   data[base+1] & 0x7F,
        "KSR": (data[base+2] >> 6) & 0x03,
        "AR":   data[base+2] & 0x1F,
        "AM":  (data[base+3] >> 7) & 0x01,
        "DR":   data[base+3] & 0x1F,
        "SR":   data[base+4] & 0x1F,
        "SL":  (data[base+5] >> 4) & 0x0F,
        "RR":   data[base+5] & 0x0F,
    }

def convert(src_path, dst_path, bank_no=0, group="OPN"):
    data = Path(src_path).read_bytes()
    assert len(data) == 128 * 64, f"Expected 8192 bytes, got {len(data)}"

    patches = []
    for prog in range(128):
        base = prog * 64

        # N88-BASIC(86)格納順: M1(op0), C1(op1), M2(op2), C2(op3)
        op_m1 = parse_op(data, base + 0)   # OP1 = M1
        op_c1 = parse_op(data, base + 6)   # OP2 = C1
        op_m2 = parse_op(data, base + 12)  # OP3 = M2
        op_c2 = parse_op(data, base + 18)  # OP4 = C2

        fb_alg = data[base + 24]
        ams_fms = data[base + 25]

        alg = fb_alg & 0x07
        fb  = (fb_alg >> 3) & 0x07
        fms =  ams_fms & 0x07       # PMS (B4h の bit2-0)
        ams = (ams_fms >> 4) & 0x03 # AMS (B4h の bit5-4)

        name = GM_NAMES[prog] if prog < len(GM_NAMES) else f"Program {prog}"

        # FITOM_X HwPatch JSON形式
        # ops[] の順番: M1, M2, C1, C2 (OPNレジスタ順: 30h系はM1,M2,C1,C2)
        # ただしFITOM_X の FmHwOp[4] の並びを確認
        # VoiceData.h: hwOp[0]=op1, [1]=op2, [2]=op3, [3]=op4
        # OPNレジスタ: op=0→M1(+0), op=1→M2(+8), op=2→C1(+4), op=3→C2(+12)
        # → [0]=M1, [1]=M2, [2]=C1, [3]=C2 の順で格納
        patches.append({
            "prog": prog,
            "name": name,
            "hw": {
                "ALG": alg,
                "FB":  fb,
                "AMS": ams,
                "FMS": fms,
            },
            "ops": [
                op_m1,  # op[0] = M1
                op_m2,  # op[1] = M2
                op_c1,  # op[2] = C1
                op_c2,  # op[3] = C2
            ]
        })

    out = {
        "name": "necopn GM Bank",
        "group": group,
        "bank": bank_no,
        "source": "necopn.bin (Windows OPNA driver preset, N88-BASIC(86) format)",
        "patches": patches
    }

    Path(dst_path).write_text(json.dumps(out, indent=2, ensure_ascii=False))
    print(f"Converted {len(patches)} patches → {dst_path}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="necopn.bin → FITOM_X hwbank.json")
    parser.add_argument("input",  help="necopn.bin")
    parser.add_argument("output", help="output .hwbank.json")
    parser.add_argument("--bank", type=int, default=0, help="HwBank番号 (default: 0)")
    parser.add_argument("--group", default="OPN", help="チップグループ (default: OPN)")
    args = parser.parse_args()
    convert(args.input, args.output, args.bank, args.group)
