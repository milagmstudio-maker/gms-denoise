"""Gate evaluation: how much speech a gate cuts when its threshold is set the way a user
would (just above the level shown during a noise-only pause).
The gate is a scalar gain, so g(t) = |y_gate| / |y_nogate| (1 ms smoothing) is applied to
the clean speech to measure speech loss. run from the eval data dir."""
import sys, os, subprocess, tempfile, json
import numpy as np, soundfile as sf
from multiprocessing import Pool
sys.path.insert(0, "/home/claude/zerodenoise/prototype")
import realeval as re_
FS = 48000
CLI = "/home/claude/zerodenoise/build/zd_cli"
NOISES = os.environ.get("NOISES", "AirConditioner_1,tr_Office_1,tr_Cafe_1,tr_Traffic_1,hiss,Typing_1").split(",")
SNRS = [int(s) for s in os.environ.get("SNRS", "10,20").split(",")]
MARGIN = float(os.environ.get("MARGIN", "2"))


def cli(x, env, td, tag, dump=None):
    i, o = f"{td}/{tag}_in.wav", f"{td}/{tag}_out.wav"
    sf.write(i, x.astype(np.float32), FS, subtype="FLOAT")
    e = dict(os.environ, ZD_AI="1", ZD_LOOKAHEAD="480", ZD_RED="40", **env)
    if dump: e.update(ZD_DUMP=dump, ZD_GDUMP="1")
    subprocess.run([CLI, i, o, "0.7"], env=e, check=True, capture_output=True)
    y, _ = sf.read(o)
    return y[480:]


def smooth(p, n=48):
    return np.convolve(p, np.ones(n) / n, mode="same")


def job(a):
    det, name, snr, spk = a
    clean = re_.speech_clip(spk)
    nz = re_.noise_clip(name, len(clean), spk)
    ps = np.mean(clean[np.abs(clean) > 0.01] ** 2)
    mix = clean + nz * np.sqrt(ps / 10 ** (snr / 10))
    env = {"ZD_GATEOUT": "1"} if det == "out" else {}
    with tempfile.TemporaryDirectory() as td:
        y0 = cli(mix, env, td, "a", dump=f"{td}/lvl.bin")
        lvl = np.fromfile(f"{td}/lvl.bin", dtype=np.float32)
        # user: watch the meter during the noise-only lead-in (1.5..3 s), set just above its peak
        h0, h1 = int(1.5 * FS / 64), int(2.9 * FS / 64)
        thr = float(np.max(lvl[h0:h1])) + MARGIN
        y1 = cli(mix, dict(env, ZD_GATE=str(thr)), td, "b")
    n = min(len(y0), len(y1), len(clean))
    g = np.sqrt(smooth(y1[:n] ** 2) / (smooth(y0[:n] ** 2) + 1e-20))
    g = np.clip(g, 0, 1)
    c = clean[:n]
    fl = 480  # 10 ms frames
    nf = n // fl
    ce = np.array([np.sum(c[k * fl:(k + 1) * fl] ** 2) for k in range(nf)])
    ge = np.array([np.sum((c[k * fl:(k + 1) * fl] * g[k * fl:(k + 1) * fl]) ** 2) for k in range(nf)])
    act = ce > np.max(ce) * 10 ** (-40 / 10)  # speech frames within 40 dB of the peak
    quiet = act & (ce < np.max(ce) * 10 ** (-20 / 10))  # quiet speech (word ends, soft consonants)
    cut = act & (ge < ce * 10 ** (-6 / 10))
    loss = 10 * np.log10(np.sum(ge[act]) / np.sum(ce[act]))
    pause = ~act
    pause[: int(3.0 * FS / fl)] = False
    muted = np.mean(g[: nf * fl].reshape(nf, fl).mean(1)[pause] < 1e-3) if pause.any() else np.nan
    return dict(det=det, noise=name, snr=snr, spk=spk, thr=thr, loss=loss,
                cut=float(np.mean(cut[act])), cutq=float(np.mean(cut[quiet])) if quiet.any() else 0.0,
                muted=float(muted))


if __name__ == "__main__":
    os.chdir("/home/claude/evaldata")
    jobs = [(d, nm, s, k) for d in ("in", "out") for nm in NOISES for s in SNRS for k in range(4)]
    with Pool(4) as p:
        res = p.map(job, jobs)
    json.dump(res, open(f"/home/claude/evaldata/gate_results_m{MARGIN:g}.json", "w"), indent=1)
    for d in ("in", "out"):
        for s in SNRS:
            r = [x for x in res if x["det"] == d and x["snr"] == s]
            print(f"det={d:3s} snr={s:2d}: speech loss {np.mean([x['loss'] for x in r]):6.2f} dB, "
                  f"speech frames cut>6dB {100*np.mean([x['cut'] for x in r]):5.1f}%, quiet speech cut {100*np.mean([x['cutq'] for x in r]):5.1f}%, "
                  f"pauses fully muted {100*np.nanmean([x['muted'] for x in r]):5.1f}%")
        for nm in NOISES:
            r = [x for x in res if x["det"] == d and x["noise"] == nm]
            print(f"   {nm:18s} cut {100*np.mean([x['cut'] for x in r]):5.1f}%  quiet {100*np.mean([x['cutq'] for x in r]):5.1f}%  muted {100*np.nanmean([x['muted'] for x in r]):5.1f}%")
