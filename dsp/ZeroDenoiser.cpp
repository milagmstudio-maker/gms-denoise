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
    fftAc.prepare(2 * nFFT);
    acBuf.assign(2 * nFFT, 0.0f);
    acSpec.assign(nFFT + 1, {});
    lagMin = (int) std::floor(fs / 1600.0);
    lagMax = (int) std::ceil(fs / 100.0);
    acKLo = (int) (300.0 / fs * 2 * nFFT); // above car/engine rumble; voices have harmonics here
    acKHi = (int) (5000.0 / fs * 2 * nFFT);

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
    aSlow = std::exp(-frameT / 0.03);
    aTail = std::exp(-frameT / 0.15);
    aVoiceRef = 1.0 - std::exp(-frameT / 1.5);
    gateAtkC = std::exp(-1.0 / (0.0005 * fs));
    gateRelC = std::exp(-1.0 / (0.010 * fs));
    gateOpenC = std::exp(-1.0 / (0.0007 * fs));
    gateCloseC = std::pow(1e-6, 1.0 / (0.040 * fs));                   // reaches -120 dB in 40 ms
    gateHoldLen = (long) (0.080 * fs);
    floorSubLen = (long) (0.5 / frameT);
    aVoiceRel = std::exp(-frameT / 0.015); // short release: noise must not leak after a word ends
    maxVoiceHoldHops = (long) (2.0 / frameT);
    // window autocorrelation, to undo the Hann taper's bias on long lags
    winAc.assign(nFFT, 0.0f);
    {
        double w0 = 0.0;
        for (int i = 0; i < nFFT; ++i) w0 += (double) winLong[i] * winLong[i];
        for (int t = 0; t <= lagMax && t < nFFT; ++t) {
            double a = 0.0;
            for (int i = 0; i + t < nFFT; ++i) a += (double) winLong[i] * winLong[i + t];
            winAc[t] = (float) std::max(a / w0, 0.25);
        }
    }
    aVadRel = std::exp(-frameT / 0.15);
    aLearn = std::exp(-frameT / 2.0); // learning = running mean, then ~2 s memory
    warmFrames = (int) (0.1 * fs / hop);
    settleFrames = warmFrames + (long) (1.0 * fs / hop);

    frame.assign(nFFT, 0.0f);
    tmp.assign(nFFT, 0.0f);
    spec.assign(nBins, {});
    specFast.assign(nFast / 2 + 1, {});
    specMp.assign(nBins, {});
    gain.assign(nBins, 1.0f);
    gainOut.assign(nBins, 1.0f);
    gAi.assign(nBins, 1.0f);
    gDsp.assign(nBins, 1.0f);
    gFloor.assign(nBins, 1.0f);
    voiceRefPow.assign(nBins, 0.1); // before any voice: assume ~-10 dB under a voice
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
    std::fill(gAi.begin(), gAi.end(), 1.0f);
    std::fill(gDsp.begin(), gDsp.end(), 1.0f);
    aiVad = 0.0;
    voiced = 0.0;
    holdBudget = (double) maxVoiceHoldHops;
    tailEnv = 0.0; voicedP = 0.0; onsetEnv = 0.0; combLag = 0; lastLag = 0;
    pSlow.assign(nBins, 0.0);
    subMin.assign(nBins, 1e300);
    floorN.assign(nBins, 0.0);
    floorRing.assign(kFloorSubs, std::vector<double>(nBins, 1e300));
    floorRingPos = 0; floorHop = 0; floorFilled = 0;
    pcEnv = 1.0; pcHoldCnt = 0;
    voiceRefPow.assign(nBins, 0.1);
    gateEnv = 0.0; gateGain = 1.0; gateOpen = true; gateHold = gateHoldLen; gateLevelDb = -120.0f;
    if (pendingLookahead >= 0) { lookahead = pendingLookahead; pendingLookahead = -1; }
    xfLen = 256 * scale; // ~5 ms
    // after audio has played, fade in: the output would otherwise jump from the denoised
    // signal to the untouched input (fresh filters) in one sample
    if (freshSinceReset) { xfState = 0; xfGain = 1.0f; } else { xfState = 2; xfGain = 0.0f; xfHold = lookahead; }
    freshSinceReset = true;
}

