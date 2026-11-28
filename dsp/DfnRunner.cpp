#include "DfnRunner.h"
#include <algorithm>
#include <cmath>

#if ZD_HAS_DFN
#include "dfbridge.h"
#endif

namespace zd {

#if ZD_HAS_DFN
bool DfnRunner::available() { return true; }
int DfnRunner::latencyFor(int model, double sampleRate) { return dfb_latency_for(model, sampleRate); }
#else
bool DfnRunner::available() { return false; }
int DfnRunner::latencyFor(int model, double sampleRate)
{
    const double ms = model == 1 ? 40.0 : 20.0;
    return (int) std::lround(ms * 0.001 * sampleRate);
}
#endif

DfnRunner::~DfnRunner() { freeAll(); }

void DfnRunner::freeAll()
{
    for (auto& e : eng) {
        Dfb* d = e.exchange(nullptr);
#if ZD_HAS_DFN
        if (d != nullptr) dfb_free(d);
#else
        (void) d;
#endif
    }
}

void DfnRunner::prepare(double sampleRate, int numChannels)
{
    std::lock_guard<std::mutex> lk(cfgLock);
    numChannels = std::max(1, std::min(8, numChannels));
    if (sampleRate != sr || numChannels != nch) {
        freeAll();
        for (auto& f : failed) f.store(false);
    }
    sr = sampleRate;
    nch = numChannels;
    for (int m = 0; m < kModels; ++m) {
        Slot& s = slot[(size_t) m];
        s.latency = latencyFor(m, sr);
        s.delay.assign((size_t) nch, std::vector<float>((size_t) std::max(1, s.latency), 0.0f));
        s.pos = 0;
        s.fed = 0;
        s.ramp = 0.0f;
        s.needReset = true;
    }
    inBuf.assign((size_t) nch, std::vector<float>(8192, 0.0f));
    wetBuf.assign((size_t) nch, std::vector<float>(8192, 0.0f));
    gainBuf.assign(481, 1.0f);
}

bool DfnRunner::load(int model)
{
#if ZD_HAS_DFN
    std::lock_guard<std::mutex> lk(cfgLock);
    if (eng[(size_t) model].load() != nullptr) return true;
    if (failed[(size_t) model].load()) return false;
    Dfb* d = dfb_create(model, nch, sr);
    if (d == nullptr || dfb_latency(d) != slot[(size_t) model].latency) {
        if (d != nullptr) dfb_free(d);
        failed[(size_t) model].store(true);
        want[(size_t) model].store(false);
        return false;
    }
    eng[(size_t) model].store(d, std::memory_order_release);
    want[(size_t) model].store(false);
    return true;
#else
    (void) model;
    return false;
#endif
}

void DfnRunner::idle(int model, const float* const* in, int numChannels, int numSamples)
{
    // keep the dry delay line running, so a later switch into this mode starts with the
    // correctly delayed input instead of a gap
    Slot& s = slot[(size_t) model];
    s.fed = 0;
    s.ramp = 0.0f;
    s.needReset = true;
    const int L = s.latency;
    if (L <= 0) return;
    const int nc = std::min(numChannels, nch);
    for (int c = 0; c < nch; ++c) {
        float* d = s.delay[(size_t) c].data();
        int pos = s.pos;
        for (int i = 0; i < numSamples; ++i) {
            const float x = c < nc ? in[c][i] : 0.0f;
            d[pos] = std::isfinite(x) ? std::max(-8.0f, std::min(8.0f, x)) : 0.0f;
            if (++pos >= L) pos = 0;
        }
    }
    s.pos = (int) ((s.pos + (long) numSamples) % L);
}

void DfnRunner::process(int model, float* const* io, int numChannels, int numSamples, const DfnParams& p)
{
    Slot& s = slot[(size_t) model];
    const int nc = std::min(numChannels, nch);
    for (int done = 0; done < numSamples;) {
        const int n = std::min(numSamples - done, (int) inBuf[0].size());
        // sanitise (NaN/Inf from the host would poison the network state)
        for (int c = 0; c < nch; ++c) {
            float* in = inBuf[(size_t) c].data();
            if (c < nc) {
                const float* x = io[c] + done;
                for (int i = 0; i < n; ++i) in[i] = std::isfinite(x[i]) ? std::max(-8.0f, std::min(8.0f, x[i])) : 0.0f;
            } else {
                std::fill(in, in + n, 0.0f);
            }
        }
        Dfb* d = eng[(size_t) model].load(std::memory_order_acquire);
        bool wet = false;
#if ZD_HAS_DFN
        if (d != nullptr) {
            if (s.needReset) {
                dfb_reset(d);
                s.needReset = false;
                s.fed = 0;
                s.ramp = 0.0f;
            }
            dfb_set_params(d, p.attenDb, p.guard, p.pfBeta);
            const float* ins[8];
            float* outs[8];
            for (int c = 0; c < nch; ++c) { ins[c] = inBuf[(size_t) c].data(); outs[c] = wetBuf[(size_t) c].data(); }
            dfb_process(d, ins, outs, n);
            wet = dfb_is_broken(d) == 0;
            if (wet) dfb_gains(d, gainBuf.data(), (int) gainBuf.size());
        } else if (!failed[(size_t) model].load(std::memory_order_relaxed)) {
            want[(size_t) model].store(true, std::memory_order_relaxed);
        }
#else
        (void) d;
#endif
        // the network's first output (after a reset) is its prefill silence plus a few frames
        // of settling: start the crossfade once that has passed
        const long warm = (long) s.latency + (long) std::lround(sr * 0.05);
        const float step = 1.0f / (float) std::max(1.0, sr * 0.02); // 20 ms crossfade
        const int L = s.latency;
        for (int i = 0; i < n; ++i) {
            float g = s.ramp;
            if (wet) {
                if (s.fed >= warm) s.ramp = std::min(1.0f, s.ramp + step);
                ++s.fed;
            } else {
                s.ramp = std::max(0.0f, s.ramp - step);
            }
            for (int c = 0; c < nc; ++c) {
                float dry;
                if (L > 0) {
                    float& slotv = s.delay[(size_t) c][(size_t) s.pos];
                    dry = slotv;
                    slotv = inBuf[(size_t) c][(size_t) i];
                } else {
                    dry = inBuf[(size_t) c][(size_t) i];
                }
                const float w = wet ? wetBuf[(size_t) c][(size_t) i] : 0.0f;
                io[c][done + i] = dry + g * (w - dry);
            }
            if (L > 0 && ++s.pos >= L) s.pos = 0;
        }
        if (!wet) std::fill(gainBuf.begin(), gainBuf.end(), 1.0f);
        done += n;
    }
}

} // namespace zd
