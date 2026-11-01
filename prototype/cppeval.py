"""Real-noise eval through the FINAL C++ path (zd_cli), all four plugin modes.
Shadow filtering without touching the DSP: the analysis uses the channel mean, so
feeding stereo [mix+clean, mix-clean] gives H(mix)=(y0+y1)/2, H(clean)=(y0-y1)/2 exactly.
usage: run from the eval data dir: python3 cppeval.py"""
import sys, os, subprocess, tempfile, json
import numpy as np, soundfile as sf
from multiprocessing import Pool
sys.path.insert(0, "/home/claude/zerodenoise/prototype")
import realeval as re_
import evaluate as ev
import dnsmos
FS = 48000
CLI = "/home/claude/zerodenoise/build/zd_cli"
MODES = {  # name: (env, lookahead)
    "zero": ({}, 0), "low": ({}, 128), "hq": ({}, 512), "ai": ({"ZD_AI": "1"}, 480)}
AMOUNT = os.environ.get("AMOUNT", "0.7")


def job(args):
    mode, name, snr, spk = args
    clean = re_.speech_clip(spk)
    nz = re_.noise_clip(name, len(clean), spk)
    ps = np.mean(clean[np.abs(clean) > 0.01] ** 2)
    nz = nz * np.sqrt(ps / 10 ** (snr / 10))
    mix = clean + nz
    if mode == "bypass":
        oc, on, om = clean, nz, mix
    else:
        env, la = MODES[mode]
        with tempfile.TemporaryDirectory() as td:
            i, o = f"{td}/in.wav", f"{td}/out.wav"
            sf.write(i, np.stack([mix + clean, mix - clean], 1).astype(np.float32), FS, subtype="FLOAT")
            e = dict(os.environ, ZD_LOOKAHEAD=str(la), **env)
            subprocess.run([CLI, i, o, AMOUNT], env=e, check=True, capture_output=True)
            y, _ = sf.read(o)
        y = y[la:]
        y = np.concatenate([y, np.zeros((la, 2))])  # undo reported latency (host PDC)
        om, oc = (y[:, 0] + y[:, 1]) / 2, (y[:, 0] - y[:, 1]) / 2
        on = om - oc
    m = ev.metrics(clean, nz, oc, on, om)
    sk = 2 * FS
    m["lsd"] = re_.lsd(clean[sk:], oc[sk:])
    m.update(dnsmos.score(om[sk:], FS))
    m.pop("n_onsets", None)
    if os.environ.get("SAVE") and snr == 10:
        os.makedirs("out_cpp", exist_ok=True)
        sf.write(f"out_cpp/{name}_{snr}dB_{mode}.wav", om, FS, subtype="PCM_24")
    return mode, name, snr, {k: float(v) for k, v in m.items()}


if __name__ == "__main__":
    modes = sys.argv[1:] or ["bypass", "zero", "low", "hq", "ai"]
    jobs = [(c, nn, s, i) for c in modes for i, nn in enumerate(re_.NOISES) for s in re_.SNRS]
    with Pool(4) as p:
        res = p.map(job, jobs)
    keys = ["SIG", "BAK", "OVRL", "speech_dB", "onset_dB", "pause_dB", "lsd", "stoi"]
    by = {}
    for c, nn, s, m in res:
        by.setdefault(c, []).append(m)
    print(f"amount={AMOUNT}, {len(re_.NOISES)} noises x {re_.SNRS} dB")
    print(f"{'mode':8s} " + " ".join(f"{k:>9s}" for k in keys))
    for c in modes:
        print(f"{c:8s} " + " ".join(f"{np.mean([m[k] for m in by[c]]):9.2f}" for k in keys), flush=True)
    if os.environ.get("VERBOSE"):
        for c, nn, s, m in sorted(res, key=lambda r: (r[1], r[2], r[0])):
            print(f"{nn:18s} {s:2d}dB {c:7s} " + " ".join(f"{k}={m[k]:.2f}" for k in keys))
    json.dump(res, open(f"cpp_results_{AMOUNT}.json", "w"))
