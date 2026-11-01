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

## Voice guard (v0.8, 2026-11-01)

RNNoise cuts voices it was not trained on: on EARS screams/yells it removed 4 dB on
average (5 dB above 2 kHz, 37 % of loud frames cut by more than 6 dB), and laughter
lost 1.6 dB of highs. Lowering the input 12 dB changed nothing, so it is the voice
character (pitch, roughness), not the level. A second effect: a sustained scream was
learned as *noise* by the DSP tracker, so the hiss assist then cut it as well.

Fix (AI mode only, on by default): a periodicity detector on the part of the signal that
stands clearly above the noise floor (300 Hz-5 kHz, f0 100-1600 Hz, at least ~6 dB above
2x the floor), plus loud onsets from the fast path. While it fires, the network may not cut
below the DSP gain, and the noise profile is held (budget 2 s per ~6 s, none in the first
second). Release 15 ms so noise does not leak after a word ends. Quiet background voices
(cafe babble) are deliberately *not* protected.

`prototype/expreval.py` (EARS loud / highpitch / scream+yell / anger / singing / laughter,
4 speakers, 4 noises, 10 dB vs regular speech), speech-component change:

| | scream | laugh | anger | onset (all styles) |
|---|---|---|---|---|
| Zero latency | -0.04 dB | -0.20 | -0.94 | -2.1 dB |
| AI v0.7 | -4.07 dB | -0.62 | -0.79 | -3.8 dB |
| AI v0.8 | -0.77 dB | -0.11 | -0.56 | -0.8 dB |

Real-noise set (14 noises): OVRL 2.92 (v0.7 2.94), pause -23.9 dB (v0.7 -25.2), LSD 2.05
(2.51). Car pauses are the main cost (-13 vs -27 dB in v0.7, still better than Zero latency).
Quiet speech onsets buried in 5-10 dB SNR noise are unchanged (-13 dB first 20 ms; HQ mode -6).

## Voice guard v0.9 (2026-11-01)

The user still heard parts of screams vanish in v0.8. Frame audit (`prototype/scream_frames.py`,
4 speakers x 4 noises): 672 of 8448 loud scream frames were cut by more than 6 dB, almost all in
long screams. Cause: the guard measured "above the noise" against the adaptive tracker, which
catches up with a sustained scream, so the guard released.

v0.9: the guard's reference is a separate minimum-statistics floor (min over 6 x 0.5 s of a
30 ms smoothed periodogram, x2 bias), which a scream cannot raise for ~3 s; the protective gain
is computed against it too; gate 3..8 x the floor; pitched protection is limited to the
detected voice's harmonics (onsets stay broadband); a short tail floor (-12 dB, 150 ms).
Result: cut frames 672 -> about 60 (the rest are breath/decay at noise level), scream
speech-component change -0.77 -> -0.01 dB.

Cost, measured on the 14-noise set: pause attenuation -24.0 -> -19.1 dB, OVRL 2.92 -> 2.87; mainly
cafe (-32 -> -14 dB) and hallway/traffic, because background voices now count as voices.
Gate 4..12 (env ZD_RLO/ZD_RHI) is the middle ground: scream -0.08 dB, cafe -21 dB.

### Strong voice protect switch (v0.9.1)

Protecting every scream frame and removing background chatter pull in opposite directions, so
the gate is now a user switch (parameter `voiceprotect`, "Strong voice protect", AI mode only).
Off (default): gate 4..12 x floor, ~93 of 8448 loud scream frames cut >6 dB, cafe pauses -21 dB.
On: gate 3..8, ~61 frames cut, cafe pauses -14 dB. Use On for screaming / shouting content in
quiet-ish noise; Off when other people talk in the background.

## Reduction and Gate (v0.10)

User asked for a settable reduction depth and a "below X dB = silence" threshold.

`Reduction` (0..80 dB, default 40): hard limit on attenuation (`DenoiserParams::reductionDb`;
< 0 keeps the old amount x 55 dB mapping for the library/tests). Amount now only sets how
eagerly noise is removed (over-subtraction). Measured (AI, 6 noises x 5/10 dB SNR,
`cppeval.py ai ai_red60 ai_red80`): pause -19.6 dB at 40, -19.8 at 60/80 - the floor is not
what limits pauses; the residual comes from the network's own gains, the hiss-assist cap and
the voice guard. So Reduction is mainly useful to remove *less*.