void ZeroDenoiser::setLookahead(int samples)
{
    samples = std::clamp(samples, 0, maxLookahead);
    if (freshSinceReset) { // nothing played yet: switch at once
        lookahead = samples; pendingLookahead = -1;
        for (auto& d : delay) std::fill(d.begin(), d.end(), 0.0f);
        if (xfState == 2) xfHold = lookahead;
        return;
    }
    if (samples == getLookahead()) return;
    if (samples == lookahead) { pendingLookahead = -1; if (xfState == 1) xfState = 2; return; } // switched back before it happened
    pendingLookahead = samples;
    xfState = 1; // fade out, then switch in process()
}

void ZeroDenoiser::switchLookaheadNow()
{
    lookahead = pendingLookahead; pendingLookahead = -1;
    for (auto& d : delay) std::fill(d.begin(), d.end(), 0.0f);
    for (auto& h : hist) std::fill(h.begin(), h.end(), 0.0f);
    xfState = 2;
    xfHold = lookahead; // the emptied delay line plays silence first: fade in when the audio arrives
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
    const bool gateOn = params.gateDb > DenoiserParams::kGateOff;
    const double thrOpen = std::pow(10.0, params.gateDb / 10.0);        // power
    const double thrClose = std::pow(10.0, (params.gateDb - 3.0) / 10.0); // 3 dB hysteresis
    if (numSamples > 0) freshSinceReset = false;
    const float xfStep = 1.0f / (float) xfLen;
    for (int n = 0; n < numSamples; ++n) {
        // a NaN / Inf from the host (or an upstream plug-in) would otherwise poison the
        // noise estimate for good: treat it as silence
        for (int c = 0; c < nc; ++c) if (!(std::fabs(ch[c][n]) <= 1.0e6f)) ch[c][n] = 0.0f;
        float mono = 0.0f;
        double pw = 0.0;
        if (!gateDetectOutput) for (int c = 0; c < nc; ++c) pw = std::max(pw, (double) ch[c][n] * ch[c][n]);
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
            if (gateDetectOutput) pw = std::max(pw, (double) ch[c][n] * ch[c][n]);
        }
        // gate detector: fast attack (~0.5 ms), ~10 ms release on the loudest channel
        gateEnv = pw > gateEnv ? gateAtkC * gateEnv + (1.0 - gateAtkC) * pw : gateRelC * gateEnv + (1.0 - gateRelC) * pw;
        if (gateOn) {
            if (gateEnv >= thrOpen) { gateOpen = true; gateHold = gateHoldLen; }
            else if (gateEnv < thrClose) { if (gateHold > 0) --gateHold; else gateOpen = false; }
            // open in ~1 ms, close over ~40 ms down to true silence
            gateGain = gateOpen ? gateOpenC * gateGain + (1.0 - gateOpenC)
                                : (gateGain * gateCloseC < 1e-6 ? 0.0 : gateGain * gateCloseC);
            for (int c = 0; c < nc; ++c) ch[c][n] *= (float) gateGain;
        } else {
            gateOpen = true; gateHold = gateHoldLen;
            if (gateGain < 1.0) { // gate switched off while closed: ramp up, don't jump
                gateGain = gateOpenC * gateGain + (1.0 - gateOpenC);
                if (gateGain > 0.9999) gateGain = 1.0;
                for (int c = 0; c < nc; ++c) ch[c][n] *= (float) gateGain;
            }
        }
        if (xfState != 0) {
            if (xfState == 1) { xfGain -= xfStep; if (xfGain <= 0.0f) { xfGain = 0.0f; switchLookaheadNow(); } }
            else if (xfHold > 0) --xfHold;
            else { xfGain += xfStep; if (xfGain >= 1.0f) { xfGain = 1.0f; xfState = 0; } }
            const float g = xfGain * xfGain * (3.0f - 2.0f * xfGain); // smoothstep
            for (int c = 0; c < nc; ++c) ch[c][n] *= g;
        }

        if (++hopCount == hop) {
            hopCount = 0;
            gateLevelDb = (float) (10.0 * std::log10(gateEnv + 1e-12));
            if (aiActive) analyseAi(); else analyse();
        }
    }
}

void ZeroDenoiser::analyse()
{
    computeDspGain(gain.data());
    applySteadiness(vad);
    for (int k = 0; k < nBins; ++k) gainOut[k] = gain[k];
    designMinPhase(gain.data(), hTmp.data());
    curRev.swap(nextRev);
    for (int i = 0; i < nTaps; ++i) nextRev[i] = hTmp[nTaps - 1 - i];
    // crossfade starts from the filter that just finished fading in
    // (curRev now holds the previous 'next' filter)
}

