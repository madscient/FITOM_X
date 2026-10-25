"""OPLL系の内蔵リズムの SD/TOM が、ノート番号どおりの音程で上下するかを測る。

内蔵リズム(CC#0=0x70、CC#32=0x28)で SD/TOM を、ノート番号を変えて鳴らし、スペクトルの
最大の山の周波数を、同じノートのメロディ(基音だけの音色)の基音で割る。リズムがメロディ側と
同じ F-number で鳴っていれば、この比はノートによらず一定(楽器の MUL)になる。
基準のノートの比からのずれ(セント)で判定する。

スペクトル全体の相互相関でずれを求める方法は使わない。SD のノイズが強いエンジンでは、
ずれないノイズの成分で相関の山が決まってしまう。

使い方:
  python rhythm_pitch.py --staging <FITOM_staging> --render <fitom_render.exe>
                         [--emuif <FitomEmuIF.dll>] [--work DIR]
"""

import argparse
import sys
import tempfile
from pathlib import Path

import fitom_measure as fm
from fm_pitch import SINE

# (名前, サブプロファイル, チップ)
TARGETS = [
    ("OPLL", "fmemuif_opll5.profile.json", "OPLL"),
    ("OPLLEX (opll5)", "fmemuif_opll5.profile.json", "OPLLEX"),
    ("OPLLEX (msx2pp)", "fmemuif_msx2pp.profile.json", "OPLLEX"),
]
# 内蔵リズムの楽器番号(ドラムキット opll_rhythm と同じ)
INSTRUMENTS = [("TOM", 2), ("SD", 3)]
NOTES = [36, 48, 55, 60, 72]
REF_NOTE = 48
NOTE_ON, NOTE_OFF = 0.3, 0.8
WINDOW = (0.32, 0.62)


def render_note(args, work, prof, tag, note, rhythm_prog=None):
    ch = 0
    if rhythm_prog is None:
        ev = [(0.0, m) for m in fm.select_device_patch(ch, 40, 2, 0)]
        ev.append((0.05, fm.hwpatch_override(ch, SINE["opll"])))
        ev.append((0.05, fm.swpatch_override(ch, {"sw": {"depth_cents": 0, "LFR": 0}, "fine_transpose": 0})))
    else:
        ev = [(0.0, m) for m in fm.select_device_patch(ch, 0x70, 0x28, rhythm_prog)]
    ev += [(NOTE_ON, fm.note_on(ch, note)), (NOTE_OFF, fm.note_off(ch, note))]
    mid, wav = work / f"{tag}.mid", work / f"{tag}.wav"
    fm.write_smf(mid, [ev])
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
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="rhythm_pitch_"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"work: {work}")
    failures = 0

    for ti, (name, sub, chip_name) in enumerate(TARGETS):
        engine, chip = staging.find_chip(sub, chip_name)
        prof = fm.write_single_chip_profile(staging, sub, engine, chip, work, f"r{ti}", rhythm=True)
        print(f"{name}  ({sub} {chip_name}, rhythm_mode)")
        melody = {}
        for note in NOTES:
            rate, seg = render_note(args, work, prof, f"r{ti}_mel{note}", note)
            f, _ = fm.peak_freq(seg, rate, fm.note_freq(note) / 1.5, fm.note_freq(note) * 1.5)
            melody[note] = f
        for inst, prog in INSTRUMENTS:
            print(f"  {inst}: {'note':>4} {'level dB':>8} {'peak Hz':>9} {'peak/melody':>11} {'vs ref c':>8}")
            ratios = {}
            for note in NOTES:
                rate, seg = render_note(args, work, prof, f"r{ti}_{inst}{note}", note, prog)
                f, _ = fm.peak_freq(seg, rate, 40.0, 12000.0)
                ratios[note] = (f / melody[note], f, fm.rms_db(seg))
            for note in NOTES:
                ratio, f, level = ratios[note]
                dev = fm.cents(ratio, ratios[REF_NOTE][0])
                ok = level > -70 and abs(dev) <= args.tolerance
                failures += 0 if ok else 1
                print(f"       {note:4} {level:8.1f} {f:9.1f} {ratio:11.3f} {dev:+8.2f}{'' if ok else '  <-- NG'}")
    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
