"""モノ・レガート・チャンネルの再利用まわりの発音の振る舞いを測る。

  first note      モノ(CC#126=1)で最初のノートが鳴る
  chip change     モノで前のノートを押さえたまま、鳴らすチップが変わる音色へプログラムチェンジし、
                  次のノートを鳴らす。次のノートが新しいチップで鳴り(OPNA を左・OPM を右に定位させて
                  見分ける)、前のノートの周波数が残らない
  vibrato repress モノで、LFS=0 のビブラートの音色を弾き直しても、2秒を過ぎてビブラートが続く
  legato          減衰する音色で A から B へつなぐ。レガートオンでは B の頭で音量が跳ね上がらない
                  (再トリガーされない)。レガートオフでは跳ね上がる(対照)
  poly reuse      ポリで、6チャンネルを埋めてから1つを離し、2秒以内に次のノートを鳴らす
                  (リリース中のチャンネルが必ず再利用される)。LFS=0 のビブラートが2秒を過ぎても続く

first note・chip change・vibrato repress はレガートのオン・オフの両方で測る。
音は OPNA と OPM(FITOM_staging の pc88 のサブプロファイル、YMFMEngine)で、基音だけの音色に上書きして鳴らす。

使い方:
  python behavior.py --staging <FITOM_staging> --render <fitom_render.exe>
                     [--emuif <FitomEmuIF.dll>] [--work DIR]
"""

import argparse
import sys
import tempfile
from pathlib import Path

import numpy as np

import fitom_measure as fm
from fm_pitch import SINE

SUB = "fmemuif_pc88.profile.json"
OPNA, OPM = (17, 0, 0), (25, 0, 0)
NO_VIBRATO = {"sw": {"depth_cents": 0, "LFR": 0}, "fine_transpose": 0}
VIBRATO = {"sw": {"LWF": 6, "LFS": 0, "LFM": 0, "LFD": 0, "LFR": 60, "LFI": 0, "depth_cents": 100},
           "fine_transpose": 0}
# キャリアだけ減衰させる(DR>0、SL=15)。再トリガーされると B の頭で音量が戻る
DECAY = dict(SINE["opn"], ops=SINE["opn"]["ops"][:3] + [dict(SINE["opn"]["ops"][3], DR=12, SL=15)])
SILENCE_DB = -50.0


def select(t, ch, dev, hw, sw):
    ev = [(t, m) for m in fm.select_device_patch(ch, *dev)]
    ev += [(t + 0.01, fm.hwpatch_override(ch, hw)), (t + 0.01, fm.swpatch_override(ch, sw))]
    return ev


def mono(t, ch, legato):
    return [(t, fm.cc(ch, 126, 1)), (t, fm.cc(ch, 68, 127 if legato else 0))]


def vibrato_range(seg, rate, center):
    _, track = fm.pitch_track(seg, rate, center / 1.3, center * 1.3)
    dev = np.array([fm.cents(f, center) for f in track])
    return (np.percentile(dev, 98) - np.percentile(dev, 2)) / 2