void ZeroDenoiser::computeDspGain(float* out)
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
    const bool fillFrames = (hissFix & 32) == 0 && (long) frames * hop < nFFT;
    // Learn: average the input as pure noise. Freeze: keep the profile fixed.
    const bool learning = params.learn;
    // AI mode voice guard: a sustained pitched voice (scream, held note) must not
    // be learned as noise. Hold the noise profile while it lasts, at most 3 s so a
    // tonal noise that starts later is still learned eventually.
    // Budgeted: holding spends 1 hop, not holding refills 1/3 hop (max 2 s of hold
    // per ~6 s), and nothing is held in the first second while the tracker settles.
    bool voiceHold = false;
    if (aiActive && params.aiVoiceGuard && !dbgNoHold && voiced > 0.5 && !learning && frames > settleFrames && holdBudget >= 1.0) {
        voiceHold = true;
        holdBudget -= 1.0;
    } else {
        holdBudget = std::min(holdBudget + 1.0 / 3.0, (double) maxVoiceHoldHops);
    }
    const bool frozen = (!params.adaptive && !learning && !warming) || voiceHold;
    if (learning) ++learnFrames;
    const double aL = std::max(1.0 - 1.0 / (double) std::max<long>(1, learnFrames), aLearn);

    const double amt = std::clamp((double) params.amount, 0.0, 1.0);
    const double over = 1.0 + 1.5 * amt;
    // Up to 55 dB of attenuation where there is no speech.
    const double gmin = floorGain();

    for (int k = 0; k < nBins; ++k) {
        const double Praw = (double) std::norm(spec[k]);
        if (first) psm[k] = Praw;
        psm[k] = aPs * psm[k] + (1.0 - aPs) * Praw; // ~4 ms periodogram smoothing
        const double P = psm[k];
        // Until the analysis buffer is full the frame is mostly zero padding: take the
        // level as the noise estimate directly (otherwise it starts near zero and the
        // speech-presence logic needs ~1-3 s to climb to the real noise: hiss at the start).
        if (first || fillFrames) noise[k] = P + 1e-12;
        else if (warming && !learning && !frozen && !(hissFix & 16)) noise[k] = 0.5 * noise[k] + 0.5 * P;
        const double snrPost = P / noise[k];
        double spp = 1.0 / (1.0 + (1.0 + xiH1) * std::exp(-snrPost * xiFac));
        pbar[k] = aP * pbar[k] + (1.0 - aP) * spp;
        if (pbar[k] > 0.99) spp = std::min(spp, 0.99);
        const double nEst = (1.0 - spp) * P + spp * noise[k];
        if (learning) noise[k] = aL * noise[k] + (1.0 - aL) * P;
        else if (!frozen && !(warming && !(hissFix & 16))) noise[k] = a * noise[k] + (1.0 - a) * nEst;

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
        if (ge != 1.0) {
            if (hissFix & 4) {
                // soften only bins that carry speech (a-priori SNR around/above 0 dB);
                // noise-only bins under the voice (hiss above the voice's range) keep the full gain
                for (int k = 0; k < nBins; ++k) {
                    const double s = std::clamp(graw[k] / 0.5, 0.0, 1.0); // graw = xi/(1+xi): 0.5 at xi = 1
                    const double e = 1.0 - (1.0 - ge) * s;
                    graw[k] = std::exp(e * std::log(std::max(graw[k], 1e-12)));
                }
            } else
                for (int k = 0; k < nBins; ++k) graw[k] = (double) std::exp((float) ge * std::log((float) std::max(graw[k], 1e-12)));
        }
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
        if (k >= hfStartBin && !(hissFix & 2)) { // keep some 'air' (sibilance, breath) while speech is present
            double v = vad;
            // ...only where there is HF signal above the noise (hiss-only bins are not kept)
            if (hissFix & 64) v *= std::clamp((psm[k] / std::max(noise[k], 1e-20) - 1.5) / 3.0, 0.0, 1.0);
            gm = std::exp((1.0 - v) * std::log(gmin) + v * std::log(std::max(gmin, kHfFloor)));
        }
        out[k] = (float) (gm + (1.0 - gm) * gPrev[k]);
    }
}

