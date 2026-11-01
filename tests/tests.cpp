// Minimal self-contained tests for the DSP core (no framework needed).
#include "../dsp/FFT.h"
#include "../dsp/ZeroDenoiser.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
        else { std::printf("ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

static double rmsDb(const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0;
    for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return 10.0 * std::log10(s / (double) (b - a) + 1e-30);
}

static void run(zd::ZeroDenoiser& d, std::vector<float>& x, int block)
{
    std::mt19937 rng(7);
    for (size_t pos = 0; pos < x.size();) {
        int bs = block > 0 ? block : 1 + (int) (rng() % 999);
        bs = (int) std::min<size_t>(bs, x.size() - pos);
        float* p = x.data() + pos;
        d.process(&p, 1, bs);
        pos += bs;
    }
}

static void testFFT()
{
    const int n = 256;
    zd::FFT f(n);
    std::vector<float> x(n), y(n);
    std::mt19937 rng(1);
    std::normal_distribution<float> nd;
    for (auto& v : x) v = nd(rng);
    std::vector<zd::FFT::cpx> X(n / 2 + 1);
    f.forwardReal(x.data(), X.data());
    double maxErr = 0;
    for (int k = 0; k <= n / 2; ++k) {
        std::complex<double> ref = 0;
        for (int i = 0; i < n; ++i) ref += (double) x[i] * std::polar(1.0, -2.0 * M_PI * k * i / n);
        maxErr = std::max(maxErr, std::abs(ref - std::complex<double>(X[k])));
    }
    CHECK(maxErr < 1e-3, "FFT matches direct DFT (max err %.2e)", maxErr);
    f.inverseReal(X.data(), y.data());
    double e = 0;
    for (int i = 0; i < n; ++i) e = std::max(e, (double) std::abs(x[i] - y[i]));
    CHECK(e < 1e-5, "FFT round trip (max err %.2e)", e);
}

static void testTransparentAtZero()
{
    // amount = 0 -> unity gain -> output must equal input with NO delay.
    zd::ZeroDenoiser d;
    d.prepare(48000, 1);
    zd::DenoiserParams p; p.amount = 0.0f; p.onsetPath = true;
    d.setParams(p);
    std::vector<float> x(48000), y;
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.1f);
    for (auto& v : x) v = nd(rng);
    y = x;
    run(d, y, 0);
    double e = 0;
    for (size_t i = 0; i < x.size(); ++i) e = std::max(e, (double) std::abs(x[i] - y[i]));
    CHECK(e < 1e-4, "amount=0 is sample-exact passthrough, zero latency (max err %.2e)", e);
}

static void testLookaheadIsExactDelay()
{
    for (int D : { 128, 512 }) {
        zd::ZeroDenoiser d;
        d.prepare(48000, 1);
        zd::DenoiserParams p; p.amount = 0.0f;
        d.setParams(p);
        d.setLookahead(D);
        std::vector<float> x(20000), y;
        std::mt19937 rng(4);
        std::normal_distribution<float> nd(0.f, 0.1f);
        for (auto& v : x) v = nd(rng);
        y = x;
        run(d, y, 0);
        double e = 0;
        for (size_t i = (size_t) D; i < x.size(); ++i) e = std::max(e, (double) std::abs(y[i] - x[i - (size_t) D]));
        CHECK(e < 1e-6 && d.getLookahead() == D, "lookahead %d = exact %d-sample delay (max err %.2e)", D, D, e);
    }
}

static void testBlockSizeIndependence()
{
    std::vector<float> x(96000);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (size_t i = 0; i < x.size(); ++i) x[i] = nd(rng) + (i > 40000 && i < 60000 ? 0.3f * std::sin(0.05f * i) : 0.f);
    std::vector<float> a = x, b = x, c = x;
    zd::ZeroDenoiser d1, d2, d3;
    for (auto* d : { &d1, &d2, &d3 }) { d->prepare(48000, 1); d->setParams({}); }
    run(d1, a, 1);
    run(d2, b, 512);
    run(d3, c, 0);
    double e = 0;
    for (size_t i = 0; i < x.size(); ++i) e = std::max({ e, (double) std::abs(a[i] - b[i]), (double) std::abs(a[i] - c[i]) });
    CHECK(e == 0.0, "output independent of host block size (max diff %.2e)", e);
}

static void testNoiseReduction()
{
    zd::ZeroDenoiser d;
    d.prepare(48000, 1);
    zd::DenoiserParams p; p.amount = 1.0f;
    d.setParams(p);
    std::vector<float> x(48000 * 3);
    std::mt19937 rng(9);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (auto& v : x) v = nd(rng);
    std::vector<float> y = x;
    run(d, y, 0);
    const double red = rmsDb(y, 96000, 144000) - rmsDb(x, 96000, 144000);
    CHECK(red < -25.0, "stationary white noise reduced by %.1f dB at amount=1", red);
}

static void testOnsetPreserved()
{
    // noise bed, then a loud 1 kHz tone starts: the first 5 ms after onset
    // must not be heavily attenuated (onset path).
    zd::ZeroDenoiser d;
    d.prepare(48000, 1);
    zd::DenoiserParams p; p.amount = 0.8f;
    d.setParams(p);
    const size_t onset = 48000 * 2;
    std::vector<float> noise(onset + 4800), tone(noise.size(), 0.f);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 0.02f);
    for (auto& v : noise) v = nd(rng);
    for (size_t i = onset; i < tone.size(); ++i) tone[i] = 0.3f * std::sin(2.f * (float) M_PI * 1000.f * i / 48000.f);
    std::vector<float> y(noise.size());
    for (size_t i = 0; i < y.size(); ++i) y[i] = noise[i] + tone[i];
    run(d, y, 0);
    // compare output to input over 1..6 ms after onset (input ~ tone dominated, +23 dB SNR)
    std::vector<float> in(noise.size());
    for (size_t i = 0; i < y.size(); ++i) in[i] = noise[i] + tone[i];
    const double loss = rmsDb(y, onset + 48, onset + 288) - rmsDb(in, onset + 48, onset + 288);
    CHECK(loss > -3.0, "tone onset 1-6 ms after start loses only %.1f dB", loss);
}

