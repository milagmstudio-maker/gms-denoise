# ZeroDenoise

Single-knob broadband noise reduction with **zero reported latency**, as a
VST3 / AU / Standalone plugin (JUCE 8).

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
- Modes: Zero latency (0 samples), Low latency (128 samples, 2.7 ms at 48 kHz), High quality (512 samples, 10.7 ms). Lookahead delays the audio while analysis runs on the undelayed input. The editor always shows the current latency in ms and samples.
- Licensing: JUCE 8 is dual-licensed (AGPLv3 or the JUCE commercial licence).
  Check the JUCE terms before distributing closed-source builds. The VST3 SDK
  has been MIT-licensed since VST 3.8.