void ZeroDenoiser::updateFloor()
{
    // psm (4 ms smoothed periodogram) is updated by computeDspGain this hop.
    // Nothing is tracked during warm-up (the analysis buffer still holds the
    // initial zeros); the floor is valid once the first 0.5 s window completes.
    if (frames <= warmFrames) return;
    const bool first = floorHop == 0;
    for (int k = 0; k < nBins; ++k) {
        pSlow[k] = first ? psm[k] : aSlow * pSlow[k] + (1.0 - aSlow) * psm[k];
        subMin[k] = std::min(subMin[k], pSlow[k]);
    }
    ++floorHop;
    if (floorHop % floorSubLen != 0) return;
    if (floorFilled == 0) {
        for (auto& slot : floorRing) slot = subMin; // first window fills the whole ring
    } else {
        floorRing[(size_t) floorRingPos] = subMin;
    }
    floorRingPos = (floorRingPos + 1) % kFloorSubs;
    floorFilled = std::min(floorFilled + 1, kFloorSubs);
    std::fill(subMin.begin(), subMin.end(), 1e300);
    for (int k = 0; k < nBins; ++k) {
        double m = 1e300;
        for (int j = 0; j < kFloorSubs; ++j) m = std::min(m, floorRing[(size_t) j][(size_t) k]);
        floorN[k] = m;
    }
}

double ZeroDenoiser::computeVoicing()
{
    // Normalised autocorrelation of the part of the signal that stands above the
    // tracked (stationary) noise floor, 300 Hz-5 kHz. Pitched voice - including
    // screams and very high voices the network was not trained on - gives a
    // strong peak at the pitch period; hiss, hum, fans and clatter do not.
    if (dbgFloor && floorFilled == 0) { lastResid = 0.0; return 0.0; }
    const float* x = anaBuf.data() + anaPos;
    for (int i = 0; i < nFFT; ++i) acBuf[i] = x[i] * winLong[i];
    std::fill(acBuf.begin() + nFFT, acBuf.end(), 0.0f);
    fftAc.forwardReal(acBuf.data(), acSpec.data());
    double tot = 0.0, ntot = 1e-20;
    for (int k = 0; k <= nFFT; ++k) {
        float v = 0.0f;
        if (k >= acKLo && k < acKHi) {
            const double P = (double) std::norm(acSpec[k]);
            const size_t kk = (size_t) std::min(k / 2, nBins - 1);
            const double nz = dbgFloor ? 2.0 * floorBias * floorN[kk] : 2.0 * noise[kk];
            v = (float) std::max(P - nz, 0.0);
            tot += v;
            ntot += nz;
        }
        acSpec[k] = { v, 0.0f };
    }
    lastResid = tot / ntot; // above-noise power relative to 2x noise floor, 80 Hz-5 kHz
    if (tot <= 1e-12) return 0.0;
    fftAc.inverseReal(acSpec.data(), acBuf.data());
    const double r0 = acBuf[0];
    if (r0 <= 0.0) return 0.0;
    double best = 0.0;
    for (int t = lagMin; t <= lagMax; ++t) best = std::max(best, (double) acBuf[t] / (r0 * winAc[t]));
    // pitch period: the shortest local peak within 10 % of the best (avoids sub-harmonics)
    lastLag = 0;
    for (int t = lagMin + 1; t < lagMax; ++t) {
        const double a = (double) acBuf[t] / (r0 * winAc[t]);
        if (a >= 0.9 * best && acBuf[t] >= acBuf[t - 1] && acBuf[t] >= acBuf[t + 1]) { lastLag = t; break; }
    }
    return best;
}