static void testHighSampleRates()
{
    for (double fs : { 44100.0, 96000.0, 192000.0 }) {
        zd::ZeroDenoiser d;
        d.prepare(fs, 1);
        zd::DenoiserParams p; p.amount = 1.0f;
        d.setParams(p);
        const size_t n = (size_t) (fs * 3);
        std::vector<float> x(n);
        std::mt19937 rng(9);
        std::normal_distribution<float> nd(0.f, 0.05f);
        for (auto& v : x) v = nd(rng);
        std::vector<float> y = x;
        run(d, y, 0);
        const double red = rmsDb(y, n * 2 / 3, n) - rmsDb(x, n * 2 / 3, n);
        const double binHz = fs / d.getFFTSize();
        CHECK(red < -25.0 && binHz > 40.0 && binHz < 50.0,
              "%.1f kHz: white noise reduced %.1f dB, analysis resolution %.1f Hz/bin", fs / 1000.0, red, binHz);
    }
}

static void testLearnAndFreeze()
{
    // 3 s noise (learned), then 25 s noise + steady tone. An adaptive tracker
    // slowly absorbs a steady tone as "noise"; a frozen learned profile keeps it.
    const double fs = 48000.0;
    const size_t nLearn = (size_t) (3 * fs), n = nLearn + (size_t) (25 * fs);
    std::vector<float> noise(n), tone(n, 0.f);
    std::mt19937 rng(21);
    std::normal_distribution<float> nd(0.f, 0.02f);
    for (auto& v : noise) v = nd(rng);
    for (size_t i = nLearn; i < n; ++i) tone[i] = 0.1f * std::sin(2.f * (float) M_PI * 440.f * (float) i / (float) fs);
    double toneKeep[2] = { 0, 0 };
    for (int mode = 0; mode < 2; ++mode) {
        zd::ZeroDenoiser d;
        d.prepare(fs, 1);
        zd::DenoiserParams p; p.amount = 1.0f; p.learn = true;
        d.setParams(p);
        std::vector<float> y(n), toneOut(n);
        for (size_t i = 0; i < n; ++i) y[i] = noise[i] + tone[i];
        for (size_t pos = 0; pos < n; pos += 480) {
            if (pos >= nLearn && p.learn) { p.learn = false; p.adaptive = (mode == 0); d.setParams(p); }
            float* ptr = y.data() + pos;
            d.process(&ptr, 1, (int) std::min<size_t>(480, n - pos));
        }
        // level of the output in the last 3 s vs the tone alone
        toneKeep[mode] = rmsDb(y, n - (size_t) (3 * fs), n) - rmsDb(tone, n - (size_t) (3 * fs), n);
    }
    CHECK(toneKeep[1] > -1.0, "learned + frozen profile keeps a 25 s steady tone (%.1f dB)", toneKeep[1]);
    std::printf("     (adaptive mode after 25 s: %.1f dB)\n", toneKeep[0]);
}

