"""Evaluation on REAL noise recordings (MS-SNSD / DEMAND, 16 kHz upsampled to 48 kHz)
plus full-band synthetic hiss/hum, mixed with EARS speech (48 kHz, several speakers).

Scores: DNSMOS P.835 (SIG = voice naturalness, BAK = how clean the background
sounds, OVRL), plus shadow-filtered metrics: speech level change, onset loss,
noise reduction in pauses, log-spectral distortion of the speech component
(colour/muffling), STOI.

usage: python3 realeval.py config_name [config_name ...]   (configs in CONFIGS)
"""
import sys, glob, os, json
import numpy as np, soundfile as sf
from scipy.signal import resample_poly
from multiprocessing import Pool
sys.path.insert(0, "/home/claude/dnsmos")
import dnsmos
from pystoi import stoi
import dsp
from dsp import FS, GainEstimator, compute_gains, minphase_fir, apply_tv_fir, apply_stft
import evaluate as ev

NOISES = ["AirConditioner_1", "CopyMachine_1", "VacuumCleaner_1", "tr_Car_1", "tr_Bus_1",
          "tr_Office_1", "tr_Hallway_1", "tr_Washing_1", "tr_Traffic_1", "tr_Kitchen_1",
          "tr_Cafe_1", "Typing_1", "hiss", "hum"]
SNRS = [5, 10]


def speech_clip(spk_i):
    files = sorted(glob.glob("audio/p00*/sentences_0*_regular.wav"))
    if not files:
        files = ["audio/sentences_24_regular.wav"]
    rng = np.random.default_rng(spk_i)
    pick = [files[j] for j in rng.choice(len(files), 2, replace=False)]
    z = lambda t: np.zeros(int(t * FS))
    parts = [z(3.0)]
    for f in pick:
        s, _ = sf.read(f)
        parts += [s[: 6 * FS], z(1.0)]
    c = np.concatenate(parts)
    return c / np.max(np.abs(c)) * 0.5


def noise_clip(name, n, seed):
    if name in ("hiss", "hum"):
        x = ev.make_noise(name, n, [])
    else:
        x, fs = sf.read(f"realnoise/{name}.wav")
        x = resample_poly(x, 3, 1)  # 16k -> 48k
        start = np.random.default_rng(seed).integers(0, max(1, len(x) - n))
        x = np.resize(x[start:], n)
    return x / (np.std(x) + 1e-12)


def lsd(clean, filt):
    """log-spectral distance (dB) of speech component on speech-active frames, 50 Hz-8 kHz"""
    n, h = 2048, 512
    w = np.hanning(n)
    fr = lambda x: np.abs(np.fft.rfft(np.lib.stride_tricks.sliding_window_view(x, n)[::h] * w, axis=1)) ** 2
    C, F = fr(clean), fr(filt)
    e = C.sum(1)
    act = e > e.max() * 1e-3
    k0, k1 = int(50 / FS * n), int(8000 / FS * n)
    d = 10 * np.log10((F[act, k0:k1] + 1e-10) / (C[act, k0:k1] + 1e-10))
    # weight by clean energy so we look at audible speech, not near-silent bins
    wgt = C[act, k0:k1]
    return float(np.sqrt((d ** 2 * wgt).sum() / wgt.sum()))


CONFIGS = {}


def register(name):
    def deco(f):
        CONFIGS[name] = f
        return f
    return deco


def run_est(est_kwargs, sigs, lookahead=0, L=512):
    clean, noise, mix = sigs
    est = GainEstimator(1024, 64, "hann", **est_kwargs)
    G = compute_gains(mix, est)
    H = np.stack([minphase_fir(g, L, 1024) for g in G])
    D = lookahead
    return [apply_tv_fir(np.concatenate([np.zeros(D), s]), H, 64)[D:] for s in sigs]


V1 = dict(amount=0.7, fast=256, fthr=4, rel_tau=0, psmooth=0.004)


@register("bypass")
def c_bypass(sigs):
    return list(sigs)


@register("stft_ref")
def c_ref(sigs):
    clean, noise, mix = sigs
    est = GainEstimator(1024, 256, "hann", amount=0.7)
    G = compute_gains(mix, est)
    return [apply_stft(s, G, 1024, 256)[0] for s in sigs]


@register("v1_zero")
def c_v1(sigs):
    return run_est(V1, sigs)


@register("v1_hq")
def c_v1hq(sigs):
    return run_est(V1, sigs, lookahead=512)


def extra_configs():
    # configs defined in other modules (experiments) register themselves on import
    if os.path.exists("experiments.py"):
        import experiments  # noqa: F401
        import realeval as _r
        CONFIGS.update(_r.CONFIGS)


def job(args):
    cfg, noise_name, snr, spk = args
    clean = speech_clip(spk)
    nz = noise_clip(noise_name, len(clean), spk)
    ps = np.mean(clean[np.abs(clean) > 0.01] ** 2)
    nz = nz * np.sqrt(ps / 10 ** (snr / 10))
    mix = clean + nz
    out_c, out_n, out_m = CONFIGS[cfg]((clean, nz, mix))
    m = ev.metrics(clean, nz, out_c, out_n, out_m)
    sk = 2 * FS
    m["lsd"] = lsd(clean[sk:], out_c[sk:])
    m.update(dnsmos.score(out_m[sk:], FS))
    del m["n_onsets"]
    if os.environ.get("SAVE"):
        os.makedirs("out_real", exist_ok=True)
        sf.write(f"out_real/{noise_name}_{snr}dB_{cfg}.wav", out_m, FS, subtype="PCM_24")
    return cfg, noise_name, snr, {k: float(v) for k, v in m.items()}


if __name__ == "__main__":
    extra_configs()
    cfgs = sys.argv[1:]
    noises = os.environ.get("NOISES", ",".join(NOISES)).split(",")
    snrs = [int(s) for s in os.environ.get("SNRS", ",".join(map(str, SNRS))).split(",")]
    jobs = [(c, nn, s, i) for c in cfgs for i, nn in enumerate(noises) for s in snrs]
    with Pool(4) as p:
        res = p.map(job, jobs)
    keys = ["SIG", "BAK", "OVRL", "speech_dB", "onset_dB", "pause_dB", "lsd", "stoi"]
    allres = {}
    for c, nn, s, m in res:
        allres.setdefault(c, []).append(m)
        if os.environ.get("VERBOSE"):
            print(f"{c:14s} {nn:18s} {s:2d}dB " + " ".join(f"{k}={m[k]:.2f}" for k in keys))
    print(f"{'config':16s} " + " ".join(f"{k:>9s}" for k in keys))
    for c in cfgs:
        mean = {k: np.mean([m[k] for m in allres[c]]) for k in keys}
        print(f"{c:16s} " + " ".join(f"{mean[k]:9.2f}" for k in keys), flush=True)
    os.makedirs("results_real", exist_ok=True)
    with open(f"results_real/{'_'.join(cfgs)[:80]}.json", "w") as f:
        json.dump(res, f)