void ZeroDenoiser::analyseAi()
{
    const bool assist = params.aiHissAssist, guard = params.aiVoiceGuard;
    // The DSP estimator keeps running in AI mode: it tracks stationary noise
    // (hiss, hum, fans) that the network often leaves in.
    if (assist || guard) computeDspGain(gDsp.data());
    if (guard) {
        updateFloor();
        const double r = computeVoicing();
        lastVoicing = r;
        // periodic AND clearly above the noise floor (pauses: resid < 0.5)
        const double rl = params.aiStrongVoice ? 3.0 : residLo, rh = params.aiStrongVoice ? 8.0 : residHi;
        const double gate = std::clamp((lastResid - rl) / (rh - rl), 0.0, 1.0);
        const double vp = std::clamp((std::min(r, 1.0) - voiceLo) / (voiceHi - voiceLo), 0.0, 1.0) * gate;
        // loud onsets (also unvoiced) well above the noise: protect from the first hop
        double vo = dbgOnset ? fastVad * gate : 0.0;
        // onsets count only when the network also hears speech (keyboard clicks, knocks do not)
        if (onsetNeedsVad) vo *= std::clamp(aiVad / 0.5, 0.0, 1.0);
        if (vp > 0.0 && lastLag > 0) combLag = lastLag;
        voicedP = vp > voicedP ? vp : aVoiceRel * voicedP + (1.0 - aVoiceRel) * vp;
        onsetEnv = vo > onsetEnv ? vo : aVoiceRel * onsetEnv + (1.0 - aVoiceRel) * vo;
        voiced = std::max(voicedP, onsetEnv);
    }
    if (aiNew) {
        aiNew = false;
        // Network bins: 481 over a 960-pt FFT at fs/scale. Ours: nFFT = 1024*scale
        // at fs, i.e. the same Hz spacing family, so bin k maps to k * 960/1024.
        const float gmin = (float) floorGain();
        const float* r = ai.gains();
        for (int k = 0; k < nBins; ++k) {
            const double pos = std::min((double) k * 960.0 / 1024.0, (double) (AiGains::kBins - 1));
            const int j = std::min((int) pos, AiGains::kBins - 2);
            const float t = (float) (pos - j);
            const float gr = std::clamp(r[j] + t * (r[j + 1] - r[j]), 0.0f, 1.0f);
            gAi[k] = gmin + (1.0f - gmin) * gr;
        }
        const double v = (double) ai.vadProbability();
        aiVad = v > aiVad ? v : 0.5 * aiVad + 0.5 * v; // per 10 ms frame: instant attack, ~15 ms release
    } else if (!assist && !guard) {
        curRev = nextRev; // finish the previous crossfade, hold the filter
        return;
    }
    if (!assist) {
        for (int k = 0; k < nBins; ++k) gain[k] = gAi[k];
    } else {
        // Hiss assist: extra attenuation only where the DSP estimate is lower than
        // the network's (stationary broadband noise the network leaves in). Full
        // weight between words, none while the DSP detector hears speech (the voice
        // keeps the network's more natural gains), capped at 20 dB so pauses do not
        // drop far below the noise left under the voice (no pumping).
        const double w = 1.0 - vad;
        const double kCap = assistCap; // -20 dB
        for (int k = 0; k < nBins; ++k) {
            const float a = gAi[k], d = gDsp[k];
            double wk = w;
            // noise-dominated bins (DSP gain low) get the assist even while a voice is present
            if (hissFix & 8) wk = std::max(wk, std::clamp((0.5 - (double) d) / 0.4, 0.0, 1.0));
            gain[k] = d < a ? a * (float) std::exp(wk * std::log(std::max((double) d / a, kCap))) : a;
        }
    }
    if (guard && dbgTail > 0.0) {
        // Voice tail: after a guarded voice the network may not gate the decay or
        // breath to silence at once; the floor falls from -12 dB over ~150 ms.
        tailEnv = std::max(std::min(1.0, voiced * dbgSat), aTail * tailEnv);
        // with pause cleanup the tail floor is lower (-20 dB): measured +2 dB in pauses, no loss on screams
        const float t = (float) ((params.pauseCleanup ? std::min(dbgTail, 0.1) : dbgTail) * tailEnv);
        if (t > 1e-4f) {
            if (hissFix & 1) {
                // only where there is signal above the long-term floor (hiss-only bins get no tail)
                for (int k = 0; k < nBins; ++k) {
                    const double q = 1.0 - 2.0 * floorBias * floorN[k] / std::max(psm[k], 1e-20);
                    gain[k] = std::max(gain[k], std::min(t, (float) std::sqrt(std::clamp(q, 0.0, 1.0))));
                }
            } else
                for (int k = 0; k < nBins; ++k) gain[k] = std::max(gain[k], std::min(t, gDspRawSafe(k)));
        }
    }
    if (guard && voiced > 0.0) {
        // Voice guard: while a pitched voice is present the network may not cut
        // below the DSP (stationary-noise) gain.
        const float vg = (float) std::min(1.0, voiced * dbgSat);
        if (dbgFloor) {
            // protective gain against the long-term floor (a scream cannot raise it),
            // softened like the DSP gain during speech
            // Pitched part protects only the bins at the detected voice's harmonics, so a
            // background voice or clatter between them is still removed; onsets are broadband.
            const float vpg = (float) std::min(1.0, voicedP * dbgSat), vog = (float) std::min(1.0, onsetEnv * dbgSat);
            const double binHz = fs / nFFT;
            const double f0 = combLag > 0 ? fs / combLag : 0.0;
            const bool useComb = dbgComb && f0 > 3.0 * binHz;
            for (int k = 0; k < nBins; ++k) {
                const double q = 1.0 - 2.0 * floorBias * floorN[k] / std::max(psm[k], 1e-20);
                gFloor[k] = (float) std::sqrt(std::clamp(q, 0.0, 1.0));
                float w = vpg;
                if (useComb) {
                    const double f = k * binHz;
                    const double h = std::max(1.0, std::round(f / f0));
                    const double tol = 1.5 + 0.5 * h * (fs / ((double) combLag * combLag)) / binHz;
                    const double dist = std::abs(f - h * f0) / binHz;
                    w = dist <= tol ? vpg : (dist <= tol + 1.0 ? vpg * (float) (tol + 1.0 - dist) : 0.0f);
                }
                w = std::max(w, vog);
                gain[k] = std::max(gain[k], w * std::max(gDsp[k], gFloor[k]));
            }
        } else {
            for (int k = 0; k < nBins; ++k) gain[k] = std::max(gain[k], vg * gDsp[k]);
        }
    }
    if (params.pauseCleanup) {
        // Pause cleanup: when no detector hears a voice for pcHold, every bin falls to the
        // Reduction floor over ~pcRel; any voice evidence restores the normal gains at once.
        // In AI mode the analysis runs 10 ms ahead of the audio, so the gains are back
        // before the voice reaches the output.
        double pres = 0.0;
        if (pcSrc & 1) pres = std::max(pres, aiVad);
        if (pcSrc & 2) pres = std::max(pres, voiced);
        if (pcSrc & 4) pres = std::max(pres, std::max(vad, fastVad));
        if (pcSrc & 8) pres = std::max(pres, voicedP); // pitched voice only (not the onset part)
        const long holdHops = (long) (pcHold * fs / hop);
        if (pres > pcThr) pcHoldCnt = holdHops; else if (pcHoldCnt > 0) --pcHoldCnt;
        const double aRelPc = std::exp(-(hop / fs) / std::max(pcRel, 1e-3));
        pcEnv = pcHoldCnt > 0 ? 1.0 : aRelPc * pcEnv;
        if (pcEnv < 0.999) {
            const float gmin = (float) floorGain();
            // interpolate in dB between the normal gain and the floor
            const float w = (float) pcEnv;
            for (int k = 0; k < nBins; ++k) {
                const float g = std::max(gain[k], gmin);
                gain[k] = std::exp(w * std::log(g) + (1.0f - w) * std::log(gmin));
            }
        }
    } else {
        pcEnv = 1.0; pcHoldCnt = 0;
    }
    applySteadiness(std::max({ aiVad, voiced, vad }));
    {   // the Reduction setting is a hard limit (hiss assist may otherwise go below it)
        const float gmin = (float) floorGain();
        for (int k = 0; k < nBins; ++k) gain[k] = std::max(gain[k], gmin);
    }
    for (int k = 0; k < nBins; ++k) gainOut[k] = gain[k];
    designMinPhase(gain.data(), hTmp.data());
    curRev.swap(nextRev);
    for (int i = 0; i < nTaps; ++i) nextRev[i] = hTmp[nTaps - 1 - i];
}

