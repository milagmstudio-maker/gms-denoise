"""Minimal DNSMOS P.835 scorer (SIG = speech quality, BAK = background noise, OVRL).
Logic follows microsoft/DNS-Challenge DNSMOS/dnsmos_local.py (non-personalized)."""
import numpy as np, onnxruntime as ort
from scipy.signal import resample_poly
import os
_sess = ort.InferenceSession(os.path.join(os.path.dirname(__file__), "sig_bak_ovr.onnx"))
_p_ovr = np.poly1d([-0.06766283, 1.11546468, 0.04602535])
_p_sig = np.poly1d([-0.08397278, 1.22083953, 0.0052439])
_p_bak = np.poly1d([-0.13166888, 1.60915514, -0.39604546])

def score(x, fs):
    if fs != 16000:
        from math import gcd
        g = gcd(int(fs), 16000)
        x = resample_poly(x, 16000 // g, int(fs) // g)
    fs = 16000
    L = int(9.01 * fs)
    while len(x) < L:
        x = np.append(x, x)
    hops = int(np.floor(len(x) / fs) - 9.01) + 1
    s = []
    for i in range(hops):
        seg = x[i * fs: i * fs + L]
        if len(seg) < L:
            continue
        raw = _sess.run(None, {"input_1": seg.astype("float32")[None, :]})[0][0]
        s.append((_p_sig(raw[0]), _p_bak(raw[1]), _p_ovr(raw[2])))
    s = np.array(s).mean(0)
    return dict(SIG=float(s[0]), BAK=float(s[1]), OVRL=float(s[2]))
