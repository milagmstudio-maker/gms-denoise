#pragma once
// ZeroDenoiser: single-knob, zero-latency broadband noise reduction.
//
// Signal path: input -> time-varying minimum-phase FIR -> output (no buffering
// delay, reported latency 0).  The FIR is re-designed every `kHop` samples from
// a causal spectral analysis of the most recent input:
//   * long analysis (1024-pt FFT): SPP-based noise PSD tracking and a
//     decision-directed Wiener gain per bin;
//   * fast onset path (256-pt FFT, ~1/3-octave bands): opens bands immediately
//     when energy clearly exceeds the band noise, to protect speech onsets that
//     the long analysis only sees ~10 ms later.
// Consecutive filters are crossfaded over one hop.
//
// All channels share one analysis (linked) so the stereo image is preserved.

#include "AiGains.h"
#include "FFT.h"
#include <vector>

namespace zd {

struct DenoiserParams {
    float amount = 0.7f;        // 0..1 single knob
    float fastThreshold = 4.0f; // onset detector threshold (variance-normalised)
    bool onsetPath = true;
    bool learn = false;    // while true: treat input as noise and learn its spectrum
    bool adaptive = true;  // false: freeze the noise profile (after learning)
    bool ai = false;       // gains from the RNNoise network instead of the DSP estimator
};

class ZeroDenoiser {
public:
    // Sizes at 44.1/48 kHz; multiplied by getScale() (1, 2 or 4) at higher rates.
    static constexpr int kBaseFFT = 1024;  // long analysis size
    static constexpr int kBaseHop = 64;    // filter update interval
    static constexpr int kBaseFast = 256;  // onset-path FFT size
    static constexpr int kBaseTaps = 512;  // FIR length
    static constexpr int kBaseMaxLookahead = 1024;

    void prepare(double sampleRate, int numChannels);
    void reset();
    void setParams(const DenoiserParams& p);

    // Lookahead (samples): the audio is delayed by this much while the analysis
    // runs on the undelayed input, so gains are decided with that much "future".
    // 0 = true zero latency. Reported plugin latency must equal this value.
    void setLookahead(int samples);
    int getLookahead() const { return lookahead; }
    const DenoiserParams& getParams() const { return params; }

    // In-place processing of non-interleaved channels. Any block size.
    void process(float* const* channels, int numChannels, int numSamples);

    int getScale() const { return scale; }
    int getFFTSize() const { return nFFT; }
    int getNumBins() const { return nBins; }

    // Latest per-bin gain (for metering/UI). Size getNumBins().
    const std::vector<float>& currentGain() const { return gainOut; }

private:
    void analyse();          // runs once per hop, designs nextFilter
    void designMinPhase(const float* gain, float* hOut);
    void analyseAi();        // AI mode: filter from the latest network gains

    DenoiserParams params;
    double fs = 48000.0;
    int nCh = 0;
    int scale = 1, nFFT = kBaseFFT, nBins = kBaseFFT / 2 + 1, hop = kBaseHop, nFast = kBaseFast,
        nTaps = kBaseTaps, maxLookahead = kBaseMaxLookahead;

    // analysis input (mono sum), circular with doubled storage
    std::vector<float> anaBuf; int anaPos = 0;
    // per-channel FIR history, doubled ring for contiguous reads
    std::vector<std::vector<float>> hist; int histPos = 0;
    int hopCount = 0;
    int lookahead = 0;
    std::vector<std::vector<float>> delay; int delayPos = 0;

    // filters (stored time-reversed for direct dot products)
    std::vector<float> curRev, nextRev;
    std::vector<float> fade;

    // analysis state
    FFT fftLong, fftFast, fftMp;
    std::vector<float> winLong, winFast, frame, tmp;
    std::vector<FFT::cpx> spec, specFast, specMp;
    std::vector<double> noise, pbar, prevS, gPrev, gfPrev;
    std::vector<float> gain, gainOut, hTmp;
    std::vector<int> bandEdges;
    std::vector<double> fnoise, graw, ps, psm, gRel;
    double vad = 0.0, aRel = 0.0, aVadRel = 0.0, fastVad = 0.0;
    int fastVadLo = 0, fastVadHi = 0, hfStartBin = 0;
    static constexpr double kHfFloor = 0.25118864315095801; // -12 dB
    long frames = 0;
    int warmFrames = 0;
    double aNoise = 0, aP = 0, aDD = 0, aFastRel = 0, aPs = 0, aLearn = 0;
    long learnFrames = 0;

    AiGains ai;
    bool aiNew = false, aiActive = false;
};

} // namespace zd
