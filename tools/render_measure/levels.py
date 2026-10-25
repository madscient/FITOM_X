"""音量を動かす操作が、チップ族によらず同じ dB で効くかを測る。

CC#7(ボリューム)・ベロシティ・マスターボリューム(ユニバーサルSysEx)を1つずつ振り、
発音直後の RMS を、値 127 のときからの dB で求める。期待値は GM2 の 40·log10(v/127) dB
(FITOM_X のラウドネスの目盛り。マスターボリュームは FitomEmuIF が振幅を (v/127)² にする)。
チップの音量レジスタの刻みより細かくは合わないので、刻みの半分+0.5dB を許す。

使い方:
  python levels.py --staging <FITOM_staging> --render <fitom_render.exe>
                   [--emuif <FitomEmuIF.dll>] [--work DIR] [--only 文字列]
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import fitom_measure as fm
from fm_pitch import SINE

# (名前, サブプロファイル, チップ, CC#0, CC#32, プログラム, HwPatch の上書き, 音量の刻み[dB])
# 刻みは、レジスタ1段あたりの dB(対数のチップ)。リニア乗算のチップ(SCC/SAA)は4bitで、
# 小さい音ほど1段の dB が大きくなるので、値ごとに計算する(None)。
# 比較用の FM は OPNA。ymfm の OPN2(YM2612)は DAC の不連続を模していて、小さい音で
# ひずみが -30dB 付近に残る。FM のベロシティは音色の VTL で効き方が決まるので測らない。
TARGETS = [
    ("OPNA (control)", "fmemuif_opn_profile.json", "OPNA", 17, 0, 0, SINE["opn"], 0.75),
    ("SSG", "fmemuif_msx.profile.json", "SSG", 64, 0, 0, None, 3.0),
    ("DCSG", "fmemuif_ibmpc.profile.json", "DCSG", 66, 0, 12, None, 2.0),
    ("SCC", "fmemuif_psg_stereo.profile.json", "SCC", 72, 2, 0, None, None),
    ("SAA1099", "fmemuif_ibmpc.profile.json", "SAA", 67, 0, 24, None, None),
]
VALUES = [127, 112, 96, 80, 64, 48, 32, 16]
NOTE = 69
NOTE_ON, NOTE_OFF = 0.3, 0.9
WINDOW = (0.35, 0.6)
NO_VIBRATO = {"sw": {"depth_cents": 0, "LFR": 0}, "fine_transpose": 0}


def master_volume(v):
    return bytes([0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, v, 0xF7])


def expected_db(v):
    return 40 * math.log10(v / 127)


def linear_step_db(v, levels=15):
    """4bit のリニア乗算で、値 v に相当する段の上下1段の dB 幅。"""
    amp = (v / 127) ** 2          # 40·log10 の目盛りを振幅に戻す
    n = max(1, round(amp * levels))
    return 20 * math.log10((n + 1) / n) if n < levels else 20 * math.log10(levels / (levels - 1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--emuif")
    ap.add_argument("--work")
    ap.add_argument("--only")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="levels_"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"work: {work}")
    print("value:      " + "".join(f"{v:>8}" for v in VALUES))
    print("expected:   " + "".join(f"{expected_db(v):8.2f}" for v in VALUES))
    failures = 0

    for ti, (name, sub, chip_name, cc0, bank, prog, hw, step) in enumerate(TARGETS):
        if args.only and args.only not in name:
            continue
        engine, chip = staging.find_chip(sub, chip_name)
        prof = fm.write_single_chip_profile(staging, sub, engine, chip, work, f"l{ti}")
        print(name)
        kinds = ("CC#7", "master volume") if hw is not None else ("CC#7", "velocity", "master volume")
        for kind in kinds:
            row, marks = [], []
            ref = None
            for v in VALUES:
                ev = [(0.0, m) for m in fm.select_device_patch(0, cc0, bank, prog)]
                if hw is not None:
                    ev.append((0.05, fm.hwpatch_override(0, hw)))
                ev.append((0.05, fm.swpatch_override(0, NO_VIBRATO)))
                vel = 127
                if kind == "CC#7":
                    ev.append((0.1, fm.cc(0, 7, v)))
                elif kind == "velocity":
                    vel = v
                else:
                    ev.append((0.1, master_volume(v)))
                ev += [(NOTE_ON, fm.note_on(0, NOTE, vel)), (NOTE_OFF, fm.note_off(0, NOTE))]
                tag = f"l{ti}_{kind[:3].strip('#')}{v}"
                mid, wav = work / f"{tag}.mid", work / f"{tag}.wav"
                fm.write_smf(mid, [ev])
                fm.run_render(args.render, prof, mid, wav)
                rate, x = fm.read_wav(wav)
                level = fm.rms_db(fm.segment(x, rate, *WINDOW))
                if ref is None:
                    ref = level
                d = level - ref
                row.append(d)
                s = step if step is not None else linear_step_db(v)
                # マスターボリュームはチップのレジスタを通らず、プラグインの出力に掛かる
                tol = 0.5 if kind == "master volume" else s / 2 + 0.5
                ok = abs(d - expected_db(v)) <= tol
                marks.append("" if ok else "*")
                failures += 0 if ok else 1
            print(f"  {kind:10}" + "".join(f"{d:7.2f}{m or ' '}" for d, m in zip(row, marks)))
    print("(* = 期待値から、レジスタの刻みの半分+0.5dB 以上ずれている)")
    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
