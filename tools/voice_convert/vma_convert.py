#!/usr/bin/env python3
"""
MA-2 VMA (FM) → FITOM_X hwbank.json 変換ツール

対応: FM  マジックを持つ VMA ファイル (2OP/4OP, メロディ/ドラム)
非対応: ADP  マジック (ADPCM) → スキップ

VMA フォーマット:
  byte0-3 : "FM  " マジック
  byte4-5 : 不明
  byte6-7 : データ部サイズ big-endian (N*42)
  byte8 ～ byte8+N*16-1    : 名前部 (各16バイト)
  byte8+N*16 ～             : パラメータ部 (各26バイト)
  N = 128 (メロディ) or 79 (ドラム, MIDI note 27-105に対応)

パラメータ26バイト:
  byte0   : 不明
  byte1   : OPタイプ (0=2OP/OPL2, 1=4OP/OPL3)
  byte2   : プログラム番号
  byte3   : LFO[7] | FB[5:3] | ALG[1:0]  (MA-2共通パラメータ)
  byte4   : 不明 (0x01固定)
  byte5-9 : OP1 (5バイト, MA-2形式)
  byte10-14: OP2 (5バイト)
  byte15-19: OP3 (5バイト, 2OPでは0固定)
  byte20-24: OP4 (5バイト, 2OPでは0固定)
  byte25  : パディング

MA-2 OP 5バイト:
  byte0: MULT[7:4] | VIB[3] | EGT[2] | SUS[1] | KSR[0]
  byte1: RR[7:4]   | DR[3:0]
  byte2: AR[7:4]   | SL[3:0]
  byte3: TL[7:2]   | KSL[1:0]
  byte4: DVB[7] | DAM[6] | AM[5] | WS[4:2] | xx[1:0]
"""

import json, struct, sys, argparse
from pathlib import Path

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

def parse_ma2_op(b5):
    return {
        "MULT": (b5[0] >> 4) & 0xF,
        "VIB":  (b5[0] >> 3) & 1,
        "EGT":  (b5[0] >> 2) & 1,
        "SUS":  (b5[0] >> 1) & 1,
        "KSR":   b5[0] & 1,
        "RR":   (b5[1] >> 4) & 0xF,
        "DR":    b5[1] & 0xF,
        "AR":   (b5[2] >> 4) & 0xF,
        "SL":    b5[2] & 0xF,
        "TL":   (b5[3] >> 2) & 0x3F,
        "KSL":   b5[3] & 3,
        "DVB":  (b5[4] >> 7) & 1,
        "DAM":  (b5[4] >> 6) & 1,
        "AM":   (b5[4] >> 5) & 1,
        "WS":   (b5[4] >> 2) & 7,
    }

def convert_vma(src_path, dst_path, bank_no=0):
    data = Path(src_path).read_bytes()

    magic = data[:4]
    if magic != b'FM  ':
        print(f"SKIP {src_path}: magic={magic} (非FMファイル)")
        return False

    data_size = struct.unpack_from('>H', data, 6)[0]
    N = data_size // 42
    is_drum = (N == 79)

    param_base = 8 + N * 16
    is_4op = (data[param_base + 1] == 1) if len(data) > param_base + 1 else False
    group  = "OPL3" if is_4op else "OPL2"

    patches = []
    for i in range(N):
        name_off = 8 + i * 16
        raw_name = data[name_off:name_off+16].rstrip(b'\x00').decode('ascii', errors='replace')

        off   = param_base + i * 26
        chunk = data[off:off+26]

        prog  = chunk[2]
        byte3 = chunk[3]
        lfo   = (byte3 >> 7) & 1
        fb    = (byte3 >> 3) & 7
        alg   =  byte3 & 3

        op1 = parse_ma2_op(chunk[5:10])
        op2 = parse_ma2_op(chunk[10:15])
        ops = [op1, op2]
        if is_4op:
            ops.append(parse_ma2_op(chunk[15:20]))
            ops.append(parse_ma2_op(chunk[20:25]))

        if raw_name:
            name = raw_name
        elif not is_drum and int(prog) < len(GM_NAMES):
            name = GM_NAMES[int(prog)]
        else:
            name = f"Drum Note {27+i}" if is_drum else f"Program {prog}"

        # 旧FITOM方式でALG/FBを統一エンコード:
        #   ALG bit2=conn_sel(4OPなら1), bit1=cnt1(Array1 CNT), bit0=cnt0(Array0 CNT)
        #   FB  bit5-3=fb1(Array1 FB),   bit2-0=fb0(Array0 FB)
        if is_4op:
            # MA-2 4OP: ALG/FBは全体共通 → 両ペアに同じ値を使用
            # cnt0=cnt1=alg, fb0=fb1=fb, conn_sel=1
            alg_enc = (1 << 2) | (alg << 1) | alg
            fb_enc  = (fb << 3) | fb
        else:
            alg_enc = alg   # 2OP: そのまま
            fb_enc  = fb

        patch = {
            "prog": i if is_drum else int(prog),
            "name": name,
            "hw": {"ALG": alg_enc, "FB": fb_enc, "LFO": lfo},
            "ops": ops,
        }
        if is_drum:
            patch["midi_note"] = 27 + i
        patches.append(patch)

    src_name = Path(src_path).stem
    out = {
        "name": src_name,
        "group": group,
        "bank": bank_no,
        "op_count": 4 if is_4op else 2,
        "source": f"{Path(src_path).name} (MA-2 VMA FM format)",
        "patches": patches,
    }
    Path(dst_path).write_text(json.dumps(out, indent=2, ensure_ascii=False))
    op_str = "4OP" if is_4op else "2OP"
    kind   = "drum" if is_drum else "melody"
    print(f"OK {src_name}: {N}音色 {group}({op_str}) {kind} → {dst_path}")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MA-2 VMA → FITOM_X hwbank.json")
    parser.add_argument("input",  help="*.vma ファイル (または *.vma を含むディレクトリ)")
    parser.add_argument("output", help="出力先ファイル or ディレクトリ")
    parser.add_argument("--bank", type=int, default=0, help="HwBank番号 (default: 0)")
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)

    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for vma in sorted(src.glob("*.vma")):
            out = dst / (vma.stem + ".hwbank.json")
            convert_vma(str(vma), str(out), args.bank)
    else:
        if dst.is_dir():
            dst = dst / (src.stem + ".hwbank.json")
        convert_vma(str(src), str(dst), args.bank)
