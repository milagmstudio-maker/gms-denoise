// Click / glitch stress test: loads the built VST3 through JUCE's VST3 hosting
// (like a DAW) and looks for anything the plug-in ADDS to the signal - clicks,
// pops, crackle, bursts, NaN/Inf - under hostile host behaviour: odd and random
// block sizes, parameter automation, mode switches, gate and learn toggling,
// extreme inputs, re-prepare, other sample rates.
//
// Detector: the plug-in can only attenuate, so in every 2.5 ms frame the output
// energy above 6 kHz must not exceed the input energy above 6 kHz (max over the
// frames that can reach that output through latency + FIR) by more than 6 dB.
// Test signals are chosen so a click is far above that bound: low tones plus a
// low hiss floor, and real speech in noise.
//
// usage: zd_glitch_check path/to/Plugin.vst3 [speech.wav] [outdir-for-wavs]
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <random>
#include <vector>

using Sig = std::vector<std::vector<float>>; // [ch][n]

static juce::String g_plugin;
static int g_fails = 0;
static juce::File g_outDir;

struct Inst {
    std::unique_ptr<juce::AudioPluginInstance> p;
    std::map<juce::String, juce::AudioProcessorParameter*> par;
    double fs = 48000;
    void set(const juce::String& name, float norm) { if (auto* x = par[name]) x->setValueNotifyingHost(juce::jlimit(0.f, 1.f, norm)); }
    void setMode(int m) { set("Mode", m / 3.0f); }
    void setGateDb(float db) { set("Gate Threshold", (db + 90.f) / 80.f); } // -90 (Off) .. -10
};

static Inst makeInst(double fs, int maxBlock, int nCh = 2)
{
    juce::AudioPluginFormatManager fm;
    fm.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> types;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(types, g_plugin);
    juce::String err;
    Inst in;
    in.fs = fs;
    in.p = fm.createPluginInstance(*types[0], fs, maxBlock, err);
    if (!in.p) { std::printf("FAIL load: %s\n", err.toRawUTF8()); std::exit(1); }
    if (nCh == 1) {
        auto layout = in.p->getBusesLayout();
        layout.inputBuses.getReference(0) = juce::AudioChannelSet::mono();
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
        in.p->setBusesLayout(layout);
    }
    in.p->setPlayConfigDetails(nCh, nCh, fs, maxBlock);
    in.p->prepareToPlay(fs, maxBlock);
    for (auto* x : in.p->getParameters()) in.par[x->getName(64)] = x;
    return in;
}

// Runs the signal through, calling `hook(pos)` before each block (automation).
static Sig run(Inst& in, const Sig& x, std::function<int(long)> blockAt, std::function<void(long)> hook = {})
{
    const int nCh = (int) x.size();
    const long N = (long) x[0].size();
    Sig y(nCh, std::vector<float>((size_t) N));
    juce::AudioBuffer<float> buf(nCh, 8192);
    juce::MidiBuffer midi;
    for (long pos = 0; pos < N;) {
        int b = (int) std::min<long>(blockAt(pos), N - pos);
        if (hook) hook(pos);
        juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), nCh, b);
        for (int c = 0; c < nCh; ++c) std::memcpy(view.getWritePointer(c), x[c].data() + pos, sizeof(float) * (size_t) b);
        in.p->processBlock(view, midi);
        for (int c = 0; c < nCh; ++c) std::memcpy(y[c].data() + pos, view.getReadPointer(c), sizeof(float) * (size_t) b);
        pos += b;
    }
    return y;
}

// 4th-order Butterworth high-pass (two biquads), per channel.
static std::vector<double> highpass(const std::vector<float>& s, double fs, double fc)
{
    std::vector<double> o(s.begin(), s.end());
    const double qs[2] = { 0.54119610, 1.3065630 };
    for (double q : qs) {
        const double w = 2 * M_PI * fc / fs, cw = std::cos(w), al = std::sin(w) / (2 * q);
        const double b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = b0, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : o) {
            const double xx = v, yy = (b0 * xx + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1; x1 = xx; y2 = y1; y1 = yy; v = yy;
        }
    }
    return o;
}

