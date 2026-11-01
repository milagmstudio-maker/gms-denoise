"""Expressive-voice eval: does the denoiser eat shouting, screaming, high-pitched
voice, singing? EARS styles (p001-p004) mixed with real noise, run through the
final C++ path (zd_cli, shadow filtering like cppeval.py).
Levels: per speaker one gain from the regular sentence (peak 0.3), applied to all
styles, so loud/screaming stay louder as recorded (clipped to +-0.99).
Metrics on the speech component: speech_dB (energy weighted), drop% (share of
active 20 ms frames attenuated > 6 dB), onset_dB, hf_dB (speech > 2 kHz)."""
import sys, os, glob, subprocess, tempfile, json
import numpy as np, soundfile as sf
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cppeval as ce
import realeval as re_
import evaluate as ev
from scipy.signal import butter, sosfilt
FS = 48000
CATS = {
    "regular": ["sentences_01_regular"], "loud": ["sentences_01_loud", "sentences_02_loud"],
    "highpitch": ["sentences_01_highpitch", "sentences_02_highpitch"],
    "scream": ["nonverbal_screaming", "nonverbal_yelling"], "anger": ["emo_anger_sentences"],
    "singing": ["melodic_happy_birthday"], "laugh": ["nonverbal_laughter_open"],
}
NOISES = os.environ.get("NOISES", "AirConditioner_1,tr_Cafe_1,hiss,tr_Office_1").split(",")
SNR = float(os.environ.get("SNR", "10"))
SPK = ["p001", "p002", "p003", "p004"]


def path(spk, name):
    for d in ("expr", "audio"):
        f = f"{d}/{spk}/{name}.wav"
        if os.path.exists(f):
            return f
    return None


def clip(spk, cat):
    ref, _ = sf.read(path(spk, "sentences_01_regular"))
    g = 0.3 / np.max(np.abs(ref))
    parts = [np.zeros(2 * FS)]
    for n in CATS[cat]:
        x, _ = sf.read(path(spk, n))
        parts += [x[: 10 * FS] * g, np.zeros(FS)]
    return np.clip(np.concatenate(parts), -0.99, 0.99)


def frames_db(x, n=960):
    m = len(x) // n
    return 10 * np.log10((x[: m * n].reshape(m, n) ** 2).mean(1) + 1e-12)


def job(a):
    mode, cat, nn, spk = a
    clean = clip(spk, cat)
    nz = re_.noise_clip(nn, len(clean), int(spk[1:]))
    # noise level from the *regular* speech level of this speaker (fixed room noise)
    reg = clip(spk, "regular")
    ps = np.mean(reg[np.abs(reg) > 0.01] ** 2)
    nz = nz * np.sqrt(ps / 10 ** (SNR / 10))
    L = float(os.environ.get("LEVEL", "1"))
    clean, nz = clean * L, nz * L
    mix = clean + nz
    oc, on, om = ce.run_mode(mode, clean, nz, mix)
    sk = 2 * FS
    c, o = clean[sk:], oc[sk:]
    ec, eo = frames_db(c), frames_db(o)
    act = ec > ec.max() - 35
    loud = ec > ec.max() - 20
    d = eo[act] - ec[act]
    sos = butter(4, 2000, "high", fs=FS, output="sos")
    hc, ho = sosfilt(sos, c), sosfilt(sos, o)
    m = ev.metrics(clean, nz, oc, on, om)
    return mode, cat, nn, spk, dict(
        speech_dB=float(10 * np.log10((10 ** (eo[act] / 10)).sum() / (10 ** (ec[act] / 10)).sum())),
        drop=float(100 * np.mean((eo - ec)[loud] < -6)),
        hf_dB=float(10 * np.log10(np.sum(ho ** 2) / np.sum(hc ** 2))),
        onset_dB=float(m["onset_dB"]), pause_dB=float(m["pause_dB"]))


if __name__ == "__main__":
    modes = sys.argv[1:] or ["zero", "ai"]
    cats = os.environ.get("CATS", ",".join(CATS)).split(",")
    jobs = [(mo, c, nn, s) for mo in modes for c in cats for nn in NOISES for s in SPK]
    with Pool(4) as p:
        res = p.map(job, jobs)
    keys = ["speech_dB", "drop", "hf_dB", "onset_dB", "pause_dB"]
    print(f"SNR {SNR} dB (vs regular speech), noises {NOISES}")
    print(f"{'cat':10s} {'mode':10s} " + " ".join(f"{k:>9s}" for k in keys))
    for c in cats:
        for mo in modes:
            r = [m for (a, b, _, _, m) in res if a == mo and b == c]
            print(f"{c:10s} {mo:10s} " + " ".join(f"{np.nanmean([x[k] for x in r]):9.2f}" for k in keys))
    print("ALL")
    for mo in modes:
        r = [m for (a, _, _, _, m) in res if a == mo]
        print(f"{'all':10s} {mo:10s} " + " ".join(f"{np.nanmean([x[k] for x in r]):9.2f}" for k in keys))
    json.dump(res, open(f"expr_{'_'.join(modes)}_{int(SNR)}.json", "w"))
