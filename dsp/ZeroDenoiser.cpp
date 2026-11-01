#include "ZeroDenoiser.h"
#include <algorithm>
#include <cmath>

namespace zd {

namespace {
inline float hannAt(int i, int n) { return 0.5f - 0.5f * std::cos(2.0f * (float) M_PI * (i + 0.5f) / n); }

inline float dot(const float* a, const float* b, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 += a[i] * b[i];         s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3];
        s4 += a[i + 4] * b[i + 4]; s5 += a[i + 5] * b[i + 5];
        s6 += a[i + 6] * b[i + 6]; s7 += a[i + 7] * b[i + 7];
    }
    for (; i < n; ++i) s0 += a[i] * b[i];
    return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
}
} // namespace

void ZeroDenoiser::prepare(double sampleRate, int numChannels)
{
    fs = sampleRate;
    nCh = numChannels;
    // Keep time/frequency resolution constant across sample rates: all sizes
    // are defined at 44.1/48 kHz and scaled by a power of two.
    scale = sampleRate > 100000.0 ? 4 : (sampleRate > 50000.0 ? 2 : 1);
    nFFT = kBaseFFT * scale;
    nBins = nFFT / 2 + 1;
    hop = kBaseHop * scale;
    nFast = kBaseFast * scale;
    nTaps = kBaseTaps * scale;
    maxLookahead = kBaseMaxLookahead * scale;
    fftLong.prepare(nFFT);
    fftFast.prepare(nFast);
    fftMp.prepare(nFFT);

    winLong.resize(nFFT);
    for (int i = 0; i < nFFT; ++i) winLong[i] = hannAt(i, nFFT);
    winFast.resize(nFast);
    for (int i = 0; i < nFast; ++i) winFast[i] = hannAt(i, nFast);
    fade.resize(hop);
    for (int i = 0; i < hop; ++i) fade[i] = 0.5f - 0.5f * std::cos((float) M_PI * (i + 0.5f) / hop);

    // ~1/3-octave bands on the fast spectrum (bins 1..nFast/2)
    bandEdges.clear();
    const int nbs = nFast / 2 + 1;
    bandEdges.push_back(1);
    while (bandEdges.back() < nbs - 1) {
        int w = std::max(2, (int) (bandEdges.back() * 0.26));
        bandEdges.push_back(std::min(nbs - 1, bandEdges.back() + w));
    }

    {
        const int lo = (int) (100.0 / sampleRate * nFast), hi = (int) (4000.0 / sampleRate * nFast);
        fastVadLo = (int) (std::lower_bound(bandEdges.begin(), bandEdges.end(), lo) - bandEdges.begin());
        fastVadHi = (int) (std::lower_bound(bandEdges.begin(), bandEdges.end(), hi) - bandEdges.begin());
        hfStartBin = (int) (7000.0 / sampleRate * nFFT);
    }

    // Time constants are defined in seconds so they don't depend on hop/fs.
    // Values are tuned at 48 kHz with the fixed FFT sizes above.
    const double frameT = hop / fs;
    aNoise = std::exp(-frameT / 0.08);
    aP = std::exp(-frameT / 0.05);
    aDD = std::exp(-frameT / 0.1);
    aFastRel = std::exp(-frameT / 0.03);
    aPs = std::exp(-frameT / 0.004);
    aRel = std::exp(-frameT / 0.02);
    aVadRel = std::exp(-frameT / 0.15);
    aLearn = std::exp(-frameT / 2.0); // learning = running mean, then ~2 s memory
    warmFrames = (int) (0.1 * fs / hop);

    frame.assign(nFFT, 0.0f);
    tmp.assign(nFFT, 0.0f);
    spec.assign(nBins, {});
    specFast.assign(nFast / 2 + 1, {});
    specMp.assign(nBins, {});
    gain.assign(nBins, 1.0f);
    gainOut.assign(nBins, 1.0f);
    hTmp.assign(nTaps, 0.0f);
    graw.assign(nBins, 0.0);
    ps.assign(nFast / 2 + 1, 0.0);
    ai.prepare(scale); // network runs at fs/scale (44.1/48 kHz)
    reset();
}