static void testRobustness()
{
    zd::ZeroDenoiser d;
    d.prepare(44100, 2);
    d.setParams({});
    std::vector<float> l(44100, 0.f), r(44100, 0.f);
    for (size_t i = 22050; i < l.size(); ++i) { l[i] = (i % 2) ? 1.f : -1.f; r[i] = 1e-30f; }
    float* ch[2] = { l.data(), r.data() };
    d.process(ch, 2, (int) l.size());
    bool finite = true;
    for (size_t i = 0; i < l.size(); ++i) finite &= std::isfinite(l[i]) && std::isfinite(r[i]);
    CHECK(finite, "silence -> full-scale Nyquist + denormal input stays finite (stereo, 44.1k)");
}

static void testAiMode()
{
    // AI mode as the plugin runs it: params.ai + 480-sample lookahead.
    auto make = [](zd::ZeroDenoiser& d, float amount) {
        d.prepare(48000, 1);
        zd::DenoiserParams p; p.ai = true; p.amount = amount;
        d.setParams(p);
        d.setLookahead(480);
    };
    std::vector<float> x(48000 * 3);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (auto& v : x) v = nd(rng);
    // amount 0 -> exact 480-sample delay
    {
        zd::ZeroDenoiser d; make(d, 0.0f);
        std::vector<float> y = x; run(d, y, 0);
        double e = 0;
        for (size_t i = 480; i < x.size(); ++i) e = std::max(e, (double) std::abs(y[i] - x[i - 480]));
        CHECK(e < 1e-5, "AI amount=0 = exact 480-sample delay (max err %.2e)", e);
    }
    // block-size independence
    {
        zd::ZeroDenoiser d1, d2, d3; make(d1, 1.0f); make(d2, 1.0f); make(d3, 1.0f);
        std::vector<float> a = x, b = x, c = x;
        run(d1, a, 1); run(d2, b, 512); run(d3, c, 0);
        double e = 0;
        for (size_t i = 0; i < x.size(); ++i) e = std::max({ e, (double) std::abs(a[i] - b[i]), (double) std::abs(a[i] - c[i]) });
        CHECK(e == 0.0, "AI output independent of host block size (max diff %.2e)", e);
        const double red = rmsDb(a, 48000 * 2, x.size()) - rmsDb(x, 48000 * 2, x.size());
        // RNNoise was trained on real noise + speech; on synthetic full-band white
        // noise it only reduces ~10-15 dB (known limitation, see README). Sanity check only.
        CHECK(red < -8.0, "AI engages on white noise (%.1f dB; DSP modes reach < -25 dB)", red);
    }
    // switching AI on/off at runtime stays finite
    {
        zd::ZeroDenoiser d; make(d, 1.0f);
        std::vector<float> y = x;
        bool finite = true;
        for (size_t pos = 0; pos < y.size(); pos += 4800) {
            zd::DenoiserParams p = d.getParams(); p.ai = ((pos / 4800) % 2) == 0; d.setParams(p);
            float* q = y.data() + pos; d.process(&q, 1, 4800);
        }
        for (float v : y) finite &= std::isfinite(v) && std::abs(v) < 1.0f;
        CHECK(finite, "AI on/off toggling every 100 ms stays finite and bounded");
    }
}

int main()
{
    testFFT();
    testTransparentAtZero();
    testLookaheadIsExactDelay();
    testBlockSizeIndependence();
    testNoiseReduction();
    testOnsetPreserved();
    testHighSampleRates();
    testLearnAndFreeze();
    testRobustness();
    testAiMode();
    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
