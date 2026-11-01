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

#include "FFT.h"
#include <vector>

namespace zd {

struct DenoiserParams {
    float amount = 0.7f;        // 0..1 single knob
    float fastThreshold = 4.0f; // onset detector threshold (variance-normalised)
    bool onsetPath = true;
};

class ZeroDenoiser {
public:
    static constexpr int kFFT = 1024;   // long analysis size
    static constexpr int kBins = kFFT / 2 + 1;
    static constexpr int kHop = 64;     // filter update interval
    static constexpr int kFast = 256;   // onset-path FFT size
    static constexpr int kTaps = 512;   // FIR length

    void prepare(double sampleRate, int numChannels);
    void reset();
    void setParams(const DenoiserParams& p) { params = p; }
    const DenoiserParams& getParams() const { return params; }

    // In-place processing of non-interleaved channels. Any block size.
    void process(float* const* channels, int numChannels, int numSamples);

    // Latest per-bin gain (for metering/UI). Size kBins.
    const std::vector<float>& currentGain() const { return gainOut; }

private:
    void analyse();          // runs once per hop, designs nextFilter
    void designMinPhase(const float* gain, float* hOut);

    DenoiserParams params;
    double fs = 48000.0;
    int nCh = 0;

    // analysis input (mono sum), circular with doubled storage
    std::vector<float> anaBuf; int anaPos = 0;
    // per-channel FIR history, doubled ring for contiguous reads
    std::vector<std::vector<float>> hist; int histPos = 0;
    int hopCount = 0;

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
    std::vector<double> fnoise, graw, ps, psm;
    long frames = 0;
    int warmFrames = 0;
    double aNoise = 0, aP = 0, aDD = 0, aFastRel = 0, aPs = 0;
};

} // namespace zd
