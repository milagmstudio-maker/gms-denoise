import sys, glob, time
import numpy as np
import soundfile as sf
from scipy.signal import lfilter, butter
from pystoi import stoi
from dsp import (FS, GainEstimator, compute_gains, apply_stft, filters_from_gains,
                 apply_tv_fir)

import os
rng = np.random.default_rng(1)
KINDS = os.environ.get("KINDS", "hiss,hum,hvac,babble").split(",")
SNRS = [int(v) for v in os.environ.get("SNRS", "5,15").split(",")]


def pink(n):
    w = rng.standard_normal(n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1, -2.494956002, 2.017265875, -0.522189400]
    return lfilter(b, a, w)


def make_noise(kind, n, speech_files):
    t = np.arange(n) / FS
    if kind == "hiss":
        x = pink(n) + 0.3 * rng.standard_normal(n)
    elif kind == "hum":
        x = sum((0.5 ** k) * np.sin(2 * np.pi * 60 * (k + 1) * t + k) for k in range(8))
        x += 0.3 * lfilter([1], [1, -0.995], rng.standard_normal(n)) * 0.05
    elif kind == "hvac":
        b, a = butter(2, [150 / (FS / 2), 1500 / (FS / 2)], "band")
        x = lfilter(b, a, rng.standard_normal(n)) * (1 + 0.2 * np.sin(2 * np.pi * 0.3 * t))
        x += 0.5 * lfilter([1], [1, -0.99], rng.standard_normal(n)) * 0.1
    elif kind == "babble":
        x = np.zeros(n)
        for f in speech_files:
            s, _ = sf.read(f)
            s = s[::-1]
            s = np.resize(s, n)
            x += np.roll(s, rng.integers(n))
        x += 0.1 * pink(n) * np.std(x) / np.std(pink(n))
    return x / np.std(x)


def load_clean():
    a, _ = sf.read("audio/sentences_24_regular.wav")
    b, _ = sf.read("audio/rainbow_03_regular.wav")
    z = lambda t: np.zeros(int(t * FS))
    c = np.concatenate([z(3.0), a[:8 * FS], z(1.2), b[:7 * FS], z(0.8)])
    return c / np.max(np.abs(c)) * 0.5


def env_db(x, fl=240):
    m = len(x) // fl
    e = (x[:m * fl].reshape(m, fl) ** 2).mean(1)
    return 10 * np.log10(e + 1e-12)


def metrics(clean, noise, out_clean, out_noise, out_mix):
    sk = 2 * FS  # skip warm-up
    clean, noise, out_clean, out_noise, out_mix = (v[sk:] for v in (clean, noise, out_clean, out_noise, out_mix))
    # speech-active frames
    ec = env_db(clean)
    act = ec > ec.max() - 40
    eo = env_db(out_clean)
    sp_att = 10*np.log10((10**(eo[act]/10)).sum()/(10**(ec[act]/10)).sum())  # energy-weighted
    # onsets: 5ms frames where clean jumps 15 dB above previous 50ms max
    on = []
    for i in range(10, len(ec) - 4):
        if ec[i] > ec.max() - 30 and ec[i] > ec[i - 10:i].max() + 10:
            if not on or i - on[-1] > 20:
                on.append(i)
    onset_att = np.mean([10 * np.log10((10 ** (eo[i:i + 4] / 10)).sum() /
                                       (10 ** (ec[i:i + 4] / 10)).sum()) for i in on]) if on else np.nan
    noise_att = 10 * np.log10(np.mean(out_noise ** 2) / np.mean(noise ** 2))
    pause = ec < ec.max() - 80
    en, eon = env_db(noise), env_db(out_noise)
    nn = min(len(pause), len(en))
    pz = pause[:nn]
    pause_att = 10 * np.log10((10 ** (eon[:nn][pz] / 10)).sum() / (10 ** (en[:nn][pz] / 10)).sum())
    st = stoi(clean, out_mix, FS, extended=False)
    return dict(speech_dB=sp_att, onset_dB=onset_att, noise_dB=noise_att, pause_dB=pause_att, stoi=st, n_onsets=len(on))


def run(method, clean, noise, amount):
    mix = clean + noise
    t0 = time.time()
    if method == "stft_ref":
        est = GainEstimator(1024, 256, "hann", amount=amount)
        G = compute_gains(mix, est)
        outs = [apply_stft(s, G, 1024, 256)[0] for s in (clean, noise, mix)]
        lat = 1024
    elif method.startswith("zl"):
        # zl_<window>_<L>_<hop>
        parts = method.split("_")
        win, L, hop = parts[1], int(parts[2]), int(parts[3])
        fast = int(parts[4]) if len(parts) > 4 else 0
        est = GainEstimator(1024, hop, win, tail=128, amount=amount, fast=fast)
        G = compute_gains(mix, est)
        H = filters_from_gains(G, L, "min")
        outs = [apply_tv_fir(s, H, hop) for s in (clean, noise, mix)]
        lat = 0
    elif method == "final":
        est = GainEstimator(1024, 64, "hann", amount=amount, fast=256, fthr=4, rel_tau=0, psmooth=0.004)
        G = compute_gains(mix, est)
        H = filters_from_gains(G, 512, "min")
        outs = [apply_tv_fir(s, H, 64) for s in (clean, noise, mix)]
        lat = 0
    elif method == "bypass":
        outs = [clean, noise, mix]
        lat = 0
    m = metrics(clean, noise, *outs)
    m["latency_ms"] = lat / FS * 1000
    m["sec"] = time.time() - t0
    return m, outs[2]


if __name__ == "__main__":
    files = sorted(glob.glob("audio/*.wav"))
    clean_file = "audio/sentences_24_regular.wav"
    babble_src = [f for f in files if 'regular' not in f]
    clean = load_clean()
    methods = sys.argv[1].split(",") if len(sys.argv) > 1 else [
        "bypass", "stft_ref", "zl_hann_512_256", "zl_asym_512_256", "zl_asym_512_64"]
    amount = float(sys.argv[2]) if len(sys.argv) > 2 else 0.7
    for kind in KINDS:
        for snr in SNRS:
            nz = make_noise(kind, len(clean), babble_src)
            ps = np.mean(clean[np.abs(clean) > 0.01] ** 2)
            nz = nz * np.sqrt(ps / 10 ** (snr / 10))
            for mth in methods:
                m, out = run(mth, clean, nz, amount)
                print(f"{kind:6s} {snr:2d}dB {mth:18s} speech {m['speech_dB']:6.2f}  onset {m['onset_dB']:6.2f}  "
                      f"noise {m['noise_dB']:6.1f} pause {m['pause_dB']:6.1f} n_on {m['n_onsets']}  STOI {m['stoi']:.3f}  lat {m['latency_ms']:4.1f}ms  ({m['sec']:.1f}s)", flush=True)
                if snr == 5:
                    sf.write(f"out/{kind}_{mth}.wav", out, FS)
