"""AY8930(EPSG)の拡張モードのバンク切り替えを、3チャンネルの音程とデューティ比の効きで測る。

CEPSG は拡張モードを常に使い、ch0 は Bank A、ch1/ch2 は Bank B にエンベロープとデューティ比の
レジスタを持つ。バンクの切り替えを誤ると、デューティ比が効かないか、別のレジスタを壊して
音程や音量が崩れる。3音を同時に鳴らして3チャンネルを全部使い、デューティ比を変えて、
各ノートの基音の音程と、第2倍音と基音の比をパルス波の理論値(|sin(2πd)/2| / |sin(πd)|)と比べる。

使い方:
  python epsg_expanded.py --staging <FITOM_staging> --render <fitom_render.exe>
                          [--emuif <FitomEmuIF.dll>] [--work DIR]
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import fitom_measure as fm

SUB = "fmemuif_psg_stereo.profile.json"
# PSG 系は1つのバンク番号空間を共有する。バンク 1(epsg_preset)の 0 番が EPSG の音色
SELECT = (65, 1, 0)
# 基音と第2倍音が互いに重ならないノート
NOTES = [57, 62, 66]
# AY8930 のデューティ比の符号(MAME の ay8910 の duty_cycle[] と同じ並び)
DUTIES = {4: 0.5, 3: 0.25, 2: 0.125}
NO_VIBRATO = {"sw": {"depth_cents": 0, "LFR": 0}, "fine_transpose": 0}


def expected_h2_db(d):
    h1 = abs(math.sin(math.pi * d))
    h2 = abs(math.sin(2 * math.pi * d)) / 2
    return 20 * math.log10(h2 / h1) if h2 > 1e-9 else -math.inf


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--emuif")
    ap.add_argument("--work")
    ap.add_argument("--tolerance", type=float, default=5.0, help="音程の合格とする誤差(セント)")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="epsg_"))
    work.mkdir(parents=True, exist_ok=True)
    engine, chip = staging.find_chip(SUB, "EPSG")
    prof = fm.write_single_chip_profile(staging, SUB, engine, chip, work, "epsg")
    print(f"work: {work}")
    print(f"{'duty':>6} {'note':>4} {'cents':>8} {'H2-H1 dB':>9} {'expected':>9}")
    failures = 0
    for code, d in DUTIES.items():
        hw = {"ops": [{"WS": code, "AR": 31, "DR": 0, "SL": 0, "SR": 0, "RR": 8}]}
        ev = [(0.0, m) for m in fm.select_device_patch(0, *SELECT)]
        ev += [(0.05, fm.hwpatch_override(0, hw)), (0.05, fm.swpatch_override(0, NO_VIBRATO))]
        ev += [(0.3, fm.note_on(0, n, 127)) for n in NOTES]
        ev += [(1.3, fm.note_off(0, n)) for n in NOTES]
        mid, wav = work / f"duty{code}.mid", work / f"duty{code}.wav"
        fm.write_smf(mid, [ev])
        fm.run_render(args.render, prof, mid, wav)
        rate, x = fm.read_wav(wav)
        seg = fm.segment(x, rate, 0.4, 1.2)
        exp = expected_h2_db(d)
        for n in NOTES:
            f0 = fm.note_freq(n)
            f1, l1 = fm.peak_freq(seg, rate, f0 / 1.03, f0 * 1.03)
            _, l2 = fm.peak_freq(seg, rate, 2 * f0 / 1.03, 2 * f0 * 1.03)
            c = fm.cents(f1, f0)
            h2 = l2 - l1
            ok = abs(c) <= args.tolerance and (h2 <= -25 if exp == -math.inf else abs(h2 - exp) <= 1.5)
            failures += 0 if ok else 1
            exps = "-inf" if exp == -math.inf else f"{exp:9.2f}"
            print(f"{d * 100:5.1f}% {n:4} {c:+8.2f} {h2:9.2f} {exps:>9}{'' if ok else '  <-- NG'}")
    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