void ZeroDenoiser::reset()
{
    anaBuf.assign(2 * nFFT, 0.0f);
    anaPos = 0;
    hist.assign(nCh, std::vector<float>(2 * nTaps, 0.0f));
    histPos = 0;
    hopCount = 0;
    delay.assign(nCh, std::vector<float>(maxLookahead + 1, 0.0f));
    delayPos = 0;
    curRev.assign(nTaps, 0.0f);
    curRev[nTaps - 1] = 1.0f; // unity (reversed)
    nextRev = curRev;
    noise.assign(nBins, 0.0);
    pbar.assign(nBins, 0.0);
    prevS.assign(nBins, 0.0);
    gPrev.assign(nBins, 1.0);
    gfPrev.assign(nBins, 0.0);
    psm.assign(nBins, 0.0);
    gRel.assign(nBins, 1.0);
    vad = 0.0;
    fastVad = 0.0;
    learnFrames = 0;
    fnoise.assign(bandEdges.size() - 1, 0.0);
    frames = 0;
    ai.reset();
    aiNew = false;
    aiActive = params.ai;
}

void ZeroDenoiser::setLookahead(int samples)
{
    samples = std::clamp(samples, 0, maxLookahead);
    if (samples == lookahead) return;
    lookahead = samples;
    for (auto& d : delay) std::fill(d.begin(), d.end(), 0.0f);
}

void ZeroDenoiser::setParams(const DenoiserParams& p)
{
    if (p.learn && !params.learn) learnFrames = 0; // new learn pass starts fresh
    params = p;
    if (params.ai != aiActive) { // mode switch: restart the network cleanly
        aiActive = params.ai;
        ai.reset();
        aiNew = false;
    }
}

void ZeroDenoiser::process(float* const* ch, int numChannels, int numSamples)
{
    const int nc = std::min(numChannels, nCh);
    const float invC = 1.0f / (float) std::max(1, nc);
    for (int n = 0; n < numSamples; ++n) {
        float mono = 0.0f;
        for (int c = 0; c < nc; ++c) {
            const float x = ch[c][n];
            mono += x;
            float xd = x;
            if (lookahead > 0 && c < (int) delay.size()) {
                auto& d = delay[c];
                const int size = (int) d.size();
                d[delayPos] = x;
                int r = delayPos - lookahead;
                if (r < 0) r += size;
                xd = d[r];
            }
            hist[c][histPos] = xd;
            hist[c][histPos + nTaps] = xd;
        }
        if (lookahead > 0 && !delay.empty()) delayPos = (delayPos + 1) % (int) delay[0].size();
        histPos = (histPos + 1) % nTaps;
        anaBuf[anaPos] = mono * invC;
        anaBuf[anaPos + nFFT] = mono * invC;
        anaPos = (anaPos + 1) % nFFT;
        if (aiActive && ai.push(mono * invC)) aiNew = true;

        const float f = fade[hopCount];
        for (int c = 0; c < nc; ++c) {
            const float* w = hist[c].data() + histPos; // oldest..newest
            const float yOld = dot(w, curRev.data(), nTaps);
            const float yNew = dot(w, nextRev.data(), nTaps);
            ch[c][n] = yOld + f * (yNew - yOld);
        }

        if (++hopCount == hop) {
            hopCount = 0;
            if (aiActive) analyseAi(); else analyse();
        }
    }
}

