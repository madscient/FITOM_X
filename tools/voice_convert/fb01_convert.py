#!/usr/bin/env python3
"""
Yamaha FB-01 RAW dump (.dmp) → FITOM_X hwbank.json (OPMグループ) 変換ツール

ファイル形式:
  32バイト バンクヘッダ (バンク名8バイト + 0パディング24バイト)
  64音色 × 48バイト voice data

Voice Data ($00-$2F, 48バイト):
  $00-$06: 音色名 (7バイト ASCII)
  $07:     LFO Speed (8bit)
  $08:     %xyyyyyyy  x=enable_lfo_load, y=AMD(7bit)
  $09:     %xyyyyyyy  x=sync_lfo_at_note_on, y=PMD(7bit)
  $0A:     (%xyyyyyyy 未使用 or 追加パラメータ)
  $0B:     %0wxyz000  w=OP#3_en, x=OP#2_en, y=OP#1_en, z=OP#0_en
  $0C:     %00xxxyyy  x=FB(3bit), y=ALG(3bit)
  $0D:     %0xxxxx00  x=PMS(3bit) + y=AMS(2bit) → %0xxxyyZZ: PMS[6:4] AMS[3:2]
  $0E:     %0xx00000  x=LFO wave(2bit)
  $0F:     transpose (signed byte, 100 cents resolution)
  $10-$17: OP#0 block (→ OPM M1 → ops[0])
  $18-$1F: OP#1 block (→ OPM M2 → ops[2])
  $20-$27: OP#2 block (→ OPM C1 → ops[1])
  $28-$2F: OP#3 block (→ OPM C2 → ops[3])

Operator Block (8バイト):
  $00: %0xxxxxxx  TL(7bit)
  $01: %xyyy0000  x=KLS_type_bit0, y=vel_sens_TL(3bit)
  $02: %xxxxyyyy  x=KLS_depth(4bit), y=TL_adjust(4bit)
  $03: %xyyyzzzz  x=KLS_type_bit1, y=DT1(3bit), z=MUL(4bit)
  $04: %xx0yyyyy  x=KRS(2bit), y=AR(5bit)
  $05: %xyyzzzzz  x=C/M_AM(1bit), y=vel_AR(2bit), z=D1R(5bit)
  $06: %xx0yyyyy  x=DT2(2bit), y=D2R(5bit)
  $07: %xxxxyyyy  x=SL(4bit), y=RR(4bit)

OP番号→OPMスロット→FITOM_X ops[]対応:
  FB-01 OP#0 → OPM M1 → ops[0]
  FB-01 OP#2 → OPM C1 → ops[1]
  FB-01 OP#1 → OPM M2 → ops[2]
  FB-01 OP#3 → OPM C2 → ops[3]
"""

import json, argparse
from pathlib import Path

HEADER_SIZE  = 32
VOICE_SIZE   = 48
VOICE_COUNT  = 64

# FB-01 OP格納順 → FITOM_X ops[M1,C1,M2,C2] へのインデックス
# ops[0]=M1=OP#0, ops[1]=C1=OP#2, ops[2]=M2=OP#1, ops[3]=C2=OP#3
OP_SLOT_ORDER = [0, 2, 1, 3]  # OP#インデックス順: [M1,C1,M2,C2]

def is_valid_name(name_bytes):
    """音色名が有効(ASCII印刷可能で最初の文字がアルファベット/数字)か判定"""
    if not name_bytes:
        return False
    first = name_bytes[0]
    if not (0x41 <= first <= 0x5A or 0x61 <= first <= 0x7A or 0x30 <= first <= 0x39):
        return False
    return all(0x20 <= b <= 0x7E for b in name_bytes)

def parse_op_block(b8):
    """8バイトのOP blockを解析してFmHwOp互換辞書を返す"""
    return {
        "TL":    b8[0] & 0x7F,
        "MUL":   b8[3] & 0x0F,
        "DT1":  (b8[3] >> 4) & 0x07,
        "DT2":  (b8[6] >> 6) & 0x03,
        "KS":   (b8[4] >> 6) & 0x03,   # = KRS
        "AR":    b8[4] & 0x1F,
        "D1R":   b8[5] & 0x1F,
        "D2R":   b8[6] & 0x1F,
        "D1L":  (b8[7] >> 4) & 0x0F,   # = SL
        "RR":    b8[7] & 0x0F,
        "AM":   (b8[5] >> 7) & 0x01,   # C/M_AM flag
        # ソフトパラメータ
        "VEL_TL":   (b8[1] >> 4) & 0x07,
        "VEL_AR":   (b8[5] >> 5) & 0x03,
        "KLS_type": (((b8[3] >> 7) & 1) << 1) | ((b8[1] >> 7) & 1),
        "KLS_depth": (b8[2] >> 4) & 0x0F,
        "TL_adj":    b8[2] & 0x0F,
    }

