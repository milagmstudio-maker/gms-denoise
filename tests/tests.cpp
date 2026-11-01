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

int main()
{
    testFFT();
    testTransparentAtZero();
    testBlockSizeIndependence();
    testNoiseReduction();
    testOnsetPreserved();
    testRobustness();
    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
