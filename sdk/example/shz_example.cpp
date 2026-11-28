#define _USE_MATH_DEFINES
// Example / self-test for the Shizumaru embedding API.
//   shz_example                      -> self-test on generated noise, every mode
//   shz_example in.wav out.wav [mode] -> process a WAV (16/24/32-bit PCM or 32-bit float)
#include "../shizumaru.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

struct Wav { int ch = 0, rate = 0; std::vector<std::vector<float>> d; };

static bool readWav(const char* path, Wav& w)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> b;
    unsigned char tmp[65536]; size_t r;
    while ((r = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + r);
    std::fclose(f);
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, bits = 0; size_t pos = 12;
    while (pos + 8 <= b.size()) {
        uint32_t sz; std::memcpy(&sz, &b[pos + 4], 4);
        const unsigned char* c = &b[pos + 8];
        if (!std::memcmp(&b[pos], "fmt ", 4)) {
            uint16_t t, ch, bps; uint32_t sr;
            std::memcpy(&t, c, 2); std::memcpy(&ch, c + 2, 2); std::memcpy(&sr, c + 4, 4); std::memcpy(&bps, c + 14, 2);
            if (t == 0xFFFE && sz >= 26) std::memcpy(&t, c + 24, 2);
            fmt = t; w.ch = ch; w.rate = (int) sr; bits = bps;
        } else if (!std::memcmp(&b[pos], "data", 4) && w.ch > 0) {
            const int bytes = bits / 8;
            const size_t frames = std::min<size_t>(sz, b.size() - pos - 8) / (size_t) (bytes * w.ch);
            w.d.assign((size_t) w.ch, std::vector<float>(frames));
            for (size_t i = 0; i < frames; ++i)
                for (int k = 0; k < w.ch; ++k) {
                    const unsigned char* s = c + (i * (size_t) w.ch + (size_t) k) * (size_t) bytes;
                    float v = 0;
                    if (fmt == 3 && bits == 32) std::memcpy(&v, s, 4);
                    else if (bits == 16) { int16_t x; std::memcpy(&x, s, 2); v = x / 32768.0f; }
                    else if (bits == 24) { int32_t x = (s[0] << 8) | (s[1] << 16) | (s[2] << 24); v = (float) (x / 2147483648.0); }
                    else if (bits == 32) { int32_t x; std::memcpy(&x, s, 4); v = (float) (x / 2147483648.0); }
                    w.d[(size_t) k][i] = v;
                }
            return true;
        }
        pos += 8 + sz + (sz & 1);
    }
    return false;
}

static void writeWav(const char* path, const Wav& w)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint32_t n = (uint32_t) w.d[0].size(), dataBytes = n * (uint32_t) w.ch * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(3); u16((uint16_t) w.ch); u32((uint32_t) w.rate); u32((uint32_t) w.rate * (uint32_t) w.ch * 4); u16((uint16_t) (w.ch * 4)); u16(32);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (uint32_t i = 0; i < n; ++i) for (int c = 0; c < w.ch; ++c) std::fwrite(&w.d[(size_t) c][i], 4, 1, f);
    std::fclose(f);
}

static void run(shz_engine* e, Wav& w, int block)
{
    std::vector<float*> p((size_t) w.ch);
    const size_t n = w.d[0].size();
    for (size_t i = 0; i < n; i += (size_t) block) {
        const int k = (int) std::min<size_t>((size_t) block, n - i);
        for (int c = 0; c < w.ch; ++c) p[(size_t) c] = w.d[(size_t) c].data() + i;
        shz_process(e, p.data(), k);
    }
}

int main(int argc, char** argv)
{
    if (argc >= 3) {
        Wav w;
        if (!readWav(argv[1], w)) { std::printf("cannot read %s\n", argv[1]); return 1; }
        const int mode = argc > 3 ? std::atoi(argv[3]) : SHZ_MODE_AI_DEEP;
        shz_engine* e = shz_create(w.rate, w.ch, 512);
        shz_set_mode(e, mode);
        shz_preload(e, mode);
        run(e, w, 512);
        std::printf("mode %d, latency %d samples (%.1f ms) - output is delayed by that much\n", mode, shz_get_latency(e), 1000.0 * shz_get_latency(e) / w.rate);
        shz_destroy(e);
        writeWav(argv[2], w);
        return 0;
    }
    // self-test: white noise must be reduced in every mode; latency must match shz_latency_for
    int fails = 0;
    for (int mode = 0; mode < 6; ++mode) {
        const int rate = 48000;
        Wav w; w.ch = 2; w.rate = rate; w.d.assign(2, std::vector<float>((size_t) rate * 3));
        std::mt19937 rng(1); std::normal_distribution<float> nd(0.f, 0.05f);
        for (size_t i = 0; i < w.d[0].size(); ++i) w.d[0][i] = w.d[1][i] = nd(rng);
        double inE = 0; for (size_t i = (size_t) rate * 2; i < w.d[0].size(); ++i) inE += w.d[0][i] * w.d[0][i];
        shz_engine* e = shz_create(rate, 2, 256);
        shz_params p = shz_default_params(); p.amount = 100; p.strength = SHZ_STRONG;
        shz_set_params(e, &p);
        shz_set_mode(e, mode);
        const int ready = shz_preload(e, mode);
        run(e, w, 256);
        double outE = 0; for (size_t i = (size_t) rate * 2; i < w.d[0].size(); ++i) outE += w.d[0][i] * w.d[0][i];
        const double red = 10 * std::log10(outE / inE);
        const bool ok = ready && red < -25.0 && shz_get_latency(e) == shz_latency_for(mode, rate);
        fails += ok ? 0 : 1;
        std::printf("%s mode %d: white noise %.1f dB, latency %d samples\n", ok ? "ok  " : "FAIL", mode, red, shz_get_latency(e));
        shz_destroy(e);
    }
    {   // mode switching every 0.25 s while playing (AI Deep loads in the background): no NaN, no jumps
        const int rate = 44100;
        shz_engine* e = shz_create(rate, 2, 512);
        std::vector<float> L(512), R(512);
        float* p[2] = { L.data(), R.data() };
        double ph = 0, prev = 0, worst = 0; bool fin = true; long t = 0;
        for (int blk = 0; blk < rate * 6 / 512; ++blk) {
            if (blk % (rate / 4 / 512) == 0) shz_set_mode(e, (blk / (rate / 4 / 512)) % 6);
            for (int i = 0; i < 512; ++i, ++t) { L[(size_t) i] = R[(size_t) i] = (float) (0.3 * std::sin(ph)); ph += 2 * M_PI * 200 / rate; }
            shz_process(e, p, 512);
            for (int i = 0; i < 512; ++i) { fin = fin && std::isfinite(L[(size_t) i]); worst = std::max(worst, std::fabs(L[(size_t) i] - prev)); prev = L[(size_t) i]; }
        }
        // a 200 Hz sine at 0.3 changes by at most 0.0086 per sample; crossfades of shifted copies stay well under 0.05
        const bool ok = fin && worst < 0.05;
        fails += ok ? 0 : 1;
        std::printf("%s mode switch every 0.25 s @44.1 kHz: max sample step %.4f\n", ok ? "ok  " : "FAIL", worst);
        shz_destroy(e);
    }
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
