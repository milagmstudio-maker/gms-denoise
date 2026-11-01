# ZeroDenoise

Single-knob broadband noise reduction with **zero reported latency**, as a
VST3 / AU plugin (JUCE 8).

## How it works

Classic spectral denoisers (STFT spectral subtraction / Wiener) need one
analysis window of latency (about 20 ms). ZeroDenoise keeps the spectral
*analysis* but moves the *filtering* to the time domain:

1. Every 64 samples a 1024-point FFT of the most recent input feeds a
   speech-presence-probability noise tracker and a decision-directed Wiener
   gain per bin. One `Amount` control sets over-subtraction and the maximum
   attenuation (0 to 40 dB).
2. A parallel 256-point onset detector (about 1/3-octave bands) opens bands
   immediately when energy clearly exceeds the band noise floor. This protects
   speech onsets, which the long analysis only sees about 10 ms late.
3. The gains become a 512-tap minimum-phase FIR. The audio runs through it
   sample by sample, and consecutive filters are crossfaded over each hop.

Nothing is buffered on the signal path, so the plugin reports 0 samples of
latency. The cost of zero latency is that gains are always decided from past
audio, so onsets lose some level compared with an STFT design that has latency.
`prototype/` contains the measurements.

## Layout

| Path | Contents |
|---|---|
| `dsp/` | Framework-independent DSP core (`ZeroDenoiser`, small FFT) |
| `plugin/` | JUCE processor and editor |
| `tests/` | Unit tests for the core (zero latency, transparency, block-size independence, noise reduction, onset handling) |
| `tools/zd_cli.cpp` | Offline WAV runner with a CPU-load readout |
| `prototype/` | Python reference implementation and evaluation scripts |

## Build

```bash
# core + tests only (no JUCE needed)
cmake -S . -B build -DZD_BUILD_PLUGIN=OFF && cmake --build build -j && ./build/zd_tests

# plugin (fetches JUCE 8.0.10; AU is added automatically on macOS)
cmake -S . -B build-plugin -DCMAKE_BUILD_TYPE=Release
cmake --build build-plugin -j --target ZeroDenoisePlugin_All
./build-plugin/zd_host_check_artefacts/Release/zd_host_check build-plugin/ZeroDenoisePlugin_artefacts/Release/VST3/ZeroDenoise.vst3
```

On Linux the plugin build needs the usual JUCE packages (ALSA, X11, Xrandr,
Xinerama, Xcursor, Xcomposite, Freetype, Fontconfig, GL dev packages).

## Status / known issues

- The Python and C++ implementations match to about -129 dB.
- `zd_host_check` loads the built VST3 through JUCE hosting and checks latency 0, parameters, processing and state.
- CPU: about 4 % of one core per channel in the test container (naive FFT, no
  explicit SIMD).
- All sizes scale by 2 at 88.2/96 kHz and by 4 at 176.4/192 kHz, so time and frequency resolution (about 47 Hz/bin) stay the same. CPU at 96 kHz is about 9 % of one core per channel.
- Learn: while on, the input is averaged as pure noise. With Adaptive off, the learned profile stays frozen. With it on, tracking continues from the learned profile.
- Versus the 21 ms STFT reference (5 dB SNR): noise in pauses is reduced as much or more, but speech onsets lose 2 to 4 dB more and STOI is 0.02 to 0.04 lower. See `prototype/results_v3.txt`.
- Modes: Zero latency (0 samples), Low latency (128 samples, 2.7 ms at 48 kHz), High quality (512 samples, 10.7 ms), AI (480 samples, 10.0 ms at 48 kHz; 10.9 ms at 44.1 kHz). Lookahead delays the audio while analysis runs on the undelayed input. The editor always shows the current latency in ms and samples.
- Licensing: JUCE 8 is dual-licensed (AGPLv3 or the JUCE commercial licence).
  Check the JUCE terms before distributing closed-source builds. The VST3 SDK
  has been MIT-licensed since VST 3.8.

## v2 tuning on real noise (2026-11-01)

