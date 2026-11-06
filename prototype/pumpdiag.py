"""Pumping ('breathing') measure: residual noise level under the voice vs in pauses.
Per mode: noise attenuation in speech frames, in pauses (>300 ms after speech, after 3 s),
and their difference = how much the background jumps when the voice starts."""
import sys, os, numpy as np
sys.path.insert(0, "/home/claude/zerodenoise/prototype")
import cppeval as ce, realeval as re_
FS = 48000
noises = os.environ.get("NOISES", "hiss").split(",")
snr = float(os.environ.get("SNR", "10"))
for m in sys.argv[1:]:
    acc = {"sp": [0, 0], "gap": [0, 0]}
    for name in noises:
        for spk in range(4):
            clean = re_.speech_clip(spk); nz = re_.noise_clip(name, len(clean), spk)
            ps = np.mean(clean[np.abs(clean) > 0.01] ** 2); nz = nz * np.sqrt(ps / 10 ** (snr / 10)); mix = clean + nz
            oc, on, om = ce.run_mode(m, clean, nz, mix)
            fl = 480; nf = len(clean) // fl
            e = lambda x: np.array([np.sum(x[k * fl:(k + 1) * fl] ** 2) for k in range(nf)])
            ec, ei, eo = e(clean), e(nz), e(on)
            act = ec > ec.max() * 1e-4  # clearly audible voice (within 40 dB of peak)
            sil = ec < ec.max() * 1e-8
            since = np.zeros(nf); last = -1e9
            for k in range(nf):
                if not sil[k]: last = k
                since[k] = k - last
            gap = sil & (since > 30) & (np.arange(nf) >= 300)
            for key, msk in (("sp", act), ("gap", gap)):
                acc[key][0] += eo[msk].sum(); acc[key][1] += ei[msk].sum()
    sp = 10 * np.log10(acc["sp"][0] / acc["sp"][1]); gp = 10 * np.log10(acc["gap"][0] / acc["gap"][1])
    print(f"{m:12s} under voice {sp:6.1f} dB   pauses {gp:6.1f} dB   jump {sp - gp:5.1f} dB", flush=True)
