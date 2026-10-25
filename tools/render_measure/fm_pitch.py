"""FM系チップの音程を測る。

ステージングの FitomEmuIF のサブプロファイルに載っている FM系チップを1個ずつ、
同じエンジン・同じクロックで鳴らし、A4(既定)の基音が master_pitch=440Hz に
合っているかを見る。音色は直接デバイス選択で選んだうえで、HwPatch の上書きで
「全オペレータ MUL=1・デチューン0・フィードバック0」に変え、基音だけが出るようにする
(元の音色の MUL やデチューンで基音がずれるのを避けるため)。

使い方:
  python fm_pitch.py --staging <FITOM_staging> --render <fitom_render.exe> [--emuif <FitomEmuIF.dll>]
                     [--work DIR] [--note 69]
"""

import argparse
import sys
import tempfile
from pathlib import Path

import fitom_measure as fm

# 1オペレータだけを TL 最小(最大音量)にし、残りは最大減衰。どのオペレータも MUL=1 なので、
# どれが鳴っても基音は同じ。
_OPN_OP = {"MUL": 1, "DT1": 0, "AR": 31, "DR": 0, "SR": 0, "SL": 0, "RR": 15, "AM": 0, "KSR": 0}
_OPM_OP = dict(_OPN_OP, DT2=0)
_OPZ_OP = dict(_OPM_OP, WS=0)
_OPL_OP = {"MUL": 1, "AR": 31, "DR": 0, "SR": 0, "SL": 0, "RR": 15, "AM": 0, "VIB": 0, "KSR": 0, "KSL": 0, "WS": 0}


def _four_op(op, tl_off):
    return [dict(op, TL=tl_off), dict(op, TL=tl_off), dict(op, TL=tl_off), dict(op, TL=0)]


SINE = {
    "opn": {"FB": 0, "ALG": 7, "AMS": 0, "PMS": 0, "ops": _four_op(_OPN_OP, 127)},
    "opm": {"FB": 0, "ALG": 7, "AMS": 0, "PMS": 0, "ops": _four_op(_OPM_OP, 127)},
    "opz": {"FB": 0, "ALG": 7, "AMS": 0, "PMS": 0, "ops": _four_op(_OPZ_OP, 127)},
    # OPL は ALG=1(加算)。モジュレータは最大減衰。
    "opl": {"FB": 0, "ALG": 1, "ops": [dict(_OPL_OP, TL=63), dict(_OPL_OP, TL=0)]},
    # OPLL は FM のみ。モジュレータの TL=63 は -47.25dB で、変調はわずかに残る。
    "opll": {"FB": 0, "ops": [dict(_OPL_OP, TL=63), dict(_OPL_OP, TL=0)]},
}

# チップ名 → (直接デバイス選択の CC#0, CC#32, 音色の種類)。バンクはステージングの
# unified.bankset.json に登録されている番号(OPLL 系は 0 番が無い)。
# 単体チップのプロファイルでは、そのチップが再生できる別系統のバンクへフォールバックする。
FM_CHIPS = {
    "OPN": (16, 0, "opn"),
    "OPN2": (17, 0, "opn"), "OPNA": (17, 0, "opn"), "OPNB": (17, 0, "opn"), "OPNBB": (17, 0, "opn"),
    "OPM": (25, 0, "opm"),
    "OPZ": (26, 0, "opz"),
    "Y8950": (32, 0, "opl"), "OPL2": (33, 0, "opl"), "OPL2EX": (33, 0, "opl"),
    "OPL3": (34, 0, "opl"), "OPL4": (34, 0, "opl"),
    "OPLL": (40, 2, "opll"), "OPLLP": (41, 2, "opll"), "OPLLX": (42, 2, "opll"), "VRC7": (43, 2, "opll"),
    "OPLLEX": (40, 2, "opll"),
}

NOTE_START = 0.5
NOTE_LEN = 1.5


def build_smf(path, cc0, bank, kind, note):
    ch = 0
    ev = [(0.0, m) for m in fm.select_device_patch(ch, cc0, bank)]
    ev += [(0.05, fm.hwpatch_override(ch, SINE[kind])),
           (0.05, fm.swpatch_override(ch, {"sw": {"depth_cents": 0}, "fine_transpose": 0})),
           (0.05, fm.cc(ch, 7, 100)),
           (NOTE_START, fm.note_on(ch, note)),
           (NOTE_START + NOTE_LEN, fm.note_off(ch, note))]
    fm.write_smf(path, [ev])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--render", required=True)
    ap.add_argument("--emuif", help="FitomEmuIF.dll(既定はステージングの bin/ のもの)")
    ap.add_argument("--work")
    ap.add_argument("--note", type=int, default=69)
    ap.add_argument("--rate", type=int, default=48000, help="fitom_render の出力サンプルレート")
    ap.add_argument("--only", help="この文字列を含むチップ名・サブプロファイル名だけを測る")
    ap.add_argument("--tolerance", type=float, default=5.0, help="合格とする誤差(セント)")
    args = ap.parse_args()

    staging = fm.Staging(args.staging, args.emuif)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="fm_pitch_"))
    expected = fm.note_freq(args.note)
    print(f"work: {work}")
    print(f"note {args.note}: expected {expected:.3f} Hz (master_pitch 440), rate {args.rate} Hz")
    print(f"{'subprofile':42} {'engine':16} {'chip':7} {'clock':>9} {'Hz':>10} {'cents':>7} {'dB':>6}  used by")

    failures = 0
    seen = set()
    for sub_name, users in staging.emu_subprofiles().items():
        sub = staging.load(staging.hw_plugins / sub_name)
        for engine in sub.get("engines", []):
            for chip in engine.get("chips", []):
                name = chip.get("chip")
                if name not in FM_CHIPS:
                    continue
                key = (sub_name, engine.get("dll"), name, chip.get("clock"))
                if key in seen:
                    continue
                seen.add(key)
                if args.only and args.only not in name and args.only not in sub_name:
                    continue
                cc0, bank, kind = FM_CHIPS[name]
                tag = f"{Path(sub_name).stem.replace('.profile', '')}_{Path(engine['dll']).name}_{name}"
                prof = fm.write_single_chip_profile(staging, sub_name, engine, chip, work, tag)
                mid, wav = work / f"{tag}.mid", work / f"{tag}.wav"
                build_smf(mid, cc0, bank, kind, args.note)
                try:
                    fm.run_render(args.render, prof, mid, wav, rate=args.rate)
                    rate, x = fm.read_wav(wav)
                    seg = fm.segment(x, rate, NOTE_START + 0.3, NOTE_START + NOTE_LEN - 0.1)
                    f, _ = fm.peak_freq(seg, rate, expected / 1.5, expected * 1.5)
                    level = fm.rms_db(seg)
                    c = fm.cents(f, expected)
                    ok = abs(c) <= args.tolerance and level > -60
                    result = f"{f:10.3f} {c:+7.2f} {level:6.1f}"
                except Exception as e:   # 1件の失敗で残りを止めない
                    ok = False
                    result = "ERROR " + str(e).splitlines()[0]
                failures += 0 if ok else 1
                print(f"{sub_name:42} {Path(engine['dll']).name:16} {name:7} {chip.get('clock', 0):9} "
                      f"{result}  {','.join(users)}{'' if ok else '  <-- NG'}")
    print(f"NG: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
