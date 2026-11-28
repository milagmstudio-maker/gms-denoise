// Implementation of sdk/shizumaru.h: the plug-in's processing (PluginProcessor) without JUCE.
#include "shizumaru.h"
#include "../dsp/DfnRunner.h"
#include "../dsp/ZeroDenoiser.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kNumModes = 6;
constexpr int kModeLookahead[kNumModes] = { 0, 128, 512, 480, -1, -1 };
constexpr float kStrengthJumpDb[3] = { 6.0f, 12.0f, 0.0f };
constexpr int kMeterBands = 48;

int pathOfMode(int mode) { return mode == 4 ? 1 : mode == 5 ? 2 : 0; }

struct Buf {
    std::vector<std::vector<float>> ch;
    std::vector<float*> ptr;
    void setSize(int nch, int n)
    {
        ch.assign((size_t) nch, std::vector<float>((size_t) std::max(1, n), 0.0f));
        ptr.resize((size_t) nch);
        for (int c = 0; c < nch; ++c) ptr[(size_t) c] = ch[(size_t) c].data();
    }
    float* const* data() { return ptr.data(); }
};

} // namespace

struct shz_engine {
    zd::ZeroDenoiser denoiser;
    zd::DfnRunner dfn;
    double rate = 48000.0;
    int nch = 2, maxBlock = 512;
    std::atomic<int> mode { 0 };
    std::atomic<int> latency { 0 };
    std::atomic<bool> loading { false };

    std::mutex parLock;
    shz_params pending {}, cur {};
    std::atomic<bool> parDirty { true };

    int path = 0, prevPath = -1, fadeLeft = 0, fadeLen = 960;
    Buf inCopy, pathA, pathB, pathC;
    float outGain = 1.0f;

    std::array<int, kMeterBands + 1> edges {}, dfnEdges {};
    std::array<std::atomic<float>, kMeterBands> meter {};

    std::thread loader;
    std::mutex loaderLock;
    std::condition_variable loaderCv;
    bool stop = false;

    shz_engine()
    {
        pending = cur = shz_default_params();
        loader = std::thread([this] {
            std::unique_lock<std::mutex> lk(loaderLock);
            while (!stop) {
                loaderCv.wait_for(lk, std::chrono::milliseconds(40));
                if (stop) break;
                lk.unlock();
                for (int m = 0; m < zd::DfnRunner::kModels; ++m)
                    if (dfn.loadWanted(m) && !dfn.isLoaded(m)) dfn.load(m);
                lk.lock();
            }
        });
    }
    ~shz_engine()
    {
        { std::lock_guard<std::mutex> lk(loaderLock); stop = true; }
        loaderCv.notify_all();
        if (loader.joinable()) loader.join();
    }

    void prepare(double sr, int channels, int block)
    {
        rate = sr;
        nch = std::max(1, std::min(8, channels));
        maxBlock = std::max(64, block);
        denoiser.prepare(sr, nch);
        dfn.prepare(sr, nch);
        inCopy.setSize(nch, maxBlock); pathA.setSize(nch, maxBlock); pathB.setSize(nch, maxBlock); pathC.setSize(nch, maxBlock);
        path = pathOfMode(mode.load()); prevPath = -1; fadeLeft = 0;
        fadeLen = std::max(1, (int) std::lround(sr * 0.02));
        if (path > 0) dfn.load(path - 1);
        const double binHz = sr / denoiser.getFFTSize();
        for (int b = 0; b <= kMeterBands; ++b) {
            const double f = 40.0 * std::pow((sr * 0.5) / 40.0, (double) b / kMeterBands);
            edges[(size_t) b] = std::max(1, std::min(denoiser.getNumBins(), (int) std::lround(f / binHz)));
            dfnEdges[(size_t) b] = std::max(1, std::min(480, (int) std::lround(std::min(f, 24000.0) / dfn.binHz())));
        }
        for (auto& m : meter) m.store(0.0f);
        outGain = std::pow(10.0f, cur.output_db / 20.0f);
        applyMode();
    }

    void applyMode()
    {
        const int m = std::max(0, std::min(kNumModes - 1, mode.load()));
        const int pth = pathOfMode(m);
        int la;
        if (pth > 0) {
            la = zd::DfnRunner::latencyFor(pth - 1, rate);
        } else {
            la = kModeLookahead[m] * denoiser.getScale();
            if (la != denoiser.getLookahead()) denoiser.setLookahead(la);
        }
        latency.store(la);
    }

