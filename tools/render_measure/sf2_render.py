"""fitom_render で FitomSf2IF(SF2直行パス)と FM を一緒に鳴らし、経路と発音の時刻を測る。

プロファイルには OPNA(左だけに定位)と SF2 を載せ、sf2_channel_windows で MPU1 の ch2 を SF2 へ回す。
SMF は2トラック:
  トラック1  MIDI Port なし(MPU0)、ch1。OPNA の正弦波で A4 を鳴らす
  トラック2  FF 21 01(MPU1)、ch2。SF2 の GM 80番(Square Lead)を右へ定位させて E5 を鳴らす。
             SF2 の窓は CC#32 で SoundFont のバンクが決まるまでプログラムチェンジを読み捨てるので、
             CC#32=0 を先に送る
両方のノートは同じ時刻に置く。

  routing    E5 が右に出る(SF2 で鳴る)。対照として、トラック2の FF 21 を外すと MPU0 へ行き、
             SF2 では鳴らない(MPU0 の ch2 には音色が無いので無音になる)
  onset      左(OPNA)と右(SF2)の立ち上がりの時刻の差。どちらも 64 フレームの格子で送られるので、
             差は音色の立ち上がりの分だけになる(格子に揃えないと、fluidsynth の 64 フレーム単位の
             合成の分だけ SF2 が遅れる)

使い方:
  python sf2_render.py --staging <FITOM_staging> --render <fitom_render.exe>
                       --sf2if <FitomSf2IF.dll> [--emuif <FitomEmuIF.dll>] [--work DIR]
"""

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

import fitom_measure as fm
from fm_pitch import SINE

SUB = "fmemuif_pc88.profile.json"
NOTE_FM, NOTE_SF2 = 69, 76
# 64フレームの格子から外れた時刻に置く。SMF の時刻は1ティック(120BPM・480分解能で約1.04ms)単位なので、
# 0.5秒の3ティック後(48kHz で 24150 フレーム、次の格子 24192 まで 0.875ms)。格子の上だと、イベントを
# 格子に揃えて送る処理が無くても立ち上がりが揃ってしまい、揃えていることを確かめられない
NOTE_ON, NOTE_OFF = 0.5 + 3 * fm.TEMPO_US / fm.PPQN / 1e6, 1.5
ONSET_DB = -60.0


def write_profile(staging, sf2if_dll, work):
    engine, opna = staging.find_chip(SUB, "OPNA")
    path = fm.write_engine_profile(staging, SUB, engine, [dict(opna, pan=1)], work, "sf2")
    prof = json.loads(path.read_text(encoding="utf-8"))
    prof["banks"] = str(fm.write_bankset(staging, work, keep_sf2=True))
    prof["hw_plugins"].append({
        "name": "FitomSf2IF",
        "dll": str(Path(sf2if_dll).resolve()),
        "profile": str(staging.hw_plugins / "fitom_sf2if_profile.json"),
    })
    prof["devices"] = [{"chip": "SF2", "if": "HW", "label": "SF2", "plugin": "FitomSf2IF"}]
    prof["sf2_channel_windows"] = [{"mpu": 1, "ch": 1, "fluidsynth_chan": 0}]
    path.write_text(json.dumps(prof, ensure_ascii=False, indent=2), encoding="utf-8")
    return path


def onset(x, threshold_db, after):
    th = 10 ** (threshold_db / 20)
    idx = np.nonzero(np.abs(x) > th)[0]
    idx = idx[idx >= after]
    return int(idx[0]) if len(idx) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--sf2if", required=True, help="オフライン描画に対応した FitomSf2IF.dll")
    ap.add_argument("--emuif")
    ap.add_argument("--work")
    ap.add_argument("--onset-ms", type=float, default=0.5, help="立ち上がりの差の許容(ミリ秒)")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="sf2_"))
    work.mkdir(parents=True, exist_ok=True)
    prof = write_profile(staging, args.sf2if, work)
    print(f"work: {work}")
    failures = 0

    fm_track = [(0.0, m) for m in fm.select_device_patch(0, 17, 0, 0)]
    fm_track += [(0.05, fm.hwpatch_override(0, SINE["opn"])),
                 (0.05, fm.swpatch_override(0, {"sw": {"depth_cents": 0, "LFR": 0}})),
                 (NOTE_ON, fm.note_on(0, NOTE_FM, 127)), (NOTE_OFF, fm.note_off(0, NOTE_FM))]
    sf2_notes = [(0.0, fm.cc(1, 32, 0)), (0.01, fm.program(1, 80)), (0.01, fm.cc(1, 10, 127)),
                 (NOTE_ON, fm.note_on(1, NOTE_SF2, 127)), (NOTE_OFF, fm.note_off(1, NOTE_SF2))]
    port1 = [(0.0, bytes([0xFF, 0x21, 0x01, 0x01]))]

    for label, track2, expect_sf2 in (("FF 21 01 (MPU1 -> SF2)", port1 + sf2_notes, True),
                                      ("no FF 21 (MPU0, not SF2: control)", sf2_notes, False)):
        mid, wav = work / f"sf2_{expect_sf2}.mid", work / f"sf2_{expect_sf2}.wav"
        fm.write_smf(mid, [fm_track, track2])
        fm.run_render(args.render, prof, mid, wav, dll_dirs=[Path(args.sf2if).parent])
        rate, x = fm.read_wav(wav)
        seg = fm.segment(x, rate, NOTE_ON + 0.2, NOTE_OFF - 0.1)
        band = (fm.note_freq(NOTE_SF2) / 1.03, fm.note_freq(NOTE_SF2) * 1.03)
        _, e5_l = fm.peak_freq(seg[:, 0], rate, *band)
        _, e5_r = fm.peak_freq(seg[:, 1], rate, *band)
        ok = (e5_r >= -40 and e5_r >= e5_l - 20) if expect_sf2 else (e5_r <= -80)
        failures += 0 if ok else 1
        print(f"{label:34} E5 L {e5_l:7.1f} dB  R {e5_r:7.1f} dB{'' if ok else '  <-- NG'}")
        if expect_sf2:
            start = int((NOTE_ON - 0.05) * rate)
            ol, orr = onset(x[:, 0], ONSET_DB, start), onset(x[:, 1], ONSET_DB, start)
            if ol is None or orr is None:
                failures += 1
                print(f"{'onset':34} not found  <-- NG")
            else:
                d = (orr - ol) / rate * 1000
                ok = abs(d) <= args.onset_ms
                failures += 0 if ok else 1
                print(f"{'onset (R SF2 - L OPNA)':34} {d:+7.2f} ms (L {ol / rate * 1000:.2f} ms, "
                      f"R {orr / rate * 1000:.2f} ms){'' if ok else '  <-- NG'}")
    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
