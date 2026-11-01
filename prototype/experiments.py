from realeval import register, run_est, V1


def cfg(name, la=0, **kw):
    p = dict(V1); p.update(kw)
    register(name)(lambda sigs, p=p, la=la: run_est(p, sigs, lookahead=la))


cfg("over07", over_k=0.7)
cfg("sfloor12", speech_floor_db=12)
cfg("sfloor12_over07", speech_floor_db=12, over_k=0.7)
cfg("cep", cep=True)
cfg("rel20", rel_tau=0.02)
cfg("ps10", psmooth=0.010)
cfg("rel40", rel_tau=0.04)
cfg("r20_sf12_o07", rel_tau=0.02, speech_floor_db=12, over_k=0.7)
cfg("r40_sf12_o07", rel_tau=0.04, speech_floor_db=12, over_k=0.7)
cfg("r20_sf12_o07_f55", rel_tau=0.02, speech_floor_db=12, over_k=0.7, floor_db=55)
cfg("r20_sf9_o1_f55", rel_tau=0.02, speech_floor_db=9, over_k=1.0, floor_db=55)
cfg("r20_g05", rel_tau=0.02, gexp=0.5)
cfg("r20_gsp05", rel_tau=0.02, gexp_sp=0.5)
cfg("r20_gsp05_f55", rel_tau=0.02, gexp_sp=0.5, floor_db=55)
cfg("r20_gsp05_sf12_f55", rel_tau=0.02, gexp_sp=0.5, speech_floor_db=12, floor_db=55)
cfg("r20_gsp06_o1_f55", rel_tau=0.02, gexp_sp=0.6, over_k=1.0, floor_db=55)
cfg("r20_g05_f60", rel_tau=0.02, gexp=0.5, floor_db=60)
B = dict(rel_tau=0.02, gexp_sp=0.5, floor_db=55)
cfg("B_gsp04", **dict(B, gexp_sp=0.4))
cfg("B_g12", **dict(B, gexp=1.2))
cfg("B_vad30", **dict(B, vad_rel=0.3))
cfg("B_la128", la=128, **B)
cfg("B_la512", la=512, **B)
cfg("v1_la512", la=512)
cfg("v2_zero", **B)
cfg("v2_hq", la=512, **B)
cfg("B_ft3", **dict(B, fthr=3))
cfg("B_ft2", **dict(B, fthr=2))
cfg("B_fast128", **dict(B, fast=128))
cfg("B_vf", **dict(B, vad_fast=True))
cfg("B_vf_ft3", **dict(B, vad_fast=True, fthr=3))
cfg("B_frel60", **dict(B, fast_rel=0.06))
BV = dict(B, vad_fast=True)
cfg("v3_zero", **BV)
cfg("cq06", **dict(BV, cq=0.06))
cfg("cq12", **dict(BV, cq=0.12))
cfg("cq25", **dict(BV, cq=0.25))
cfg("cq12_4k", **dict(BV, cq=0.12, cq_start=4000))
cfg("hf12", **dict(BV, hf_floor_db=12))
cfg("hf18", **dict(BV, hf_floor_db=18))

cfg("v3_final", **dict(BV, hf_floor_db=12))
cfg("v3_final_hq", la=512, **dict(BV, hf_floor_db=12))


def _rnnoise(sigs):
    """xiph RNNoise v0.1.1 (BSD) via a tiny CLI; 480-sample (10 ms) algorithmic delay removed."""
    import subprocess, tempfile
    import numpy as np
    clean, noise, mix = sigs
    with tempfile.TemporaryDirectory() as d:
        mix.astype("float32").tofile(f"{d}/in.raw")
        subprocess.run(["/home/claude/ext/rnnoise/rn_cli", f"{d}/in.raw", f"{d}/out.raw"], check=True)
        y = np.fromfile(f"{d}/out.raw", dtype="float32").astype(float)
    y = y[480:480 + len(mix)]
    y = np.pad(y, (0, len(mix) - len(y)))
    # nonlinear: shadow filtering impossible -> speech/noise columns are not meaningful
    return [clean, noise, y]


register("rnnoise")(_rnnoise)


# ---- hybrid: RNNoise gains applied causally through our zero-latency FIR path ----
def _rnn_gains(mix):
    import subprocess, tempfile
    import numpy as np
    with tempfile.TemporaryDirectory() as d:
        mix.astype("float32").tofile(f"{d}/in.raw")
        subprocess.run(["/home/claude/ext/rnnoise/rn_cli_g", f"{d}/in.raw", f"{d}/out.raw", f"{d}/g.raw"], check=True)
        g = np.fromfile(f"{d}/g.raw", dtype="float32").astype(float).reshape(-1, 481)
    nfr = len(mix) // 64
    rows = np.full((nfr, 513), 1.0)
    x481 = np.linspace(0, 1, 481); x513 = np.linspace(0, 1, 513)
    gi = np.stack([np.interp(x513, x481, r) for r in g])
    for i in range(nfr):
        k = ((i + 1) * 64) // 480 - 1   # latest RNN frame finished by sample (i+1)*64
        if 0 <= k < len(gi):
            rows[i] = gi[k]
    return rows


def _hybrid(mode, la=0):
    def f(sigs):
        import numpy as np
        from dsp import GainEstimator, compute_gains, minphase_fir, apply_tv_fir
        clean, noise, mix = sigs
        R = np.maximum(_rnn_gains(mix), 10 ** (-55 / 20))
        if mode == "rnn":
            G = R
        else:
            est = GainEstimator(1024, 64, "hann", **dict(V1, **BV, hf_floor_db=12))
            Gv = compute_gains(mix, est)
            n = min(len(Gv), len(R)); Gv, R = Gv[:n], R[:n]
            if mode == "min":
                G = np.minimum(Gv, R)
            elif mode == "soft":
                G = Gv * np.sqrt(R)
            elif mode == "geo":
                G = np.sqrt(Gv * R)
        H = np.stack([minphase_fir(g, 512, 1024) for g in G])
        return [apply_tv_fir(np.concatenate([np.zeros(la), s]), H, 64)[la:] for s in sigs]
    return f


register("zl_rnn")(_hybrid("rnn"))
register("zl_rnn_la512")(_hybrid("rnn", 512))
register("v3_min_rnn")(_hybrid("min"))
register("v3_x_rnn_soft")(_hybrid("soft"))
register("v3_geo_rnn")(_hybrid("geo"))


def _rnn_onset(scale=1.0, la=0, fthr=4):
    """RNNoise gains (zero latency) + our 256-pt onset path opening bands until the RNN catches up."""
    def f(sigs):
        import numpy as np
        from dsp import GainEstimator, minphase_fir, apply_tv_fir
        clean, noise, mix = sigs
        R = np.maximum(_rnn_gains(mix), 10 ** (-55 / 20))
        est = GainEstimator(1024, 64, "hann", **dict(V1, **dict(BV, hf_floor_db=12, fthr=fthr)))
        n = est.n
        xp = np.concatenate([np.zeros(n), mix])
        G = R.copy()
        for i in range(len(R)):
            end = n + (i + 1) * 64
            est.frame(xp[end - n:end])
            G[i] = np.maximum(R[i], scale * est.gf_last)
        H = np.stack([minphase_fir(g, 512, 1024) for g in G])
        return [apply_tv_fir(np.concatenate([np.zeros(la), s]), H, 64)[la:] for s in sigs]
    return f


register("rnn_onset")(_rnn_onset())
register("rnn_onset_ft6")(_rnn_onset(fthr=6))
register("rnn_onset_half")(_rnn_onset(scale=0.5))