    void runPath(int which, Buf& io, int n, const zd::DfnParams& dp)
    {
        if (which == 0) denoiser.process(io.data(), nch, n);
        else dfn.process(which - 1, io.data(), nch, n, dp);
    }

    void processChunk(float* const* ch, int n)
    {
        if (parDirty.load(std::memory_order_acquire) && parLock.try_lock()) {
            cur = pending;
            parDirty.store(false);
            parLock.unlock();
        }
        applyMode();
        const int m = std::max(0, std::min(kNumModes - 1, mode.load()));
        const int newPath = pathOfMode(m);
        if (newPath != path) { prevPath = path; path = newPath; fadeLeft = fadeLen; }

        zd::DenoiserParams p = denoiser.getParams();
        p.amount = cur.amount * 0.01f;
        p.learn = cur.learn != 0;
        p.adaptive = cur.adaptive != 0;
        p.aiStrongVoice = cur.voice_protect != 0;
        p.reductionDb = cur.reduction_db;
        p.gateDb = cur.gate_db;
        p.pauseCleanup = cur.pause_cleanup != 0;
        p.hissCut = cur.hiss_cut * 0.01f;
        p.maxJumpDb = kStrengthJumpDb[std::max(0, std::min(2, cur.strength))];
        p.ai = m == 3;
        denoiser.setParams(p);

        zd::DfnParams dp;
        dp.attenDb = p.reductionDb >= 79.5f ? 100.0f : p.reductionDb * std::min(1.0f, p.amount * 2.0f);
        dp.pfBeta = p.amount > 0.5f ? 0.04f * (p.amount - 0.5f) * 2.0f : 0.0f;
        dp.guard = p.aiStrongVoice ? 2.0f : 1.0f;

        const bool fading = fadeLeft > 0 && prevPath >= 0;
        for (int c = 0; c < nch; ++c) std::copy(ch[c], ch[c] + n, inCopy.ch[(size_t) c].data());
        for (int c = 0; c < nch; ++c) std::copy(ch[c], ch[c] + n, pathA.ch[(size_t) c].data());
        runPath(0, pathA, n, dp); // always runs: switching back to modes 0-3 continues seamlessly
        for (int k = 0; k < zd::DfnRunner::kModels; ++k) {
            const bool used = path == k + 1 || (fading && prevPath == k + 1);
            if (!used) dfn.idle(k, inCopy.data(), nch, n);
        }
        auto outputOf = [&](int which, Buf& dst) {
            if (which == 0) { if (&dst != &pathA) for (int c = 0; c < nch; ++c) std::copy(pathA.ch[(size_t) c].begin(), pathA.ch[(size_t) c].begin() + n, dst.ch[(size_t) c].begin()); return; }
            for (int c = 0; c < nch; ++c) std::copy(inCopy.ch[(size_t) c].begin(), inCopy.ch[(size_t) c].begin() + n, dst.ch[(size_t) c].begin());
            runPath(which, dst, n, dp);
        };
        Buf* out = &pathA;
        if (path != 0) { outputOf(path, pathB); out = &pathB; }
        if (fading) {
            Buf* oldp = &pathA; // old path = denoiser: already computed
            if (prevPath != 0) { oldp = (out == &pathB) ? &pathC : &pathB; outputOf(prevPath, *oldp); }
            Buf& old = *oldp;
            const int k = std::min(n, fadeLeft);
            for (int i = 0; i < k; ++i) {
                const float g = (float) (fadeLeft - i) / (float) fadeLen;
                for (int c = 0; c < nch; ++c) {
                    float& o = out->ch[(size_t) c][(size_t) i];
                    o = o + g * (old.ch[(size_t) c][(size_t) i] - o);
                }
            }
            fadeLeft -= k;
            if (fadeLeft <= 0) prevPath = -1;
        }
        // output gain, smoothed per block (one-pole ~20 ms)
        const float target = std::pow(10.0f, std::max(-12.0f, std::min(12.0f, cur.output_db)) / 20.0f);
        const float a = 1.0f - std::exp(-1.0f / (float) (rate * 0.02));
        for (int i = 0; i < n; ++i) {
            outGain += a * (target - outGain);
            for (int c = 0; c < nch; ++c) ch[c][i] = out->ch[(size_t) c][(size_t) i] * outGain;
        }
        loading.store(path > 0 && !dfn.isWet(path - 1));
        updateMeter();
    }

