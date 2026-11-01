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
    bool aiHissAssist = true; // AI mode: DSP estimator adds attenuation for stationary noise (hiss)
    bool aiVoiceGuard = true; // AI mode: where a pitched voice is detected the network may not cut below the DSP gain
    bool aiStrongVoice = false; // AI mode: protect quieter voices too (screams fully kept, more background chatter survives)
    float reductionDb = -1.0f;  // maximum attenuation in dB; < 0: follow amount (amount x 55 dB)
    float gateDb = -200.0f;     // gate threshold on the input level (dBFS); <= kGateOff: gate off
    static constexpr float kGateOff = -90.0f;
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

    // Gate (after the denoiser): input level (dBFS, ~10 ms RMS) and state, for the UI.
    float getInputLevelDb() const { return gateLevelDb; }
    bool isGateOpen() const { return gateOpen; }
    bool gateDetectOutput = false; // detector on the denoised output instead of the input

    // Latest per-bin gain (for metering/UI). Size getNumBins().
    const std::vector<float>& currentGain() const { return gainOut; }

private:
    void analyse();          // runs once per hop, designs nextFilter
    void designMinPhase(const float* gain, float* hOut);
    void analyseAi();        // AI mode: filter from the latest network gains
    void computeDspGain(float* out); // DSP estimator, one hop
    void updateFloor();              // minimum-statistics noise floor (voice guard reference)
    double computeVoicing();         // 0..1 periodicity of the above-noise signal (300 Hz-5 kHz, f0 100-1600 Hz)

    double floorGain() const; // linear minimum gain from reductionDb / amount

    DenoiserParams params;
    double fs = 48000.0;
    // gate: detector on the undelayed input (so lookahead modes open the gate early)
    double gateEnv = 0.0, gateAtkC = 0.0, gateRelC = 0.0, gateGain = 1.0, gateOpenC = 0.0, gateCloseC = 0.0;
    long gateHold = 0, gateHoldLen = 0;
    bool gateOpen = true; float gateLevelDb = -120.0f;
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
    std::vector<float> gAi, gDsp;
    double aiVad = 0.0; // network's speech probability (diagnostics)
    FFT fftAc;
    std::vector<float> acBuf, winAc;
    std::vector<FFT::cpx> acSpec;
    double voiced = 0.0, aVoiceRel = 0.0, lastVoicing = 0.0, lastResid = 0.0;
    long maxVoiceHoldHops = 0, settleFrames = 0;
    // Minimum-statistics floor: min over 6 sub-windows of 0.5 s of a 30 ms smoothed
    // periodogram. A scream cannot raise it for ~3 s, unlike the adaptive tracker.
    static constexpr int kFloorSubs = 6;
    std::vector<double> pSlow, subMin, floorN;
    std::vector<std::vector<double>> floorRing;
    int floorRingPos = 0; long floorHop = 0, floorSubLen = 0; int floorFilled = 0;
    double aSlow = 0.0;
    std::vector<float> gFloor;
    double tailEnv = 0.0, aTail = 0.0, voicedP = 0.0, onsetEnv = 0.0;
    int lastLag = 0, combLag = 0;
    float gDspRawSafe(int) const { return 1.0f; } // tail floor is flat (bins above the voice are masked by it)
public:
    bool dbgFloor = true, dbgComb = true; double floorBias = 2.0, dbgTail = 0.25;
private:
    double holdBudget = 0.0;
    // Voice guard tuning (prototype/expreval.py + cppeval.py, 2026-11-01):
    // periodicity 0.6..0.95, above-noise power 3..8 x (2 x 2 x min-statistics floor),
    // full protection from half confidence. Lower = screams fully kept but more
    // background chatter survives (prototype/expreval.py vs cppeval.py, v0.9).
    double voiceLo = 0.6, voiceHi = 0.95, residLo = 4.0, residHi = 12.0; // Strong: 3..8
public:
    double dbgSat = 2.0; bool dbgOnset = true, dbgNoHold = false; // tuning hooks (zd_cli env)
private:
    int lagMin = 0, lagMax = 0, acKLo = 0, acKHi = 0;
public:
    double getVoicing() const { return voiced; }
    double getRawVoicing() const { return lastVoicing; }
    double getResid() const { return lastResid; }
    void setVoicingThresholds(double lo, double hi) { voiceLo = lo; voiceHi = hi; }
    void setResidThresholds(double lo, double hi) { residLo = lo; residHi = hi; }
    void setVoicingRelease(double sec) { aVoiceRel = std::exp(-(hop / fs) / sec); }
    void setVoicingBand(double loHz) { acKLo = (int) (loHz / fs * 2 * nFFT); }
};

} // namespace zd
