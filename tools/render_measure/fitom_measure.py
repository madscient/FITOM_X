"""fitom_render で鳴らした音を測るための部品。

SMFを組み立て、fitom_render を呼び、出てきたWAVを numpy で読んで測る。
ステージングリポジトリのプロファイル・HWプラグインを使うが、パスは呼び出し側が渡す。
"""

import json
import math
import struct
import subprocess
from pathlib import Path

import numpy as np


# ─── SMF ─────────────────────────────────────────────────────────────────────

PPQN = 480
TEMPO_US = 500000   # 120BPM。1tick = 500000/480 us


def seconds_to_ticks(sec):
    return int(round(sec * 1e6 / TEMPO_US * PPQN))


def _vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def _track(events):
    """events: [(秒, bytes)]。SysEx は F0 から F7 までを1つの bytes で渡す。"""
    body = bytearray()
    last = 0
    for sec, msg in sorted(events, key=lambda e: e[0]):
        tick = seconds_to_ticks(sec)
        body += _vlq(tick - last)
        last = tick
        if msg[0] == 0xF0:
            body += b"\xF0" + _vlq(len(msg) - 1) + bytes(msg[1:])
        else:
            body += bytes(msg)
    body += b"\x00\xFF\x2F\x00"
    return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def write_smf(path, tracks):
    """tracks: [[(秒, bytes)]]。フォーマット1、テンポ指定なし(120BPM)。"""
    data = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), PPQN)
    data += b"".join(_track(t) for t in tracks)
    Path(path).write_bytes(data)


def cc(ch, num, val):
    return bytes([0xB0 | ch, num, val])


def program(ch, prog):
    return bytes([0xC0 | ch, prog])


def note_on(ch, note, vel=100):
    return bytes([0x90 | ch, note, vel])


def note_off(ch, note):
    return bytes([0x80 | ch, note, 0])


def pitch_bend(ch, value):
    """value: -8192〜+8191。"""
    v = max(0, min(16383, value + 8192))
    return bytes([0xE0 | ch, v & 0x7F, v >> 7])


def rpn(ch, msb, lsb, data_msb, data_lsb=0):
    """RPN を設定して値を書き、RPN NULL に戻す列。"""
    return [cc(ch, 101, msb), cc(ch, 100, lsb), cc(ch, 6, data_msb), cc(ch, 38, data_lsb),
            cc(ch, 101, 127), cc(ch, 100, 127)]


def fine_tune(ch, cents):
    """チャンネルファインチューニング(RPN#1)。-100〜+100セント。"""
    v = max(0, min(16383, 8192 + round(cents * 8192 / 100)))
    return rpn(ch, 0, 1, v >> 7, v & 0x7F)


def select_device_patch(ch, device_cc0, bank=0, prog=0):
    """直接デバイス選択モードで、チップ種別 device_cc0 の bank/prog を選ぶ列。"""
    return [cc(ch, 0, device_cc0), cc(ch, 32, bank), program(ch, prog)]


def hwpatch_override(ch, obj, layer=0):
    """チャンネルの HwPatch を一時的に上書きするプライベートSysEx。"""
    js = json.dumps(obj, separators=(",", ":")).encode("ascii")
    return bytes([0xF0, 0x00, 0x48, 0x01, 0x01, 0x00, ch, layer]) + js + b"\xF7"


def swpatch_override(ch, obj, layer=0):
    js = json.dumps(obj, separators=(",", ":")).encode("ascii")
    return bytes([0xF0, 0x00, 0x48, 0x01, 0x02, 0x00, ch, layer]) + js + b"\xF7"


# ─── 変換 ────────────────────────────────────────────────────────────────────

