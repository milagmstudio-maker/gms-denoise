#pragma once
// AiGains: wraps the RNNoise network (BSD, vendored in third_party/rnnoise) as
// a pure *gain estimator*. It is fed the mono analysis signal sample by
// sample; every 480 samples (10 ms at 48 kHz) it produces 481 per-bin gains.
// No audio is synthesised here: ZeroDenoiser applies the gains through its own
// minimum-phase FIR, so the plugin's latency is only the lookahead it chooses
// (no extra frame buffering), Amount still sets the floor and stereo stays linked.
//
// The network was trained at 48 kHz. At 88.2/96 kHz and 176.4/192 kHz the
// analysis signal is decimated by 2 / 4 first; at 44.1 kHz it runs directly
// (band edges shift by ~8%).

#include <vector>

struct DenoiseState;

namespace zd {

class AiGains {
public:
    static constexpr int kFrame = 480;
    static constexpr int kBins = 481;

    AiGains() = default;
    ~AiGains();
    AiGains(const AiGains&) = delete;
    AiGains& operator=(const AiGains&) = delete;

    void prepare(int decimation); // allocates; not real-time safe
    void reset();                 // real-time safe

    // Push one host-rate sample (nominal full scale +-1). Returns true when a
    // new set of gains became available with this sample.
    bool push(float x);
    const float* gains() const { return g.data(); }
    float vadProbability() const { return vadProb; }

private:
    DenoiseState* st = nullptr;
    int decim = 1, phase = 0, pos = 0;
    std::vector<float> lp, lpHist; int lpPos = 0; // decimation low-pass
    std::vector<float> frame, g;
    float vadProb = 0.0f;
};

} // namespace zd