def parse_voice(vbytes):
    """48バイトのvoice dataを解析"""
    name = vbytes[:7].decode('ascii', errors='replace').rstrip()

    lfo_speed    = vbytes[7]
    enable_lfo   = (vbytes[8] >> 7) & 1
    amd          =  vbytes[8] & 0x7F
    sync_lfo     = (vbytes[9] >> 7) & 1
    pmd          =  vbytes[9] & 0x7F

    op_enable = [(vbytes[11] >> (3 + i)) & 1 for i in range(4)]  # OP#0-3

    fb          = (vbytes[12] >> 3) & 0x07
    alg         =  vbytes[12] & 0x07
    pms         = (vbytes[13] >> 4) & 0x07
    ams         = (vbytes[13] >> 2) & 0x03
    lfo_wave    = (vbytes[14] >> 1) & 0x03
    transpose   =  vbytes[15] if vbytes[15] < 128 else vbytes[15] - 256

    # 4OP blocks: OP#0=$10, OP#1=$18, OP#2=$20, OP#3=$28
    raw_ops = [parse_op_block(vbytes[16 + i*8 : 24 + i*8]) for i in range(4)]

    # FITOM_X ops[M1,C1,M2,C2] 順に並べ替え
    # OP#0=M1→[0], OP#2=C1→[1], OP#1=M2→[2], OP#3=C2→[3]
    ops = [raw_ops[OP_SLOT_ORDER[i]] for i in range(4)]

    return {
        "name":  name,
        "hw": {
            "ALG": alg,
            "FB":  fb,
            "PMS": pms,
            "AMS": ams,
            "LFO_WAVE":   lfo_wave,
            "LFO_SYNC":   sync_lfo,
            "LFO_ENABLE": enable_lfo,
        },
        "ops": ops,
        "sw": {
            "lfo_speed":  lfo_speed,
            "amd":        amd,
            "pmd":        pmd,
            "transpose":  transpose,
            "op_enable":  op_enable,
        },
    }

def convert(src_path, dst_path, bank_no=0):
    data = Path(src_path).read_bytes()
    assert len(data) == HEADER_SIZE + VOICE_COUNT * VOICE_SIZE, \
        f"Expected {HEADER_SIZE + VOICE_COUNT*VOICE_SIZE}, got {len(data)}"

    bank_name = data[:8].decode('ascii', errors='replace').rstrip()
    src_name  = Path(src_path).stem

    patches = []
    prog_no = 0
    for i in range(VOICE_COUNT):
        off    = HEADER_SIZE + i * VOICE_SIZE
        vbytes = data[off:off + VOICE_SIZE]
        name_bytes = vbytes[:7]

        if not is_valid_name(name_bytes):
            continue  # 未使用スロットをスキップ

        voice = parse_voice(vbytes)
        patches.append({
            "prog":  prog_no,
            "slot":  i,           # 元のスロット番号
            "name":  voice["name"],
            "hw":    voice["hw"],
            "ops":   voice["ops"],
            "sw":    voice["sw"],
        })
        prog_no += 1

    out = {
        "name":     f"{src_name} ({bank_name})",
        "group":    "OPM",
        "bank":     bank_no,
        "op_count": 4,
        "source":   f"{Path(src_path).name} (Yamaha FB-01 ROM dump)",
        "patches":  patches,
    }
    Path(dst_path).write_text(json.dumps(out, indent=2, ensure_ascii=False))
    print(f"OK {src_name}: {len(patches)}音色 OPM(4OP) → {dst_path}")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Yamaha FB-01 .dmp → FITOM_X hwbank.json (OPMグループ)")
    parser.add_argument("input",  help="*.dmp ファイル (またはディレクトリ)")
    parser.add_argument("output", help="出力先ファイル or ディレクトリ")
    parser.add_argument("--bank", type=int, default=0)
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)

    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for f in sorted(src.glob("*.dmp")):
            out = dst / (f.stem + ".hwbank.json")
            convert(str(f), str(out), args.bank)
    else:
        if dst.is_dir():
            dst = dst / (src.stem + ".hwbank.json")
        convert(str(src), str(dst), args.bank)