def run_render(render_exe, profile, mid, wav, rate=48000, fmt="f32", tail=0.5):
    """fitom_render のログ(fitom_render.log)は、出力の WAV と同じディレクトリに出す。"""
    # Windows の CreateProcess は、区切りが / の相対パスを実行ファイルとして見つけられない。
    r = subprocess.run([str(Path(render_exe).resolve()), str(profile), str(mid), str(wav),
                        "--rate", str(rate), "--format", fmt, "--tail", str(tail)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       cwd=Path(wav).resolve().parent)
    if r.returncode != 0:
        raise RuntimeError(f"fitom_render failed ({r.returncode}): {r.stderr.strip()}")
    return r.stderr


def read_wav(path):
    """(サンプルレート, float64 の [frames, channels]) を返す。PCM 16/24bit と float32 に対応。"""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"not a WAV file: {path}")
    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, chans, rate, _, _, bits = struct.unpack_from("<HHIIHH", body)
            fmt = (tag, chans, rate, bits)
        elif cid == b"data":
            tag, chans, rate, bits = fmt
            if tag == 3 and bits == 32:
                x = np.frombuffer(body, dtype="<f4").astype(np.float64)
            elif tag == 1 and bits == 16:
                x = np.frombuffer(body, dtype="<i2") / 32768.0
            elif tag == 1 and bits == 24:
                b = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
                v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
                v = np.where(v & 0x800000, v - 0x1000000, v)
                x = v / 8388608.0
            else:
                raise ValueError(f"unsupported WAV format tag={tag} bits={bits}")
            return rate, x.reshape(-1, chans)
        pos += 8 + size + (size & 1)
    raise ValueError(f"no data chunk: {path}")


# ─── 測定 ────────────────────────────────────────────────────────────────────

def segment(x, rate, start_sec, end_sec):
    return x[int(start_sec * rate):int(end_sec * rate)]


def rms_db(x):
    """直流分を除いた RMS(dB)。ymfm の YM2612 のように、無音でも直流を出すエンジンがあるため。"""
    if len(x):
        x = x - x.mean(axis=0)
    v = float(np.sqrt(np.mean(np.square(x)))) if len(x) else 0.0
    return 20 * math.log10(v) if v > 0 else -math.inf


def peak_freq(x, rate, fmin, fmax, pad=8):
    """fmin〜fmax で最も強いスペクトルのピークの周波数(Hz)と、その強さ(dB)。

    Hann窓・ゼロ詰め・対数振幅の放物線補間。純音なら1セントより十分細かく求まる。
    """
    if x.ndim == 2:
        x = x.mean(axis=1)
    x = x - x.mean()
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n), n * pad))
    freqs = np.fft.rfftfreq(n * pad, 1.0 / rate)
    lo, hi = np.searchsorted(freqs, [fmin, fmax])
    k = lo + int(np.argmax(spec[lo:hi]))
    a, b, c = (np.log(max(spec[i], 1e-300)) for i in (k - 1, k, k + 1))
    d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    f = (k + d) * rate / (n * pad)
    return f, 20 * math.log10(spec[k] / (n / 4) + 1e-300)


def pitch_track(x, rate, fmin, fmax, frame_sec=0.05, hop_sec=0.01):
    """短い区間ごとの基音(Hz)の列。(時刻[秒], 周波数[Hz]) の配列の組を返す。"""
    if x.ndim == 2:
        x = x.mean(axis=1)
    frame, hop = int(frame_sec * rate), int(hop_sec * rate)
    times, freqs = [], []
    for start in range(0, len(x) - frame, hop):
        f, _ = peak_freq(x[start:start + frame], rate, fmin, fmax, pad=16)
        times.append((start + frame / 2) / rate)
        freqs.append(f)
    return np.array(times), np.array(freqs)


def cents(f, ref):
    return 1200 * math.log2(f / ref)


def note_freq(note, a4=440.0):
    return a4 * 2 ** ((note - 69) / 12)


# ─── ステージングのプロファイルから測定用のプロファイルを作る ─────────────────

