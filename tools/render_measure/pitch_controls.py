"""音程を動かす操作が、チップの音程に正しく効くかを測る。

チップごとに A4 を鳴らし、ピッチベンド・ファインチューン(RPN#1)・ソフトウェアLFOの
ビブラート・マスターピッチを1つずつ変えて、基音のずれ(セント)を何もしないときと比べる。
比べる相手が同じチップの基音なので、サンプルの元の音程が分からない PCM 系でも測れる。
音程が正しいと分かっている FM チップ(OPN2)を、測り方の確認のために並べる。

使い方:
  python pitch_controls.py --staging <FITOM_staging> --render <fitom_render.exe>
                           [--emuif <FitomEmuIF.dll>] [--work DIR]
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import numpy as np

import fitom_measure as fm
from fm_pitch import SINE

# (名前, サブプロファイル, チップ, CC#0, CC#32, プログラム, HwPatch の上書き, 未対応の操作)
# 未対応の操作は、docs/STATUS.md の4節に載っている既知の未対応。測って表示するが不合格に数えない。
TARGETS = [
    ("OPN2 (control)", "fmemuif_opn_profile.json", "OPN2", 17, 0, 0, SINE["opn"], set()),
    # PSG バンク 24 番は SAA1099 向けの矩形波
    ("SAA1099", "fmemuif_ibmpc.profile.json", "SAA", 67, 0, 24, None, set()),
    ("SAA1099 (psg_stereo)", "fmemuif_psg_stereo.profile.json", "SAA", 67, 0, 24, None, set()),
    # PSG バンク 0 番は SSG 向けの矩形波。SAA1099 と同じ PSG 系の比較用
    ("SSG", "fmemuif_msx.profile.json", "SSG", 64, 0, 0, None, set()),
    # AWM の GM バンク 16 番(Drawbar Organ)。持続する音で基音が安定している
    # AWM はマスターピッチを読まず、チャンネルLFO(ビブラート)を動かさない
    ("OPL4 AWM", "fmemuif_opl5.profile.json", "OPL4", 84, 0, 16, None, {"master_pitch", "vibrato"}),
]

NOTE = 69
NOTE_ON, NOTE_OFF = 0.5, 2.0
WINDOW = (0.8, 1.9)
NO_VIBRATO = {"sw": {"depth_cents": 0, "LFR": 0}, "fine_transpose": 0}
# LFR=0 の音色は CC#1 でビブラートの深さを決める。深さの上限は RPN#5(セント)
MOD_WHEEL = {"sw": {"LWF": 6, "LFS": 1, "LFM": 0, "LFD": 0, "LFR": 0, "LFI": 0, "depth_cents": 0},
             "fine_transpose": 0}
VIBRATO = {"sw": {"LWF": 6, "LFS": 1, "LFM": 0, "LFD": 0, "LFR": 60, "LFI": 0, "depth_cents": 100},
           "fine_transpose": 0}

# (名前, 発音前に送る列, 期待するずれ[セント])。ベンド幅は2半音にしてから送る。
STEADY_CASES = [
    ("base", [], 0.0),
    ("bend +4096 (1 semitone)", [fm.pitch_bend(0, 4096)], 100.0),
    ("bend +8191 (2 semitones)", [fm.pitch_bend(0, 8191)], 200.0 * 8191 / 8192),
    ("bend -8192 (-2 semitones)", [fm.pitch_bend(0, -8192)], -200.0),
    ("fine tune +50", fm.fine_tune(0, 50), 50.0),
    ("fine tune -50", fm.fine_tune(0, -50), -50.0),
]
MASTER_PITCH_CASES = [430.0, 450.0]


def build_smf(path, target, pre, sw):
    _, _, _, cc0, bank, prog, hw, _ = target
    ch = 0
    ev = [(0.0, m) for m in fm.select_device_patch(ch, cc0, bank, prog)]
    ev += [(0.02, m) for m in fm.rpn(ch, 0, 0, 2)]
    if hw is not None:
        ev.append((0.05, fm.hwpatch_override(ch, hw)))
    ev.append((0.05, fm.swpatch_override(ch, sw)))
    ev += [(0.3, m) for m in pre]
    ev += [(NOTE_ON, fm.note_on(ch, NOTE)), (NOTE_OFF, fm.note_off(ch, NOTE))]
    fm.write_smf(path, [ev])


def render(args, staging, work, target, tag, pre, sw, master_pitch=440.0):
    name, sub, chip_name, *_ = target
    engine, chip = staging.find_chip(sub, chip_name)
    prof = fm.write_single_chip_profile(staging, sub, engine, chip, work, tag, master_pitch=master_pitch)
    mid, wav = work / f"{tag}.mid", work / f"{tag}.wav"
    build_smf(mid, target, pre, sw)
    fm.run_render(args.render, prof, mid, wav)
    rate, x = fm.read_wav(wav)
    return rate, fm.segment(x, rate, *WINDOW)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--emuif")
    ap.add_argument("--work")
    ap.add_argument("--tolerance", type=float, default=5.0, help="合格とする誤差(セント)")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="pitch_controls_"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"work: {work}")
    failures = 0

    def report(label, measured, expected, ok, unsupported=False):
        nonlocal failures
        if unsupported:
            mark = "  (unsupported)"
        else:
            failures += 0 if ok else 1
            mark = "" if ok else "  <-- NG"
        exp = "" if expected is None else f"{expected:+8.2f}"
        print(f"  {label:28} {measured:>24} {exp:>9}{mark}")

    for ti, target in enumerate(TARGETS):
        name = target[0]
        print(f"{name}  ({target[1]} {target[2]}, CC#0={target[3]} CC#32={target[4]} prog={target[5]})")
        print(f"  {'case':28} {'measured':>24} {'expected':>9}")
        base = None
        for ci, (label, pre, expected) in enumerate(STEADY_CASES):
            rate, seg = render(args, staging, work, target, f"t{ti}_c{ci}", pre, NO_VIBRATO)
            level = fm.rms_db(seg)
            fmin, fmax = fm.note_freq(NOTE - 4) / 2, fm.note_freq(NOTE + 4) * 2
            f, _ = fm.peak_freq(seg, rate, fmin, fmax)
            if base is None:
                base = f
                ok = level > -60
                report(label, f"{f:9.3f} Hz {level:6.1f} dB", None, ok)
                print(f"  {'(from A4=440Hz)':28} {fm.cents(f, fm.note_freq(NOTE)):+8.2f} cents")
                if not ok:
                    break
                continue
            c = fm.cents(f, base)
            report(label, f"{c:+8.2f} cents", expected, abs(c - expected) <= args.tolerance)
        if base is None:
            continue

        for mp in MASTER_PITCH_CASES:
            rate, seg = render(args, staging, work, target, f"t{ti}_mp{int(mp)}", [], NO_VIBRATO, mp)
            f, _ = fm.peak_freq(seg, rate, base / 1.2, base * 1.2)
            expected = 1200 * math.log2(mp / 440.0)
            c = fm.cents(f, base)
            report(f"master_pitch {mp:.0f}", f"{c:+8.2f} cents", expected, abs(c - expected) <= args.tolerance,
                   "master_pitch" in target[7])

        vib_cases = [
            (f"vibrato (SwPatch) {VIBRATO['sw']['depth_cents']}", [], VIBRATO, VIBRATO["sw"]["depth_cents"]),
            ("vibrato (CC#1=127, RPN#5=100)", fm.rpn(0, 0, 5, 100) + [fm.cc(0, 1, 127)], MOD_WHEEL, 100),
        ]
        for vi, (label, pre, sw, depth) in enumerate(vib_cases):
            rate, seg = render(args, staging, work, target, f"t{ti}_vib{vi}", pre, sw)
            _, track = fm.pitch_track(seg, rate, base / 1.5, base * 1.5)
            dev = np.array([fm.cents(f, base) for f in track])
            half_range = (np.percentile(dev, 98) - np.percentile(dev, 2)) / 2
            crossings = np.count_nonzero(np.diff(np.sign(dev - np.median(dev))) != 0)
            lfo_hz = crossings / 2 / (WINDOW[1] - WINDOW[0])
            report(label, f"±{half_range:6.1f} c {lfo_hz:4.1f} Hz", float(depth),
                   0.5 * depth <= half_range <= 1.5 * depth, "vibrato" in target[7])

    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