class Runner:
    def __init__(self, args, staging, work):
        self.args, self.work = args, work
        engine_a, opna = staging.find_chip(SUB, "OPNA")
        engine_m, opm = staging.find_chip(SUB, "OPM")
        assert engine_a is engine_m or engine_a == engine_m
        # どちらのチップで鳴ったかを左右で見分けるため、OPNA を左だけ・OPM を右だけにする
        # (サブプロファイルの pan: 1=L のみ、2=R のみ)
        self.two = fm.write_engine_profile(staging, SUB, engine_a, [dict(opna, pan=1), dict(opm, pan=2)],
                                           work, "two")
        self.one = fm.write_engine_profile(staging, SUB, engine_a, [opna], work, "one")
        self.failures = 0

    def render(self, tag, events, profile=None):
        mid, wav = self.work / f"{tag}.mid", self.work / f"{tag}.wav"
        fm.write_smf(mid, [events])
        fm.run_render(self.args.render, profile or self.two, mid, wav)
        return fm.read_wav(wav)

    def report(self, label, measured, ok):
        self.failures += 0 if ok else 1
        print(f"  {label:34} {measured:42}{'' if ok else '  <-- NG'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--emuif")
    ap.add_argument("--work")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="behavior_"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"work: {work}")
    r = Runner(args, staging, work)

    for legato in (False, True):
        lg = "legato on " if legato else "legato off"
        print(f"mono, {lg}")

        ev = mono(0.0, 0, legato) + select(0.02, 0, OPNA, SINE["opn"], NO_VIBRATO)
        ev += [(0.3, fm.note_on(0, 69)), (0.9, fm.note_off(0, 69))]
        rate, x = r.render(f"first_{legato}", ev)
        level = fm.rms_db(fm.segment(x, rate, 0.35, 0.6))
        r.report("first note", f"{level:6.1f} dB", level > SILENCE_DB)

        # A=220Hz を OPNA で押さえたまま OPM へ切り替え、B=659Hz を鳴らす
        ev = mono(0.0, 0, legato) + select(0.02, 0, OPNA, SINE["opn"], NO_VIBRATO)
        ev += [(0.3, fm.note_on(0, 57))]
        ev += select(1.0, 0, OPM, SINE["opm"], NO_VIBRATO)
        ev += [(1.2, fm.note_on(0, 76)), (2.0, fm.note_off(0, 57)), (2.5, fm.note_off(0, 76))]
        rate, x = r.render(f"chip_{legato}", ev)
        seg = fm.segment(x, rate, 1.4, 1.9)
        band_b = (fm.note_freq(76) / 1.06, fm.note_freq(76) * 1.06)
        band_a = (fm.note_freq(57) / 1.06, fm.note_freq(57) * 1.06)
        _, lb_l = fm.peak_freq(seg[:, 0], rate, *band_b)
        _, lb_r = fm.peak_freq(seg[:, 1], rate, *band_b)
        _, la = fm.peak_freq(seg, rate, *band_a)
        r.report("chip change (B on OPM=R, A gone)", f"B L {lb_l:6.1f} R {lb_r:6.1f} dB, A {la:6.1f} dB",
                 fm.rms_db(seg) > SILENCE_DB and lb_r >= lb_l + 20 and la <= lb_r - 40)

        ev = mono(0.0, 0, legato) + select(0.02, 0, OPNA, SINE["opn"], VIBRATO)
        ev += [(0.3, fm.note_on(0, 69)), (0.8, fm.note_off(0, 69)),
               (1.0, fm.note_on(0, 69)), (5.0, fm.note_off(0, 69))]
        rate, x = r.render(f"vib_{legato}", ev)
        early = vibrato_range(fm.segment(x, rate, 1.3, 2.8), rate, fm.note_freq(69))
        late = vibrato_range(fm.segment(x, rate, 3.3, 4.8), rate, fm.note_freq(69))
        r.report("vibrato repress (+0.3-1.8s / +2.3-3.8s)", f"±{early:5.1f} c / ±{late:5.1f} c",
                 early >= 50 and late >= 50)

        ev = mono(0.0, 0, legato) + select(0.02, 0, OPNA, DECAY, NO_VIBRATO)
        ev += [(0.3, fm.note_on(0, 69)), (1.0, fm.note_on(0, 72)), (1.05, fm.note_off(0, 69)),
               (2.0, fm.note_off(0, 72))]
        rate, x = r.render(f"legato_{legato}", ev)
        peak = fm.rms_db(fm.segment(x, rate, 0.32, 0.4))
        before = fm.rms_db(fm.segment(x, rate, 0.92, 0.99))
        after = fm.rms_db(fm.segment(x, rate, 1.03, 1.10))
        jump = after - before
        decayed = peak - before >= 6
        ok = decayed and (jump <= 1.0 if legato else jump >= 3.0)
        r.report("A->B " + ("no retrigger" if legato else "retrigger (control)"),
                 f"decayed {peak - before:5.1f} dB, jump {jump:+5.1f} dB", ok)

    print("poly, release reuse")
    ev = select(0.0, 0, OPNA, SINE["opn"], VIBRATO)
    held = [36, 38, 40, 41, 43]
    ev += [(0.3, fm.note_on(0, n)) for n in held]
    ev += [(0.3, fm.note_on(0, 45)), (1.0, fm.note_off(0, 45)), (1.5, fm.note_on(0, 81))]
    ev += [(5.5, fm.note_off(0, n)) for n in held + [81]]
    rate, x = r.render("poly_reuse", ev, r.one)
    early = vibrato_range(fm.segment(x, rate, 1.8, 3.3), rate, fm.note_freq(81))
    late = vibrato_range(fm.segment(x, rate, 4.0, 5.4), rate, fm.note_freq(81))
    r.report("vibrato on reused ch (+0.3-1.8s / +2.5-3.9s)", f"±{early:5.1f} c / ±{late:5.1f} c",
             early >= 50 and late >= 50)

    print(f"NG: {r.failures}")
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