    void updateMeter()
    {
        const bool d = path > 0;
        const auto& g = d ? dfn.gains() : denoiser.currentGain();
        const auto& e = d ? dfnEdges : edges;
        for (int b = 0; b < kMeterBands; ++b) {
            const int lo = e[(size_t) b], hi = std::max(lo + 1, e[(size_t) b + 1]);
            float s = 0.0f;
            for (int k = lo; k < hi && k < (int) g.size(); ++k) s += g[(size_t) k] * g[(size_t) k];
            const float v = std::sqrt(s / (float) (hi - lo));
            meter[(size_t) b].store(std::max(-60.0f, 20.0f * std::log10(std::max(v, 1e-3f))), std::memory_order_relaxed);
        }
    }
};

extern "C" {

shz_params shz_default_params(void)
{
    shz_params p;
    p.amount = 50.0f; p.reduction_db = 40.0f; p.output_db = 0.0f; p.hiss_cut = 0.0f;
    p.gate_db = zd::DenoiserParams::kGateOff; p.strength = SHZ_MEDIUM; p.learn = 0; p.adaptive = 1;
    p.voice_protect = 0; p.pause_cleanup = 0;
    return p;
}

shz_engine* shz_create(double sample_rate, int channels, int max_block)
{
    auto* e = new shz_engine();
    e->prepare(sample_rate, channels, max_block);
    return e;
}

void shz_destroy(shz_engine* e) { delete e; }
void shz_prepare(shz_engine* e, double sr, int channels, int max_block) { if (e) e->prepare(sr, channels, max_block); }
void shz_reset(shz_engine* e) { if (e) e->prepare(e->rate, e->nch, e->maxBlock); }

void shz_set_mode(shz_engine* e, int mode) { if (e) e->mode.store(std::max(0, std::min(kNumModes - 1, mode))); }
int shz_get_mode(const shz_engine* e) { return e ? e->mode.load() : 0; }
int shz_preload(shz_engine* e, int mode)
{
    if (!e) return 0;
    const int p = pathOfMode(mode);
    return p == 0 ? 1 : (e->dfn.load(p - 1) ? 1 : 0);
}

void shz_set_params(shz_engine* e, const shz_params* p)
{
    if (!e || !p) return;
    std::lock_guard<std::mutex> lk(e->parLock);
    e->pending = *p;
    e->parDirty.store(true, std::memory_order_release);
}

int shz_get_latency(const shz_engine* e)
{
    if (!e) return 0;
    const int m = e->mode.load();
    const int p = pathOfMode(m);
    return p > 0 ? zd::DfnRunner::latencyFor(p - 1, e->rate) : kModeLookahead[m] * const_cast<shz_engine*>(e)->denoiser.getScale();
}

int shz_latency_for(int mode, double sr)
{
    mode = std::max(0, std::min(kNumModes - 1, mode));
    const int p = pathOfMode(mode);
    if (p > 0) return zd::DfnRunner::latencyFor(p - 1, sr);
    const int scale = sr > 100000.0 ? 4 : sr > 50000.0 ? 2 : 1;
    return kModeLookahead[mode] * scale;
}

int shz_is_loading(const shz_engine* e) { return e && e->loading.load() ? 1 : 0; }

void shz_process(shz_engine* e, float* const* channels, int n)
{
    if (!e || !channels || n <= 0) return;
    float* tmp[8];
    for (int done = 0; done < n;) {
        const int k = std::min(n - done, e->maxBlock);
        for (int c = 0; c < e->nch; ++c) tmp[c] = channels[c] + done;
        e->processChunk(tmp, k);
        done += k;
    }
}

void shz_get_meter(const shz_engine* e, float* out, int bands)
{
    if (!e || !out) return;
    for (int b = 0; b < bands; ++b) {
        const int src = std::min(kMeterBands - 1, b * kMeterBands / std::max(1, bands));
        out[b] = e->meter[(size_t) src].load(std::memory_order_relaxed);
    }
}

} // extern "C"