void ZeroDenoiser::applySteadiness(double presence)
{
    // Steady background (v0.13): under a voice, noise in the voice's own bins can only be
    // reduced a little (the bins carry the voice), while between words it can be removed
    // deeply. The jump between the two is heard as the background 'breathing' with the
    // voice. Track, per bin, the typical gain while a voice is present (power average, ~1.5 s)
    // and never let the gain without voice fall more than maxJumpDb below it.
    const bool speaking = presence > 0.5;
    if (speaking) {
        for (int k = 0; k < nBins; ++k)
            voiceRefPow[k] += aVoiceRef * ((double) gain[k] * gain[k] - voiceRefPow[k]);
    }
    if (params.maxJumpDb <= 0.0f) return;
    const float off = (float) (-params.maxJumpDb / 20.0 * std::log(10.0));
    for (int k = 0; k < nBins; ++k)
        gain[k] = std::max(gain[k], std::exp(0.5f * std::log((float) std::max(voiceRefPow[k], 1e-12)) + off));
}

double ZeroDenoiser::floorGain() const
{
    const double db = params.reductionDb >= 0.0f ? std::min((double) params.reductionDb, 120.0)
                                                 : std::clamp((double) params.amount, 0.0, 1.0) * 55.0;
    return std::pow(10.0, -db / 20.0);
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