Goal: keep the voice natural while still removing the noise. Evaluation used 12
real noise recordings (MS-SNSD / DEMAND, 16 kHz upsampled), full-band synthetic
hiss and hum, 4 EARS speakers, at 5 and 10 dB SNR. Scores are DNSMOS P.835
(`prototype/realeval.py`); DNSMOS works at 16 kHz, so content above 8 kHz is not judged.

| Version | SIG (voice) | BAK (background) | OVRL | speech-component spectral distortion |
|---|---|---|---|---|
| unprocessed | 3.22 | 2.89 | 2.57 | 0 dB |
| STFT reference (21 ms) | 3.31 | 3.63 | 2.86 | 4.9 dB |
| v1 zero latency | 3.15 | 3.65 | 2.76 | 6.9 dB |
| **v2 zero latency** | **3.31** | **3.72** | **2.90** | 4.8 dB |
| v2 High quality (10.7 ms) | 3.33 | 3.69 | 2.91 | 4.2 dB |

v2 changes: while speech is present the gain is softened (g^0.5), so residual noise under the voice is
masked instead of being carved out. Between words, attenuation goes up to 55 dB, and gains release
over 20 ms instead of jumping every 1.3 ms.

## AI mode (v0.6, 2026-11-01)

Mode "AI (10 ms)": the RNNoise network (xiph, BSD-3, vendored in `third_party/rnnoise`)
estimates per-bin gains every 480 samples; the gains are applied through the same
minimum-phase FIR as the other modes, with 480 samples of lookahead. No audio is
synthesised by the network, so Amount (floor up to 55 dB) and linked stereo still work.
Hiss assist (v0.7, on by default): the DSP estimator keeps running in AI mode. Between
words (DSP speech detector) it may add up to 20 dB of extra attenuation wherever its gain is
lower than the network's; while speech is present the network's gains are used unchanged.
This removes the stationary broadband noise (hiss) the network leaves in. Learn / Adaptive
act on this DSP part. CPU at 48 kHz stereo: about 14 % of one core (Zero latency: 9 %).

Final C++ path (`prototype/cppeval.py`, runs `zd_cli` with shadow filtering), Amount 70 %,
14 noises x 5/10 dB SNR x 4 EARS speakers, DNSMOS P.835:

| Mode | SIG | BAK | OVRL | speech dB | onset dB | pause dB | LSD | STOI |
|---|---|---|---|---|---|---|---|---|
| unprocessed | 3.25 | 2.86 | 2.55 | 0.00 | 0.00 | 0.00 | 0.00 | 0.83 |
| Zero latency | 3.32 | 3.72 | 2.91 | -1.05 | -10.74 | -18.62 | 4.34 | 0.80 |
| Low latency | 3.34 | 3.70 | 2.91 | -1.01 | -8.69 | -18.48 | 4.09 | 0.81 |
| High quality | 3.30 | 3.69 | 2.89 | -0.92 | -6.81 | -18.20 | 3.77 | 0.82 |
| AI (10 ms) | 3.27 | 3.82 | 2.92 | -0.75 | -13.42 | -22.70 | 2.12 | 0.83 |
| AI (10 ms) + hiss assist, v0.7 | 3.27 | 3.88 | 2.95 | -0.81 | -13.80 | -25.13 | 2.50 | 0.83 |

AI is clearly better on non-stationary noise (cafe pause -32 vs -10 dB, car -27 vs -8,
copy machine -18 vs -8) and keeps the voice's spectrum closest (LSD 2.1 vs 4.3 dB).
Network alone (v0.6) removed broadband hiss poorly (pause -6 dB vs -25 dB for the DSP
modes; synthetic white noise anywhere between -1 and -30 dB). With the hiss assist (v0.7)
hiss pauses reach -24 dB at 5 and 10 dB SNR, voice spectral distortion on hiss stays at
1.6-2.3 dB (DSP mode 2.5-4.0). Remaining weakness: onsets lose more than the DSP modes
(-14 dB vs -11 dB in the first 20 ms); typing is barely reduced by any mode.
