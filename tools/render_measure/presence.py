"""チップ・部位ごとに、音が鳴るか(と、音程のある音なら A4 の音程)を測る。

単体チップのプロファイルで、直接デバイス選択(または内蔵リズム)で選んだ音色を1音鳴らし、
発音直後の RMS が閾値を超えるかを見る。expected_hz を持つ場合は基音も測る。
compare を持つ場合は、別の場合のスペクトルの最大の山との比(セント)を出す
(同じ波形を別のエンジンで鳴らして、再生の速さが揃っているかを見る)。

使い方:
  python presence.py --staging <FITOM_staging> --render <fitom_render.exe>
                     [--emuif <FitomEmuIF.dll>] [--work DIR] [--only 文字列]
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import fitom_measure as fm

A4 = 440.0

# key: (サブプロファイル, チップ, rhythm_mode, (CC#0, CC#32, プログラム), ノート, expected_hz, compare)
# PSG 系は1つのバンク番号空間を共有する。バンク 0 番の 0 番が SSG、2 番(scc_preset_1)の 0 番が SCC の音色。
# PCM 系の CC#32 は、バンクセットの pcm_banks の bank 番号(ADPCMB=0、SSGS_ADPCM=3)。
CASES = {
    "SSGS SSG": ("fmemuif_psg_stereo.profile.json", "SSGS", False, (64, 0, 0), 69, A4, None),
    "SSGS2 SSG": ("fmemuif_psg_stereo.profile.json", "SSGS2", False, (64, 0, 0), 69, A4, None),
    "SSGS3 SSG": ("fmemuif_psg_stereo.profile.json", "SSGS3", False, (64, 0, 0), 69, A4, None),
    "Y8960 SSGEX": ("fmemuif_msx2pp.profile.json", "SSGEX", False, (64, 0, 0), 69, A4, None),
    "SCC (psg_stereo)": ("fmemuif_psg_stereo.profile.json", "SCC", False, (72, 2, 0), 69, A4, None),
    "Y8960 SCCEX": ("fmemuif_msx2pp.profile.json", "SCCEX", False, (72, 2, 0), 69, A4, None),
    "DSG prog 0": ("fmemuif_psg_stereo.profile.json", "DSG", False, (68, 0, 0), 69, A4, None),
    "DSG rhythm 0": ("fmemuif_psg_stereo.profile.json", "DSG", False, (0x70, 0x44, 0), 60, None, None),
    "DSG rhythm 1": ("fmemuif_psg_stereo.profile.json", "DSG", False, (0x70, 0x44, 1), 60, None, None),
    "DSG rhythm 2": ("fmemuif_psg_stereo.profile.json", "DSG", False, (0x70, 0x44, 2), 60, None, None),
    "DSG rhythm 3": ("fmemuif_psg_stereo.profile.json", "DSG", False, (0x70, 0x44, 3), 60, None, None),
    "DSG rhythm 4": ("fmemuif_psg_stereo.profile.json", "DSG", False, (0x70, 0x44, 4), 60, None, None),
    "SSGS ADPCM kick": ("fmemuif_psg_stereo.profile.json", "SSGS", False, (85, 3, 37), 69, None, None),
    "SSGS ADPCM snare": ("fmemuif_psg_stereo.profile.json", "SSGS", False, (85, 3, 28), 69, None, None),
    "SSGS2 ADPCM kick": ("fmemuif_psg_stereo.profile.json", "SSGS2", False, (85, 3, 37), 69, None, None),
    "SSGS2 ADPCM snare": ("fmemuif_psg_stereo.profile.json", "SSGS2", False, (85, 3, 28), 69, None, None),
    "Y8950 ADPCM-B (YMFM)": ("fmemuif_opl5.profile.json", "Y8950", False, (81, 0, 9), 60, None, None),
    "Y8960 OPL2EX ADPCM-B": ("fmemuif_msx2pp.profile.json", "OPL2EX", False, (81, 0, 9), 60, None,
                             "Y8950 ADPCM-B (YMFM)"),
    "Y8950 rhythm 0 (YMFM)": ("fmemuif_opl5.profile.json", "Y8950", True, (35, 0, 0), 60, None, None),
    "Y8960 OPL2EX rhythm 0": ("fmemuif_msx2pp.profile.json", "OPL2EX", True, (35, 0, 0), 60, None, None),
    "Y8960 OPL2EX rhythm 1": ("fmemuif_msx2pp.profile.json", "OPL2EX", True, (35, 0, 1), 60, None, None),
    "Y8960 OPL2EX rhythm 2": ("fmemuif_msx2pp.profile.json", "OPL2EX", True, (35, 0, 2), 60, None, None),
    "Y8960 OPL2EX rhythm 3": ("fmemuif_msx2pp.profile.json", "OPL2EX", True, (35, 0, 3), 60, None, None),
    "Y8960 OPL2EX rhythm 4": ("fmemuif_msx2pp.profile.json", "OPL2EX", True, (35, 0, 4), 60, None, None),
}

NOTE_ON, NOTE_OFF = 0.3, 1.3
SILENCE_DB = -60.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--emuif")
    ap.add_argument("--work")
    ap.add_argument("--only")
    ap.add_argument("--tolerance", type=float, default=5.0, help="音程の合格とする誤差(セント)")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="presence_"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"work: {work}")
    print(f"{'case':24} {'peak dB':>8} {'rms dB':>8} {'peak Hz':>9} {'cents':>8}  note")
    failures = 0
    peaks = {}
    for ci, (label, (sub, chip_name, rhythm, (cc0, bank, prog), note, expected_hz, compare)) \
            in enumerate(CASES.items()):
        if args.only and args.only not in label:
            continue
        engine, chip = staging.find_chip(sub, chip_name)
        tag = f"p{ci}"
        prof = fm.write_single_chip_profile(staging, sub, engine, chip, work, tag, rhythm=rhythm)
        ev = [(0.0, m) for m in fm.select_device_patch(0, cc0, bank, prog)]
        ev += [(NOTE_ON, fm.note_on(0, note, 127)), (NOTE_OFF, fm.note_off(0, note))]
        mid, wav = work / f"{tag}.mid", work / f"{tag}.wav"
        fm.write_smf(mid, [ev])
        try:
            fm.run_render(args.render, prof, mid, wav)
        except RuntimeError as e:
            failures += 1
            print(f"{label:24} ERROR {str(e).splitlines()[0]}  <-- NG")
            continue
        rate, x = fm.read_wav(wav)
        head = fm.segment(x, rate, NOTE_ON, NOTE_ON + 0.3)
        peak_db = 20 * math.log10(max(float(abs(head).max()), 1e-12))
        level = fm.rms_db(head)
        ok = level > SILENCE_DB
        extra = ""
        f, _ = fm.peak_freq(head, rate, 40.0, 12000.0)
        peaks[label] = f
        cents = ""
        if expected_hz is not None and ok:
            seg = fm.segment(x, rate, NOTE_ON + 0.1, NOTE_ON + 0.9)
            f, _ = fm.peak_freq(seg, rate, expected_hz / 1.5, expected_hz * 1.5)
            c = fm.cents(f, expected_hz)
            cents = f"{c:+8.2f}"
            if abs(c) > args.tolerance:
                ok = False
        if compare is not None and compare in peaks and ok:
            c = fm.cents(f, peaks[compare])
            extra = f"vs {compare}: {c:+.2f} cents"
        failures += 0 if ok else 1
        print(f"{label:24} {peak_db:8.1f} {level:8.1f} {f:9.1f} {cents:>8}  {extra}{'' if ok else '  <-- NG'}")
    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
