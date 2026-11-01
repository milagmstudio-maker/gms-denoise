#include "AiGains.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include "rnnoise.h"
}

namespace zd {

AiGains::~AiGains()
{
    if (st) rnnoise_destroy(st);
}

void AiGains::prepare(int decimation)
{
    decim = std::max(1, decimation);
    if (!st) st = rnnoise_create(nullptr);
    frame.assign(kFrame, 0.0f);
    g.assign(kBins, 1.0f);
    // windowed-sinc low-pass at 0.45 * (fs / decim); only used when decim > 1
    const int taps = 32 * decim;
    lp.assign((size_t) taps, 0.0f);
    if (decim > 1) {
        const double fc = 0.45 / decim; // cycles/sample
        double sum = 0.0;
        for (int i = 0; i < taps; ++i) {
            const double t = i - (taps - 1) / 2.0;
            const double sinc = t == 0.0 ? 2.0 * fc : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
            const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4.0 * M_PI * i / (taps - 1));
            lp[(size_t) i] = (float) (sinc * w);
            sum += sinc * w;
        }
        for (auto& c : lp) c = (float) (c / sum);
    }
    lpHist.assign((size_t) (2 * taps), 0.0f);
    reset();
}

void AiGains::reset()
{
    if (st) rnnoise_reset_state(st);
    std::fill(frame.begin(), frame.end(), 0.0f);
    std::fill(g.begin(), g.end(), 1.0f);
    std::fill(lpHist.begin(), lpHist.end(), 0.0f);
    phase = pos = lpPos = 0;
    vadProb = 0.0f;
}

bool AiGains::push(float x)
{
    float y = x;
    if (decim > 1) {
        const int taps = (int) lp.size();
        lpHist[(size_t) lpPos] = x;
        lpHist[(size_t) (lpPos + taps)] = x;
        lpPos = (lpPos + 1) % taps;
        if (++phase < decim) return false;
        phase = 0;
        const float* h = lpHist.data() + lpPos; // oldest..newest
        float s = 0.0f;
        for (int i = 0; i < taps; ++i) s += h[i] * lp[(size_t) (taps - 1 - i)];
        y = s;
    }
    frame[(size_t) pos] = y * 32768.0f; // RNNoise expects 16-bit scale
    if (++pos < kFrame) return false;
    pos = 0;
    vadProb = rnnoise_compute_gains(st, g.data(), frame.data());
    return true;
}

} // namespace zd