struct GlitchStats { int clicks = 0; double worstExcessDb = -99; double worstAtSec = 0; bool finite = true; double peak = 0; };

static GlitchStats analyse(const Sig& x, const Sig& y, double fs, double skipSec = 0.3, double floorDb = -90)
{
    GlitchStats st;
    const int frame = (int) std::lround(fs * 0.0025);
    const int reach = (int) std::ceil((fs * 0.025) / frame) + 1; // latency (<=10.7 ms) + FIR (10.7 ms) + margin
    for (size_t c = 0; c < x.size(); ++c) {
        for (float v : y[c]) { if (!std::isfinite(v)) st.finite = false; else st.peak = std::max(st.peak, (double) std::fabs(v)); }
        if (!st.finite) return st;
        auto hx = highpass(x[c], fs, 6000), hy = highpass(y[c], fs, 6000);
        const long nf = (long) hx.size() / frame;
        std::vector<double> ex((size_t) nf), ey((size_t) nf);
        for (long f = 0; f < nf; ++f) {
            double a = 0, b = 0;
            for (int i = 0; i < frame; ++i) { a += hx[(size_t) (f * frame + i)] * hx[(size_t) (f * frame + i)]; b += hy[(size_t) (f * frame + i)] * hy[(size_t) (f * frame + i)]; }
            ex[(size_t) f] = a / frame; ey[(size_t) f] = b / frame;
        }
        const double floorP = std::pow(10.0, floorDb / 10.0);
        bool inClick = false;
        for (long f = (long) (skipSec * fs / frame); f < nf; ++f) {
            double ref = 0;
            for (long g = std::max(0L, f - reach); g <= std::min(nf - 1, f + 1); ++g) ref = std::max(ref, ex[(size_t) g]);
            const double excess = 10 * std::log10((ey[(size_t) f] + 1e-30) / (ref + floorP));
            if (excess > st.worstExcessDb) { st.worstExcessDb = excess; st.worstAtSec = f * frame / fs; }
            const bool cl = excess > 6.0;
            if (cl && !inClick) ++st.clicks;
            inClick = cl;
        }
    }
    return st;
}

static void report(const juce::String& name, const GlitchStats& s, bool expectClean = true)
{
    const bool ok = s.finite && (!expectClean || s.clicks == 0);
    if (!ok) ++g_fails;
    std::printf("%s %-58s clicks %3d  worst %+6.1f dB @%6.2fs  peak %.3f%s\n", ok ? "ok  " : "FAIL", name.toRawUTF8(),
                s.clicks, s.worstExcessDb, s.worstAtSec, s.peak, s.finite ? "" : "  NON-FINITE OUTPUT");
}

static void writeWav(const juce::String& name, const Sig& y, double fs)
{
    if (g_outDir == juce::File()) return;
    g_outDir.createDirectory();
    juce::WavAudioFormat wav;
    auto f = g_outDir.getChildFile(name + ".wav");
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), fs, (unsigned) y.size(), 24, {}, 0));
    if (!w) return;
    os.release();
    juce::AudioBuffer<float> b((int) y.size(), (int) y[0].size());
    for (size_t c = 0; c < y.size(); ++c) b.copyFrom((int) c, 0, y[c].data(), (int) y[c].size());
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}

// tones (150 + 700 Hz) at -14 dBFS each + white hiss at -60 dBFS, stereo
static Sig tones(double fs, double sec, double hissDb = -60)
{
    const long N = (long) (fs * sec);
    Sig s(2, std::vector<float>((size_t) N));
    std::mt19937 rng(7); std::normal_distribution<float> nd(0, 1);
    const float h = (float) std::pow(10.0, hissDb / 20.0), a = (float) std::pow(10.0, -14 / 20.0);
    for (long n = 0; n < N; ++n) {
        const double t = n / fs;
        const float v = a * (float) (std::sin(2 * M_PI * 150 * t) + std::sin(2 * M_PI * 700 * t + 1.0));
        for (int c = 0; c < 2; ++c) s[(size_t) c][(size_t) n] = v + h * nd(rng);
    }
    return s;
}

