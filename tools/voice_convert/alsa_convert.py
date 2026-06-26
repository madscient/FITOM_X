#!/usr/bin/env python3
"""
ALSA sbiload .sb/.o3 → FITOM_X hwbank.json 変換ツール

.sb (OPL2): 128音色 × 52バイト
  [0-3]  : マジック "SBI\x1A" or "2OP\x1A" (どちらも同じ構造)
            マジック 0x00000000 = 未使用エントリ
  [4-35] : 音色名 (32バイト, NULL終端)
  [36-46]: OPLレジスタ直接値 11バイト
    [0] Mod Char  (0x20): AM|VIB|EGT|KSR|MULT
    [1] Car Char  (0x23): AM|VIB|EGT|KSR|MULT
    [2] Mod KSL/TL (0x40): KSL|TL
    [3] Car KSL/TL (0x43): KSL|TL
    [4] Mod AR/DR  (0x60): AR|DR
    [5] Car AR/DR  (0x63): AR|DR
    [6] Mod SL/RR  (0x80): SL|RR
    [7] Car SL/RR  (0x83): SL|RR
    [8] Mod Wave   (0xE0): WS
    [9] Car Wave   (0xE3): WS
    [10] FB/CON    (0xC0): 0|FB[2:0]|CON
  [47-51]: SBTimbre拡張 (PercVoc, Transpos, PercPitch, Reserved×2)

.o3 (OPL3): 128音色 × 60バイト
  [0-3]  : マジック "4OP\x1A" (4OP) or "2OP\x1A" (2OP扱い、ただし後半にも有効データあり)
  [4-35] : 音色名
  [36-46]: 2OP part1 (11バイト) = OP1(Mod1)+OP2(Car1)
  [47-57]: 2OP part2 (11バイト) = OP3(Mod2)+OP4(Car2)
  [58-59]: パディング

drums.sb/.o3: prog番号 = MIDIノート番号 (35-81に実データあり)
"""

import json, sys, argparse
from pathlib import Path

GM_DRUM_NAMES = {
    35:"Acoustic Bass Drum",36:"Bass Drum 1",37:"Side Stick",
    38:"Acoustic Snare",39:"Hand Clap",40:"Electric Snare",
    41:"Low Floor Tom",42:"Closed Hi-Hat",43:"High Floor Tom",
    44:"Pedal Hi-Hat",45:"Low Tom",46:"Open Hi-Hat",
    47:"Low-Mid Tom",48:"High-Mid Tom",49:"Crash Cymbal 1",
    50:"High Tom",51:"Ride Cymbal 1",52:"Chinese Cymbal",
    53:"Ride Bell",54:"Tambourine",55:"Splash Cymbal",
    56:"Cowbell",57:"Crash Cymbal 2",58:"Vibraslap",
    59:"Ride Cymbal 2",60:"Hi Bongo",61:"Low Bongo",
    62:"Mute Hi Conga",63:"Open Hi Conga",64:"Low Conga",
    65:"High Timbale",66:"Low Timbale",67:"High Agogo",
    68:"Low Agogo",69:"Cabasa",70:"Maracas",
    71:"Short Whistle",72:"Long Whistle",73:"Short Guiro",
    74:"Long Guiro",75:"Claves",76:"Hi Wood Block",
    77:"Low Wood Block",78:"Mute Cuica",79:"Open Cuica",
    80:"Mute Triangle",81:"Open Triangle",
}

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
    "Sitar","Banjo","Shamisen","Koto","Kalimba","Bag pipe","Fiddle","Shanai",
    "Tinkle Bell","Agogo","Steel Drums","Woodblock",
    "Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
    "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet",
    "Telephone Ring","Helicopter","Applause","Gunshot",
]

def decode_op(char, scale, ar_dr, sl_rr, wave):
    return {
        "MULT":  char & 0xF,
        "KSR":  (char >> 4) & 1,
        "EGT":  (char >> 5) & 1,
        "VIB":  (char >> 6) & 1,
        "AM":   (char >> 7) & 1,
        "KSL":  (scale >> 6) & 3,
        "TL":    scale & 0x3F,
        "AR":   (ar_dr >> 4) & 0xF,
        "DR":    ar_dr & 0xF,
        "SL":   (sl_rr >> 4) & 0xF,
        "RR":    sl_rr & 0xF,
        "WS":    wave & 7,
    }