void ZeroDenoiser::analyse()
{
    // ---- long analysis -------------------------------------------------
    const float* x = anaBuf.data() + anaPos; // latest nFFT samples, oldest first
    for (int i = 0; i < nFFT; ++i) frame[i] = x[i] * winLong[i];
    fftLong.forwardReal(frame.data(), spec.data());

    const double xiH1 = std::pow(10.0, 1.5);
    const double xiFac = xiH1 / (1.0 + xiH1);
    const bool first = (frames == 0);
    const double a = frames > warmFrames ? aNoise : 0.5;
    const bool warming = frames <= warmFrames;
    // Learn: average the input as pure noise. Freeze: keep the profile fixed.
    const bool learning = params.learn;
    const bool frozen = !params.adaptive && !learning && !warming;
    if (learning) ++learnFrames;
    const double aL = std::max(1.0 - 1.0 / (double) std::max<long>(1, learnFrames), aLearn);

    const double amt = std::clamp((double) params.amount, 0.0, 1.0);
    const double over = 1.0 + 1.5 * amt;
    // Up to 55 dB of attenuation where there is no speech.
    const double gmin = std::pow(10.0, -(amt * 55.0) / 20.0);

    for (int k = 0; k < nBins; ++k) {
        const double Praw = (double) std::norm(spec[k]);
        if (first) psm[k] = Praw;
        psm[k] = aPs * psm[k] + (1.0 - aPs) * Praw; // ~4 ms periodogram smoothing
        const double P = psm[k];
        if (first) noise[k] = P + 1e-12;
        const double snrPost = P / noise[k];
        double spp = 1.0 / (1.0 + (1.0 + xiH1) * std::exp(-snrPost * xiFac));
        pbar[k] = aP * pbar[k] + (1.0 - aP) * spp;
        if (pbar[k] > 0.99) spp = std::min(spp, 0.99);
        const double nEst = (1.0 - spp) * P + spp * noise[k];
        if (learning) noise[k] = aL * noise[k] + (1.0 - aL) * P;
        else if (!frozen) noise[k] = a * noise[k] + (1.0 - a) * nEst;

        const double nz = noise[k] * over;
        const double gamma = P / nz;
        double xi = aDD * prevS[k] / nz + (1.0 - aDD) * std::max(gamma - 1.0, 0.0);
        xi = std::max(xi, 1e-4);
        graw[k] = xi / (1.0 + xi);
    }
    ++frames;

    // Frame-level speech presence (share of 100 Hz-4 kHz bins with a-priori
    // SNR > 0 dB). While speech is present the gain curve is softened
    // (g^0.5) so the voice keeps its natural timbre; residual noise under the
    // voice is largely masked. Between words the full Wiener gain applies.
    {
        const int k0 = (int) (100.0 / fs * nFFT), k1 = (int) (4000.0 / fs * nFFT);
        int cnt = 0;
        for (int k = k0; k < k1; ++k) cnt += graw[k] > 0.5 ? 1 : 0; // xi > 1
        const double frac = (double) cnt / (double) std::max(1, k1 - k0);
        const double v = std::max(std::clamp((frac - 0.05) / 0.15, 0.0, 1.0), fastVad); // fast path: previous hop
        vad = v > vad ? v : aVadRel * vad + (1.0 - aVadRel) * v;
        const double ge = 1.0 * (1.0 - vad) + 0.5 * vad;
        if (ge != 1.0)
            for (int k = 0; k < nBins; ++k) graw[k] = (double) std::exp((float) ge * std::log((float) std::max(graw[k], 1e-12)));
    }

    // 3-tap spectral smoothing (zero-padded edges), then prevS update
    for (int k = 0; k < nBins; ++k) {
        const double l = k > 0 ? graw[k - 1] : 0.0;
        const double r = k + 1 < nBins ? graw[k + 1] : 0.0;
        double gs = 0.25 * l + 0.5 * graw[k] + 0.25 * r;
        gs = std::max(gs, aRel * gRel[k] + (1.0 - aRel) * gs); // instant attack, 20 ms release
        gRel[k] = gs;
        gPrev[k] = gs;
        prevS[k] = gs * gs * psm[k];
    }

    // ---- fast onset path ----------------------------------------------
    if (params.onsetPath) {
        const float* xf = x + (nFFT - nFast);
        for (int i = 0; i < nFast; ++i) tmp[i] = xf[i] * winFast[i];
        fftFast.forwardReal(tmp.data(), specFast.data());
        const int nbs = nFast / 2 + 1;
        for (int k = 0; k < nbs; ++k) ps[k] = (double) std::norm(specFast[k]);
        const int nb = (int) bandEdges.size() - 1;
        int fastActive = 0;
        const int m = nFFT / nFast;
        for (int b = 0; b < nb; ++b) {
            const int lo = bandEdges[b];
            const int hi = (b == nb - 1) ? nbs : bandEdges[b + 1];
            double Pb = 0.0;
            for (int k = lo; k < hi; ++k) Pb += ps[k];
            if (learning) {
                fnoise[b] = learnFrames == 1 ? Pb : aL * fnoise[b] + (1.0 - aL) * Pb;
            } else if (frozen) {
                // keep learned band noise
            } else if (frames <= warmFrames + 1) {
                fnoise[b] = (frames == 1) ? Pb : 0.5 * fnoise[b] + 0.5 * Pb;
            } else {
                const double sp = 1.0 / (1.0 + (1.0 + xiH1) * std::exp(-(Pb / fnoise[b]) * xiFac));
                fnoise[b] = aNoise * fnoise[b] + (1.0 - aNoise) * ((1.0 - sp) * Pb + sp * fnoise[b]);
            }
            const double K = bandEdges[b + 1] - bandEdges[b];
            const double snr = Pb / (fnoise[b] * over);
            const double T = 1.0 + params.fastThreshold / std::sqrt(K);
            const double gb = std::sqrt(std::clamp((snr - T) / (snr + 1e-12), 0.0, 1.0));
            if (b >= fastVadLo && b < fastVadHi) fastActive += gb > 0.3 ? 1 : 0;
            // expand band -> long bins (each fast bin covers m long bins)
            const int kLo = lo * m;
            const int kHi = (b == nb - 1) ? nBins : hi * m;
            for (int k = kLo; k < kHi; ++k) {
                const double gf = std::max(gb, aFastRel * gfPrev[k]);
                gfPrev[k] = gf;
            }
        }
        const int nv = std::max(1, fastVadHi - fastVadLo);
        fastVad = std::clamp(((double) fastActive / nv - 0.1) / 0.3, 0.0, 1.0);
        for (int k = 0; k < bandEdges[0] * m; ++k) gfPrev[k] = aFastRel * gfPrev[k];
        for (int k = 0; k < nBins; ++k) gPrev[k] = std::max(gPrev[k], gfPrev[k]);
    }

    for (int k = 0; k < nBins; ++k) {
        double gm = gmin;
        if (k >= hfStartBin) // keep some 'air' (sibilance, breath) while speech is present
            gm = std::exp((1.0 - vad) * std::log(gmin) + vad * std::log(std::max(gmin, kHfFloor)));
        gain[k] = (float) (gm + (1.0 - gm) * gPrev[k]);
        gainOut[k] = gain[k];
    }

    // ---- new filter ----------------------------------------------------
    designMinPhase(gain.data(), hTmp.data());
    curRev.swap(nextRev);
    for (int i = 0; i < nTaps; ++i) nextRev[i] = hTmp[nTaps - 1 - i];
    // crossfade starts from the filter that just finished fading in
    // (curRev now holds the previous 'next' filter)
}