static Sig speechInNoise(const juce::File& wavFile, double fs, double snrDb)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(wavFile));
    if (!r) return {};
    juce::AudioBuffer<float> b(1, (int) r->lengthInSamples);
    r->read(&b, 0, b.getNumSamples(), 0, true, false);
    const long N = b.getNumSamples();
    double ps = 0; for (long n = 0; n < N; ++n) ps += b.getSample(0, (int) n) * b.getSample(0, (int) n);
    ps /= N;
    const float sc = (float) (0.1 / std::sqrt(ps)); // speech at -20 dBFS RMS
    const float nl = (float) (0.1 * std::pow(10.0, -snrDb / 20.0));
    std::mt19937 rng(3); std::normal_distribution<float> nd(0, 1);
    Sig s(2, std::vector<float>((size_t) N));
    for (long n = 0; n < N; ++n) {
        const float v = sc * b.getSample(0, (int) n);
        s[0][(size_t) n] = v + nl * nd(rng);
        s[1][(size_t) n] = v + nl * nd(rng);
    }
    (void) fs;
    return s;
}

static double maxDiff(const Sig& a, const Sig& b)
{
    double m = 0;
    for (size_t c = 0; c < a.size(); ++c) for (size_t n = 0; n < a[c].size(); ++n) m = std::max(m, (double) std::fabs(a[c][n] - b[c][n]));
    return m;
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf("usage: zd_glitch_check Plugin.vst3 [speech.wav] [wavdir]\n"); return 2; }
    g_plugin = argv[1];
    juce::File speechFile = argc > 2 ? juce::File(argv[2]) : juce::File();
    if (argc > 3) g_outDir = juce::File(argv[3]);
    const char* modeName[4] = { "Zero", "Low", "HQ", "AI" };
    auto fixed = [](int b) { return [b](long) { return b; }; };

    const double fs = 48000;
    const Sig T = tones(fs, 8.0);
    Sig S = speechFile.existsAsFile() ? speechInNoise(speechFile, fs, 10) : Sig();
    if (S.empty()) std::printf("note: no speech file, speech tests skipped\n");

    std::printf("== 1. steady playback, every mode, tones+hiss and speech in noise (block 256)\n");
    for (int m = 0; m < 4; ++m) {
        auto in = makeInst(fs, 4096); in.setMode(m);
        report(juce::String("steady tones, ") + modeName[m], analyse(T, run(in, T, fixed(256)), fs));
        if (!S.empty()) {
            auto in2 = makeInst(fs, 4096); in2.setMode(m); in2.set("Amount", 1.0f);
            auto y = run(in2, S, fixed(256));
            report(juce::String("steady speech 10 dB SNR, amount 100, ") + modeName[m], analyse(S, y, fs));
            writeWav(juce::String("steady_speech_") + modeName[m], y, fs);
        }
    }

    std::printf("== 2. block sizes: output must be identical to block 256 (and click-free)\n");
    for (int m = 0; m < 4; ++m) {
        const Sig& X = S.empty() ? T : S;
        auto ref = makeInst(fs, 4096); ref.setMode(m);
        auto yRef = run(ref, X, fixed(256));
        for (int b : { 1, 2, 31, 32, 64, 100, 441, 511, 512, 1024, 2048, 4096 }) {
            auto in = makeInst(fs, 4096); in.setMode(m);
            auto y = run(in, X, fixed(b));
            const double d = maxDiff(y, yRef);
            const bool ok = d < 1e-6;
            if (!ok) ++g_fails;
            std::printf("%s %s block %4d: max |diff| vs block 256 = %.2e\n", ok ? "ok  " : "FAIL", modeName[m], b, d);
        }
        std::mt19937 rng(11 + m); std::uniform_int_distribution<int> ud(1, 4096);
        auto in = makeInst(fs, 4096); in.setMode(m);
        auto y = run(in, X, [&](long) { return ud(rng); });
        const double d = maxDiff(y, yRef);
        if (d >= 1e-6) ++g_fails;
        std::printf("%s %s random blocks 1..4096: max |diff| = %.2e\n", d < 1e-6 ? "ok  " : "FAIL", modeName[m], d);
    }

    std::printf("== 3. parameter automation (random jumps every 10 ms / every block)\n");
    const char* contParams[] = { "Amount", "Reduction", "Output" };
    for (int m = 0; m < 4; ++m) {
        for (const Sig* X : std::initializer_list<const Sig*>{ &T, &S }) {
            if (X->empty()) continue;
            const bool sp = X == &S;
            {   // every continuous parameter jumping to random values every 10 ms
                auto in = makeInst(fs, 4096); in.setMode(m);
                std::mt19937 rng(5); std::uniform_real_distribution<float> u(0, 1);
                long next = 0;
                auto y = run(in, *X, fixed(64), [&](long pos) {
                    if (pos >= next) { next += 480; for (auto* n : contParams) in.set(n, juce::String(n) == "Output" ? 0.5f : u(rng)); }
                });
                report(juce::String("Amount+Reduction random jumps /10ms, ") + (sp ? "speech, " : "tones, ") + modeName[m], analyse(*X, y, fs));
                if (sp) writeWav(juce::String("auto_amount_reduction_") + modeName[m], y, fs);
            }
            {   // Output gain jumps -12..+12 every 10 ms (scaled reference: compare against scaled input)
                auto in = makeInst(fs, 4096); in.setMode(m); in.set("Amount", 0.0f); in.set("Reduction", 0.0f);
                std::mt19937 rng(6); std::uniform_real_distribution<float> u(0, 1);
                long next = 0;
                auto y = run(in, *X, fixed(64), [&](long pos) { if (pos >= next) { next += 480; in.set("Output", u(rng)); } });
                Sig xs = *X; for (auto& ch : xs) for (auto& v : ch) v *= 3.98f; // +12 dB bound
                report(juce::String("Output gain jumps +-12 dB /10ms, ") + (sp ? "speech, " : "tones, ") + modeName[m], analyse(xs, y, fs));
            }
            {   // switches toggling every 150 ms: strength, pause cleanup, voice protect, adaptive
                auto in = makeInst(fs, 4096); in.setMode(m); in.set("Amount", 1.0f);
                long next = 0; int k = 0;
                auto y = run(in, *X, fixed(128), [&](long pos) {
                    if (pos >= next) {
                        next += 7200; ++k;
                        in.set("Strength", (k % 3) / 2.0f);
                        in.set("Pause Cleanup", (k / 2) % 2 ? 1.0f : 0.0f);
                        in.set("Strong Voice Protect", (k / 3) % 2 ? 1.0f : 0.0f);
                        in.set("Adaptive", (k / 5) % 2 ? 0.0f : 1.0f);
                    }
                });
                report(juce::String("switches toggling /150ms, ") + (sp ? "speech, " : "tones, ") + modeName[m], analyse(*X, y, fs));
            }
            {   // Learn on/off every 400 ms
                auto in = makeInst(fs, 4096); in.setMode(m);
                long next = 0; bool on = false;
                auto y = run(in, *X, fixed(256), [&](long pos) { if (pos >= next) { next += 19200; on = !on; in.set("Learn", on ? 1.f : 0.f); } });
                report(juce::String("Learn toggling /400ms, ") + (sp ? "speech, " : "tones, ") + modeName[m], analyse(*X, y, fs));
            }
        }
    }

    std::printf("== 4. gate\n");
    for (int m = 0; m < 4; ++m) {
        if (!S.empty()) {
            auto in = makeInst(fs, 4096); in.setMode(m); in.setGateDb(-35);
            auto y = run(in, S, fixed(256));
            report(juce::String("gate -35 dB on speech, ") + modeName[m], analyse(S, y, fs));
            writeWav(juce::String("gate35_speech_") + modeName[m], y, fs);
        }
        {   // tone bursts crossing the threshold
            Sig X = T;
            for (size_t n = 0; n < X[0].size(); ++n) { const float g = ((n / 12000) % 2) ? 1.0f : 0.01f; for (auto& ch : X) ch[n] *= g; }
            auto in = makeInst(fs, 4096); in.setMode(m); in.setGateDb(-30);
            report(juce::String("gate -30 dB on tone bursts, ") + modeName[m], analyse(X, run(in, X, fixed(256)), fs));
        }
        {   // gate threshold toggled between Off and -10 dB (closed) every 200 ms, while signal plays
            Sig X = T; for (auto& ch : X) for (auto& v : ch) v *= 0.1f; // -34 dBFS tones: below a -10 dB threshold
            auto in = makeInst(fs, 4096); in.setMode(m); in.set("Amount", 0.0f); in.set("Reduction", 0.0f);
            long next = 0; bool on = false; long closedSamples = 0, pos0 = 0;
            auto y = run(in, X, fixed(256), [&](long pos) { if (pos >= next) { next += 9600; on = !on; in.setGateDb(on ? -10.f : -90.f); } pos0 = pos; });
            for (size_t n = 0; n < y[0].size(); ++n) if (y[0][n] == 0.0f) ++closedSamples;
            (void) pos0;
            std::printf("     (gate fully closed for %.0f %% of the time)\n", 100.0 * closedSamples / (double) y[0].size());
            report(juce::String("gate Off <-> closed every 200 ms, tones, ") + modeName[m], analyse(X, y, fs));
            writeWav(juce::String("gate_toggle_") + modeName[m], y, fs);
        }
    }

    std::printf("== 5. mode switch during playback (latency changes)\n");
    for (const Sig* X : std::initializer_list<const Sig*>{ &T, &S }) {
        if (X->empty()) continue;
        auto in = makeInst(fs, 4096);
        long next = 0; int k = 0;
        const int seq[] = { 1, 2, 3, 0, 2, 0, 3, 1 };
        auto y = run(in, *X, fixed(256), [&](long pos) { if (pos >= next) { next += 24000; in.setMode(seq[k++ % 8]); } });
        report(juce::String("mode switch every 0.5 s, ") + (X == &S ? "speech" : "tones"), analyse(*X, y, fs));
        writeWav(juce::String("mode_switch_") + (X == &S ? "speech" : "tones"), y, fs);
    }

    std::printf("== 6. extreme inputs\n");
    for (int m = 0; m < 4; ++m) {
        const long N = (long) (fs * 3);
        auto mk = [&](std::function<float(long, int)> f) { Sig s(2, std::vector<float>((size_t) N)); for (long n = 0; n < N; ++n) for (int c = 0; c < 2; ++c) s[(size_t) c][(size_t) n] = f(n, c); return s; };
        std::mt19937 rng(9); std::uniform_real_distribution<float> u(-1, 1);
        struct Case { const char* name; Sig x; };
        std::vector<Case> cases;
        cases.push_back({ "silence", mk([](long, int) { return 0.0f; }) });
        cases.push_back({ "DC +0.5", mk([](long, int) { return 0.5f; }) });
        cases.push_back({ "full-scale white noise", mk([&](long, int) { return u(rng); }) });
        cases.push_back({ "full-scale 100 Hz square", mk([&](long n, int) { return (n / 240) % 2 ? 1.0f : -1.0f; }) });
        cases.push_back({ "impulses every 0.5 s", mk([&](long n, int) { return n % 24000 == 0 ? 1.0f : 0.0f; }) });
        cases.push_back({ "denormal-level noise 1e-38", mk([&](long, int) { return u(rng) * 1e-38f; }) });
        cases.push_back({ "L only (R silent)", mk([&](long n, int c) { return c == 0 ? 0.3f * (float) std::sin(2 * M_PI * 440 * n / fs) : 0.0f; }) });
        for (auto& cs : cases) {
            auto in = makeInst(fs, 4096); in.setMode(m); in.set("Amount", 1.0f);
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            auto y = run(in, cs.x, fixed(256));
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            bool fin = true; double pk = 0, rPk = 0;
            for (int c = 0; c < 2; ++c) for (float v : y[(size_t) c]) { if (!std::isfinite(v)) fin = false; else { pk = std::max(pk, (double) std::fabs(v)); if (c == 1) rPk = std::max(rPk, (double) std::fabs(v)); } }
            bool ok = fin && pk < 2.0;
            if (cs.name == std::string("silence")) ok = ok && pk == 0.0;
            if (cs.name == std::string("L only (R silent)")) ok = ok && rPk == 0.0;
            if (!ok) ++g_fails;
            std::printf("%s %s %-28s peak out %.4f  R peak %.2e  %s  %.0f ms for 3 s\n", ok ? "ok  " : "FAIL", modeName[m], cs.name, pk, rPk, fin ? "finite" : "NON-FINITE", ms);
        }
    }

    std::printf("== 7. one NaN / Inf sample from the host, then normal audio\n");
    for (int m = 0; m < 4; ++m) {
        for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
            Sig X = T;
            X[0][(size_t) (fs * 1.0)] = bad;
            auto in = makeInst(fs, 4096); in.setMode(m);
            auto y = run(in, X, fixed(256));
            long lastBad = -1;
            for (size_t n = 0; n < y[0].size(); ++n) if (!std::isfinite(y[0][n]) || !std::isfinite(y[1][n])) lastBad = (long) n;
            const bool ok = lastBad < (long) (fs * 1.5);
            if (!ok) ++g_fails;
            std::printf("%s %s %s at 1.0 s: last non-finite output at %s\n", ok ? "ok  " : "FAIL", modeName[m], std::isnan(bad) ? "NaN" : "Inf",
                        lastBad < 0 ? "none" : juce::String(lastBad / fs, 3).toRawUTF8());
        }
    }

    std::printf("== 8. re-prepare mid-stream and other sample rates\n");
    for (double rate : { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 }) {
        const Sig X = tones(rate, 4.0);
        for (int m : { 0, 3 }) {
            auto in = makeInst(rate, 4096); in.setMode(m);
            report(juce::String("tones at ") + juce::String(rate / 1000, 1) + " kHz, " + modeName[m], analyse(X, run(in, X, fixed(512)), rate));
        }
    }
    {
        auto in = makeInst(fs, 4096);
        long next = 48000;
        auto y = run(in, T, fixed(256), [&](long pos) { if (pos >= next) { next += 96000; in.p->releaseResources(); in.p->prepareToPlay(fs, 4096); } });
        report("prepareToPlay again every 2 s while playing (Zero)", analyse(T, y, fs));
    }
    {
        auto in = makeInst(fs, 4096); in.setMode(2);
        long next = 48000;
        auto y = run(in, T, fixed(256), [&](long pos) { if (pos >= next) { next += 96000; in.p->reset(); } });
        report("reset() every 2 s while playing (HQ)", analyse(T, y, fs));
    }
    {   // mono layout
        Sig X(1, T[0]);
        auto in = makeInst(fs, 4096, 1);
        report("mono bus, tones", analyse(X, run(in, X, fixed(256)), fs));
    }
    {   // identical L/R in -> identical out
        Sig X = S.empty() ? T : S; X[1] = X[0];
        auto in = makeInst(fs, 4096); in.setMode(3);
        auto y = run(in, X, fixed(256));
        double d = 0; for (size_t n = 0; n < y[0].size(); ++n) d = std::max(d, (double) std::fabs(y[0][n] - y[1][n]));
        if (d != 0) ++g_fails;
        std::printf("%s identical L/R in -> max |L-R| out = %.2e\n", d == 0 ? "ok  " : "FAIL", d);
    }

    std::printf("\n%s (%d failures)\n", g_fails == 0 ? "ALL PASSED" : "FAILURES", g_fails);
    return g_fails == 0 ? 0 : 1;
}