class Staging:
    """ステージングリポジトリの配置。bin/ に FitomEmuIF.dll と engines/ がある前提。"""

    def __init__(self, root, emuif_dll=None):
        self.root = Path(root).resolve()
        self.bin = self.root / "bin"
        # bin/ の FitomEmuIF.dll はマシンごとに版が違い、オフライン描画に対応していない
        # ことがあるので、別のビルドを指定できるようにする。
        self.emuif_dll = Path(emuif_dll).resolve() if emuif_dll else self.bin / "FitomEmuIF.dll"
        self.profiles = self.root / "config" / "profiles"
        self.hw_plugins = self.profiles / "hw_plugins"
        self.bankset = self.profiles / "unified.bankset.json"

    def load(self, path):
        return json.loads(Path(path).read_text(encoding="utf-8"))

    def find_chip(self, subprofile_name, chip_name):
        """サブプロファイルから chip_name の最初のエントリを (エンジン, チップ) で返す。"""
        for engine in self.load(self.hw_plugins / subprofile_name).get("engines", []):
            for chip in engine.get("chips", []):
                if chip.get("chip") == chip_name:
                    return engine, chip
        raise KeyError(f"{chip_name} not in {subprofile_name}")

    def emu_subprofiles(self):
        """FitomEmuIF を使うプロファイルが参照するサブプロファイルの {ファイル名: [プロファイル名]}。"""
        out = {}
        for p in sorted(self.profiles.glob("*.profile.json")):
            for hp in self.load(p).get("hw_plugins", []):
                if hp.get("name") == "FitomEmuIF" and hp.get("profile"):
                    out.setdefault(Path(hp["profile"]).name, []).append(p.name.replace(".profile.json", ""))
        return out


def write_bankset(staging, workdir):
    """ステージングのバンクセットを、ファイルのパスを絶対パスにして workdir に写す。

    バンクセットの中の相対パスはプロファイルの位置から解決されるため、別の場所に置いた
    プロファイルからはそのままでは読めない。SF2 のバンクは、SF2 のデバイスを載せない
    測定用のプロファイルでは設定エラーになるので外す。
    """
    path = Path(workdir) / "measure.bankset.json"
    if path.exists():
        return path
    bs = staging.load(staging.bankset)
    bs.pop("sf2_banks", None)

    def absolutize(v):
        if isinstance(v, dict):
            return {k: (str((staging.profiles / x).resolve()) if k == "file" and isinstance(x, str)
                        else absolutize(x)) for k, x in v.items()}
        if isinstance(v, list):
            return [absolutize(x) for x in v]
        return v

    path.write_text(json.dumps(absolutize(bs), ensure_ascii=False, indent=2), encoding="utf-8")
    return path


def write_single_chip_profile(staging, subprofile_name, engine, chip, workdir, tag, master_pitch=440.0,
                              rhythm=False):
    """サブプロファイルの1チップだけを載せたプロファイルを workdir に書き、そのパスを返す。

    rhythm=True なら、そのチップの内蔵リズムを使う(ch6-8 を明け渡す)。

    エンジン・クロックなどチップの設定はサブプロファイルのものをそのまま使う。
    束ね・ステレオペアを外すため、チップは1個だけにする。
    """
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    sub = staging.load(staging.hw_plugins / subprofile_name)
    # エンジンの相対パスは FitomEmuIF.dll の位置から解決されるため、ステージングの bin/ を
    # 起点に絶対パスにしておく(別の場所の FitomEmuIF.dll を使っても同じエンジンを読む)。
    sub["engines"] = [dict(engine, dll=str(staging.bin / engine["dll"]), chips=[chip])]
    if "pcm_catalog" in sub:
        sub["pcm_catalog"] = str((staging.hw_plugins / sub["pcm_catalog"]).resolve())
    sub_path = workdir / f"{tag}.sub.json"
    sub_path.write_text(json.dumps(sub, ensure_ascii=False, indent=2), encoding="utf-8")

    prof = {
        "profile_name": f"measure {tag}",
        "banks": str(write_bankset(staging, workdir)),
        "master_pitch": master_pitch,
        "master_volume": 127,
        "hw_plugins": [{
            "name": "FitomEmuIF",
            "dll": str(staging.emuif_dll),
            "profile": str(sub_path),
            "auto_devices": True,
        }],
    }
    if rhythm:
        prof["hw_plugins"][0]["auto_devices_rhythm_mode"] = [chip["chip"]]
    prof_path = workdir / f"{tag}.profile.json"
    prof_path.write_text(json.dumps(prof, ensure_ascii=False, indent=2), encoding="utf-8")
    return prof_path