def parse_2op_block(p11):
    """11バイトの2OPブロックを解析して (mod_op, car_op, fb, con) を返す"""
    mod = decode_op(p11[0], p11[2], p11[4], p11[6], p11[8])
    car = decode_op(p11[1], p11[3], p11[5], p11[7], p11[9])
    fb  = (p11[10] >> 1) & 7
    con =  p11[10] & 1
    return mod, car, fb, con

def convert(src_path, dst_path, bank_no=0):
    data     = Path(src_path).read_bytes()
    src_name = Path(src_path).stem
    ext      = Path(src_path).suffix.lower()
    is_drum  = 'drum' in src_name.lower()

    VALID_MAGICS_SB = {b'SBI\x1a', b'2OP\x1a', b'\x00\x00\x00\x00'}
    VALID_MAGICS_O3 = {b'4OP\x1a', b'2OP\x1a', b'\x00\x00\x00\x00'}

    if ext == '.sb':
        assert len(data) == 128 * 52, f"Expected {128*52}, got {len(data)}"
        group, op_count, entry_size = "OPL2", 2, 52
    elif ext == '.o3':
        assert len(data) == 128 * 60, f"Expected {128*60}, got {len(data)}"
        group, op_count, entry_size = "OPL3", 4, 60
    else:
        print(f"SKIP {src_path}: unknown extension")
        return False

    patches = []
    for prog in range(128):
        off   = prog * entry_size
        magic = data[off:off+4]
        empty = (magic == b'\x00\x00\x00\x00')

        raw_name = data[off+4:off+36].split(b'\x00')[0].decode('ascii', errors='replace').strip()
        p = data[off+36:off+entry_size]  # 16 or 24 bytes

        if ext == '.sb':
            mod, car, fb, con = parse_2op_block(p[0:11])
            ops = [mod, car]
            hw  = {"ALG": con, "FB": fb}
            # SBTimbre拡張
            perc_voc   = p[11] if len(p) > 11 else 0
            perc_pitch = p[13] if len(p) > 13 else 0
        else:  # .o3
            mod1, car1, fb1, con1 = parse_2op_block(p[0:11])
            mod2, car2, fb2, con2 = parse_2op_block(p[11:22])
            ops = [mod1, car1, mod2, car2]
            hw  = {"ALG": con1, "FB": fb1, "ALG2": con2, "FB2": fb2}
            perc_voc = perc_pitch = 0

        # 音色名の決定
        if raw_name:
            name = raw_name
        elif is_drum and prog in GM_DRUM_NAMES:
            name = GM_DRUM_NAMES[prog]
        elif not is_drum and prog < len(GM_NAMES):
            name = GM_NAMES[prog]
        else:
            name = f"Program {prog}"

        patch = {
            "prog": prog,
            "name": name,
            "hw":   hw,
            "ops":  ops,
            "empty": empty,
        }
        if is_drum:
            patch["midi_note"] = prog   # drums.sb: prog番号=MIDIノート番号
        if perc_voc:
            patch["perc_voc"]   = perc_voc
            patch["perc_pitch"] = perc_pitch

        patches.append(patch)

    filled = sum(1 for p in patches if not p["empty"])
    out = {
        "name":       src_name,
        "group":      group,
        "bank":       bank_no,
        "op_count":   op_count,
        "source":     f"{Path(src_path).name} (ALSA sbiload format)",
        "patches":    patches,
    }
    Path(dst_path).write_text(json.dumps(out, indent=2, ensure_ascii=False))
    kind = "drum" if is_drum else "melody"
    print(f"OK {src_name}: {filled}/128音色 {group}({op_count}OP) {kind} → {dst_path}")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ALSA sbiload .sb/.o3 → FITOM_X hwbank.json")
    parser.add_argument("input",  help="*.sb or *.o3 file (or directory)")
    parser.add_argument("output", help="output file or directory")
    parser.add_argument("--bank", type=int, default=0)
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)

    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for f in sorted(list(src.glob("*.sb")) + list(src.glob("*.o3"))):
            out = dst / (f.stem + ".hwbank.json")
            convert(str(f), str(out), args.bank)
    else:
        if dst.is_dir():
            dst = dst / (src.stem + ".hwbank.json")
        convert(str(src), str(dst), args.bank)