`Gate Threshold` (-90 = Off .. -10 dBFS): after the denoiser; detector = loudest input channel,
0.5 ms attack / 10 ms release on x^2 (peak-ish), 3 dB hysteresis, 80 ms hold, opens in ~1 ms,
closes to exact zero in 40 ms. Detector runs on the undelayed input, so in lookahead modes the
gate opens before the onset (AI: onset 0.0 dB; zero latency: first 2 ms -3 dB).
`prototype/gateeval.py` (threshold = peak of the meter in a noise-only pause + 2 dB, AI mode):
pauses fully muted 76-83 %, but speech frames cut > 6 dB: 37.5 % at 10 dB SNR, 16 % at 20 dB
(quiet speech 60 % / 28 %); typing 76 %, cafe 32 %, hiss 5 %. Detecting on the denoised output
instead (`gateDetectOutput`, ZD_GATEOUT) helps steady noise (AirCon 15 -> 7 %) but not hiss or
typing; the input detector was kept because its level matches the meter the user sets it by.
A level gate only works when the voice is well above the noise.

## Pause cleanup (v0.11)

User asked to remove noise more deeply where nobody speaks (instead of a level gate).
`DenoiserParams::pauseCleanup` / plugin "Pause Cleanup" (AI mode, default off): presence =
max(network VAD, voice guard incl. onsets); after 150 ms without presence every bin falls to the
Reduction floor (60 ms, interpolated in dB), any presence restores at once (10 ms ahead of the
audio thanks to the AI lookahead). Tail floor drops to -20 dB while it is on.

Diagnosis first (`prototype/pausediag.py`, residual noise by region, 6 noises): with v0.10 the
residual was not limited by the floor. Typing passes almost untouched (0 dB) and office noise
right after speech too (-4.8 dB) because the voice guard's onset part treats clicks as voice
onsets; removing the onset part fixes that but costs scream onsets 3 dB and laughs 8.7 % dropped
frames, so it stays. Presence from the network VAD alone gives the deepest pauses (-23 dB) but
RNNoise does not call screams speech: scream onsets -19 dB. Pitched-only presence: scream onsets
-10 dB. Network VAD + full guard (shipped) keeps screams at -0.03 dB.

Result (cppeval, 6 noises x 5/10 dB): pause -19.6 -> -23.7 dB, OVRL 2.80 -> 2.84, onsets
-11.8 -> -14.1 dB (quiet onsets after silence lose ~2 dB: reason for default off). Expressive set:
scream unchanged, anger onset -1.8 -> -3.5 dB. Office gap -18.6 -> -19.1 (clicks keep it open),
cafe -20 -> -25, hiss -34 -> -40, aircon/traffic -35 -> -40, typing unchanged (0 dB).
Reduction 60 with pause cleanup: pause -24.6, OVRL 2.88, but onsets -20.6 dB - keep 40.

## Hiss fix (v0.12)

User: "hiss is not removed at all". `prototype/hissdiag.py` (hiss at 10 dB SNR, residual by region
and band) found two causes:
1. Start-up bug (all modes): the first analysis frames are mostly zero padding, so the noise
   estimate started near zero and the speech-presence logic needed 1-3 s to climb. First second
   -0.5 dB (zero) / -1 dB (AI). Now the estimate is taken directly until the buffer is full and
   averaged plainly during the 0.1 s warm-up: first second -19 dB (AI) / -18..-37 (zero); the
   listening sample's 3 s lead-in: -3 dB -> -29 dB.
2. Hiss under the voice in AI mode (-9.4 dB above 6 kHz vs -17 in v0.7): the voice guard's
   floor reference was too permissive (min-statistics x2 bias, but 4 ms periodogram fluctuations
   exceed it half the time), the tail floor was flat over all bins, and the DSP 'air' floor
   (-12 dB above 7 kHz while speaking) kept hiss as well as breath. Now floorBias 4, tail floor only
   where there is signal above the floor, air floor only where HF exceeds the noise estimate, hiss
   assist cap 30 dB (was 20).
Result (AI): under the voice >6 kHz -9.4 -> -14.6, 2-6 kHz -7.4 -> -8.5; right after speech
-18.6 -> -24.4; lead-in 1-3 s -11 -> -30. 6-noise cppeval OVRL 2.81 -> 2.85, onsets -12.6 -> -12.3.
Expressive: scream frames cut >6 dB 0.5 % -> 1.4 % (speech -0.07 -> -0.16 dB), anger 14.2 -> 15.0 %.
Zero latency: under the voice >6 kHz -12.8 -> -16.9, OVRL 2.84 -> 2.86.
Hiss in the voice band (0-2 kHz) under the voice stays at about -5 dB (AI) / -7 dB (zero): those
bins carry the voice; removing it there needs a different (non-gain) method.