void ZeroDenoiser::analyseAi()
{
    if (!aiNew) {
        curRev = nextRev; // finish the previous crossfade, hold the filter
        return;
    }
    aiNew = false;
    // Network bins: 481 over a 960-pt FFT at fs/scale. Ours: nFFT = 1024*scale
    // at fs, i.e. the same Hz spacing family, so bin k maps to k * 960/1024.
    const double amt = std::clamp((double) params.amount, 0.0, 1.0);
    const float gmin = (float) std::pow(10.0, -(amt * 55.0) / 20.0);
    const float* r = ai.gains();
    for (int k = 0; k < nBins; ++k) {
        const double pos = std::min((double) k * 960.0 / 1024.0, (double) (AiGains::kBins - 1));
        const int j = std::min((int) pos, AiGains::kBins - 2);
        const float t = (float) (pos - j);
        const float gr = std::clamp(r[j] + t * (r[j + 1] - r[j]), 0.0f, 1.0f);
        gain[k] = gmin + (1.0f - gmin) * gr;
        gainOut[k] = gain[k];
    }
    designMinPhase(gain.data(), hTmp.data());
    curRev.swap(nextRev);
    for (int i = 0; i < nTaps; ++i) nextRev[i] = hTmp[nTaps - 1 - i];
}

void ZeroDenoiser::designMinPhase(const float* g, float* hOut)
{
    // real cepstrum of log magnitude
    for (int k = 0; k < nBins; ++k) specMp[k] = FFT::cpx(std::log(std::max(g[k], 1e-5f)), 0.0f);
    fftMp.inverseReal(specMp.data(), tmp.data());
    // fold to causal
    const int N = nFFT;
    frame[0] = tmp[0];
    for (int i = 1; i < N / 2; ++i) frame[i] = 2.0f * tmp[i];
    frame[N / 2] = tmp[N / 2];
    for (int i = N / 2 + 1; i < N; ++i) frame[i] = 0.0f;
    fftMp.forwardReal(frame.data(), specMp.data());
    for (int k = 0; k < nBins; ++k) {
        const float m = std::exp(specMp[k].real()), ph = specMp[k].imag();
        specMp[k] = FFT::cpx(m * std::cos(ph), m * std::sin(ph));
    }
    fftMp.inverseReal(specMp.data(), tmp.data());
    const int tail = nTaps / 4;
    for (int i = 0; i < nTaps; ++i) hOut[i] = tmp[i];
    for (int i = 0; i < tail; ++i) {
        const float c = std::cos(0.5f * (float) M_PI * (i + 0.5f) / tail);
        hOut[nTaps - tail + i] *= c * c;
    }
}

} // namespace zd
