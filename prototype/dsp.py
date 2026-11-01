"""Prototype: causal noise-PSD estimation + gain computation, and several ways
to apply the gains (STFT reference with latency vs. zero-latency time-domain
filter-bank-equalizer style application).

All appliers are linear given the gain trajectory, so we can run the SAME
filters over clean speech and noise separately to measure speech distortion
and noise attenuation independently ("shadow filtering").
"""
import numpy as np

FS = 48000


def hann(n):
    return 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(n) + 0.5) / n)


def asym_window(n, tail):
    """Asymmetric analysis window: long rising half, short falling tail.
    Puts most weight on the newest samples -> less lag of the gain estimate."""
    rise = n - tail
    w = np.empty(n)
    w[:rise] = np.sin(0.5 * np.pi * (np.arange(rise) + 0.5) / rise) ** 2
    w[rise:] = np.cos(0.5 * np.pi * (np.arange(tail) + 0.5) / tail) ** 2
    return w


class GainEstimator:
    """Causal per-bin gain estimator.
    Noise PSD: SPP-based MMSE tracker (Gerkmann & Hendriks 2012 style).
    Gain: decision-directed a-priori SNR, Wiener-type with over-subtraction
    and a floor driven by a single 'amount' control (0..1)."""

    def __init__(self, n_fft=1024, hop=256, window="hann", tail=128, amount=0.7,
                 fs=FS, dd_tau=0.1, rel_tau=0.02, omlsa=False, fast=0, fast_rel=0.03, fthr=4.0, fband_min=2, psmooth=0.0):
        self.n, self.hop, self.fs = n_fft, hop, fs
        self.win = hann(n_fft) if window == "hann" else asym_window(n_fft, tail)
        self.amount = amount
        nb = n_fft // 2 + 1
        self.noise = None
        self.pbar = np.zeros(nb)
        self.prev_s = np.zeros(nb)  # previous clean-power estimate (DD)
        frame_t = hop / fs
        self.a_noise = np.exp(-frame_t / 0.08)
        self.a_p = np.exp(-frame_t / 0.05)
        self.a_dd = np.exp(-frame_t / dd_tau)
        self.a_rel = np.exp(-frame_t / rel_tau) if rel_tau > 0 else 0.0
        self.omlsa = omlsa
        self.xi_h1 = 10 ** (15 / 10)
        self.g_prev = np.ones(nb)
        self.a_ps = np.exp(-frame_t / psmooth) if psmooth > 0 else 0.0
        self.Psm = None
        self.frames = 0
        self.warm = int(0.1 * fs / hop)
        # fast onset path: short window on the newest samples
        self.fast = fast
        self.fthr, self.fband_min = fthr, fband_min
        if fast:
            self.wf = hann(fast)
            self.ratio = (self.wf ** 2).sum() / (self.win ** 2).sum()
            self.gf_prev = np.zeros(nb)
            self.a_frel = np.exp(-frame_t / fast_rel)

    def set_amount(self, a):
        self.amount = a

    def frame(self, x_frame):
        X = np.fft.rfft(x_frame * self.win)
        P = (X.real ** 2 + X.imag ** 2)
        if self.Psm is None:
            self.Psm = P.copy()
        self.Psm = self.a_ps * self.Psm + (1 - self.a_ps) * P
        P = self.Psm
        if self.noise is None:
            self.noise = P.copy() + 1e-12
        # --- noise PSD tracking (SPP) ---
        snr_post = P / self.noise
        xi = self.xi_h1
        spp = 1.0 / (1.0 + (1 + xi) * np.exp(-snr_post * xi / (1 + xi)))
        self.pbar = self.a_p * self.pbar + (1 - self.a_p) * spp
        spp = np.where(self.pbar > 0.99, np.minimum(spp, 0.99), spp)
        n_est = (1 - spp) * P + spp * self.noise
        a = self.a_noise if self.frames > self.warm else 0.5  # fast initial convergence
        self.noise = a * self.noise + (1 - a) * n_est
        self.frames += 1
        # --- gain ---
        amt = self.amount
        over = 1.0 + 1.5 * amt
        gmin = 10 ** (-(amt * 40.0) / 20)
        nz = self.noise * over
        gamma = P / nz
        xi_dd = self.a_dd * self.prev_s / nz + (1 - self.a_dd) * np.maximum(gamma - 1, 0)
        xi_dd = np.maximum(xi_dd, 1e-4)
        g = xi_dd / (1 + xi_dd)
        # spectral smoothing (3-bin) to tame musical noise
        g = np.convolve(g, [0.25, 0.5, 0.25], mode="same")
        # temporal: instant attack, smoothed release
        g = np.maximum(g, self.a_rel * self.g_prev + (1 - self.a_rel) * g)
        self.g_prev = g
        self.prev_s = (g ** 2) * P
        if self.fast:
            m = self.n // self.fast
            Xs = np.fft.rfft(x_frame[-self.fast:] * self.wf)
            Ps = Xs.real ** 2 + Xs.imag ** 2
            # noise projected to the short resolution
            ns = np.add.reduceat(self.noise[:-1], np.arange(0, self.n // 2, m)) / m
            ns = np.append(ns, ns[-1]) * self.ratio * over
            # aggregate into ~1/3-octave bands to cut estimator variance
            if not hasattr(self, "fedges"):
                nbs = len(Ps)
                edges = [1]
                while edges[-1] < nbs - 1:
                    w = max(self.fband_min, int(edges[-1] * 0.26))
                    edges.append(min(nbs - 1, edges[-1] + w))
                self.fedges = np.array(edges)
            e = self.fedges
            Pb = np.add.reduceat(Ps, e[:-1])[:len(e) - 1]
            # band noise tracked in the short-window domain itself (handles tonal leakage)
            if not hasattr(self, "fnoise") or self.frames <= self.warm + 1:
                self.fnoise = Pb.copy() if not hasattr(self, "fnoise") else 0.5 * self.fnoise + 0.5 * Pb
            else:
                sp = 1.0 / (1.0 + (1 + xi) * np.exp(-(Pb / self.fnoise) * xi / (1 + xi)))
                self.fnoise = self.a_noise * self.fnoise + (1 - self.a_noise) * ((1 - sp) * Pb + sp * self.fnoise)
            Nb = self.fnoise * over
            K = np.diff(e)
            snr = Pb / Nb
            T = 1 + self.fthr / np.sqrt(K)       # variance-aware threshold
            gb = np.clip((snr - T) / (snr + 1e-12), 0, 1) ** 0.5
            gs = np.zeros(len(Ps))
            for j in range(len(K)):
                gs[e[j]:e[j + 1]] = gb[j]
            gs[-1] = gb[-1]
            gl = np.repeat(gs[:-1], m)
            gl = np.append(gl, gs[-1])
            gf = np.maximum(gl, self.a_frel * self.gf_prev)
            self.gf_prev = gf
            g = np.maximum(g, gf)
        if self.omlsa:
            # speech-presence weighting (OM-LSA flavour): p from a-priori SNR,
            # smoothed over frequency (local + wide) to reject isolated peaks
            loc = np.convolve(xi_dd, np.ones(5) / 5, mode="same")
            wide = np.convolve(xi_dd, np.ones(31) / 31, mode="same")
            p = np.clip((10 * np.log10(np.maximum(loc, 1e-6)) + 5) / 10, 0, 1)
            pw = np.clip((10 * np.log10(np.maximum(wide, 1e-6)) + 5) / 10, 0, 1)
            p = p * pw
            g = np.exp(p * np.log(np.maximum(g, gmin)) + (1 - p) * np.log(gmin))
        else:
            g = gmin + (1 - gmin) * g
        return g, X


def compute_gains(x, est: GainEstimator):
    """Run estimator causally. Gain row i is available at sample (i+1)*hop
    (frame covers x[(i+1)*hop - n : (i+1)*hop])."""
    n, hop = est.n, est.hop
    xp = np.concatenate([np.zeros(n), x])
    nfr = len(x) // hop
    G = np.empty((nfr, n // 2 + 1))
    for i in range(nfr):
        end = n + (i + 1) * hop
        G[i], _ = est.frame(xp[end - n:end])
    return G


# ---------------------------------------------------------------------------
# Appliers
# ---------------------------------------------------------------------------

def apply_stft(sig, G, n, hop):
    """Reference: classic WOLA STFT with sqrt-hann. Latency = n samples
    (we return the output already aligned, plus the latency in samples).
    Gain for a frame is the gain computed from that same frame (non-causal
    w.r.t. the output, which is why latency is needed)."""
    w = np.sqrt(hann(n))
    norm = n / (2 * hop) if hop < n else 1
    xp = np.concatenate([np.zeros(n), sig, np.zeros(n)])
    out = np.zeros(len(xp))
    nfr = G.shape[0]
    # compensate sqrt-hann^2 = hann; OLA sum of hann with hop n/4 = 2
    ola = np.zeros(len(xp))
    for i in range(nfr):
        end = n + (i + 1) * hop
        fr = xp[end - n:end] * w
        Y = np.fft.rfft(fr) * G[i]
        out[end - n:end] += np.fft.irfft(Y, n) * w
        ola[end - n:end] += w * w
    ola[ola < 1e-6] = 1
    out = out / ola
    # out[k] corresponds to sig[k - n]; return aligned
    return out[n:n + len(sig)], n


def minphase_fir(g, L, nfft_mp=4096):
    """Minimum-phase FIR (length L) whose magnitude approximates g (rfft bins)."""
    nb = nfft_mp // 2 + 1
    gi = np.interp(np.linspace(0, 1, nb), np.linspace(0, 1, len(g)), g)
    logm = np.log(np.maximum(gi, 1e-5))
    c = np.fft.irfft(logm, nfft_mp)
    fold = np.zeros(nfft_mp)
    fold[0] = c[0]
    fold[1:nfft_mp // 2] = 2 * c[1:nfft_mp // 2]
    fold[nfft_mp // 2] = c[nfft_mp // 2]
    h = np.fft.irfft(np.exp(np.fft.rfft(fold)), nfft_mp)[:L]
    tail = L // 4
    h[L - tail:] *= np.cos(0.5 * np.pi * (np.arange(tail) + 0.5) / tail) ** 2
    return h


def linphase_fir(g, L, nfft_mp=4096):
    nb = nfft_mp // 2 + 1
    gi = np.interp(np.linspace(0, 1, nb), np.linspace(0, 1, len(g)), g)
    h = np.fft.irfft(gi, nfft_mp)
    h = np.roll(h, L // 2)[:L] * hann(L)
    return h


def filters_from_gains(G, L, kind="min"):
    f = minphase_fir if kind == "min" else linphase_fir
    return np.stack([f(g, L) for g in G])


def apply_tv_fir(sig, H, hop, extra_delay_frames=0):
    """Zero-latency time-varying FIR. Filter H[i] becomes available at sample
    (i+1)*hop (+ extra_delay_frames*hop); between updates we crossfade from the
    old filter output to the new one over one hop."""
    L = H.shape[1]
    N = len(sig)
    xp = np.concatenate([np.zeros(L - 1), sig])
    out = np.zeros(N)
    fade = 0.5 - 0.5 * np.cos(np.pi * (np.arange(hop) + 0.5) / hop)
    nblocks = (N + hop - 1) // hop
    h_unity = np.zeros(L)
    h_unity[0] = 1.0
    cur = h_unity
    for b in range(nblocks):
        s, e = b * hop, min((b + 1) * hop, N)
        # filter index ready at start of this block: H[i] ready at (i+1)*hop
        i = b - 1 - extra_delay_frames
        new = H[i] if i >= 0 and i < len(H) else (H[-1] if i >= len(H) else h_unity)
        seg = xp[s:e + L - 1]
        y_old = np.convolve(seg, cur, mode="valid")
        y_new = np.convolve(seg, new, mode="valid")
        f = fade[:e - s]
        out[s:e] = (1 - f) * y_old + f * y_new
        cur = new
    return out
