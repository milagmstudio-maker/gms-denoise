#pragma once
// Minimal WAV reader/writer for the test tools (PCM16/24/32, float32; any channels).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wav {
struct Audio { int sr = 48000; std::vector<std::vector<float>> ch; };

inline bool read(const std::string& path, Audio& a)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + r);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t p = 12; int fmt = 0, nch = 0, bits = 0;
    while (p + 8 <= d.size()) {
        uint32_t sz; std::memcpy(&sz, &d[p + 4], 4);
        if (!std::memcmp(&d[p], "fmt ", 4)) {
            uint16_t t; std::memcpy(&t, &d[p + 8], 2); fmt = t;
            uint16_t c; std::memcpy(&c, &d[p + 10], 2); nch = c;
            uint32_t s; std::memcpy(&s, &d[p + 12], 4); a.sr = (int) s;
            uint16_t b; std::memcpy(&b, &d[p + 22], 2); bits = b;
            if (fmt == 0xFFFE) { uint16_t sub; std::memcpy(&sub, &d[p + 32], 2); fmt = sub; }
        } else if (!std::memcmp(&d[p], "data", 4)) {
            const int bps = bits / 8;
            size_t n = std::min<size_t>(sz, d.size() - p - 8) / (bps * nch);
            a.ch.assign(nch, std::vector<float>(n));
            const uint8_t* q = &d[p + 8];
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < nch; ++c, q += bps) {
                    float v = 0;
                    if (fmt == 3 && bits == 32) std::memcpy(&v, q, 4);
                    else if (bits == 16) { int16_t s; std::memcpy(&s, q, 2); v = s / 32768.0f; }
                    else if (bits == 24) { int32_t s = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); v = (s >> 8) / 8388608.0f; }
                    else if (bits == 32) { int32_t s; std::memcpy(&s, q, 4); v = s / 2147483648.0f; }
                    a.ch[c][i] = v;
                }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

inline bool writeFloat(const std::string& path, const Audio& a)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t nch = (uint32_t) a.ch.size(), n = nch ? (uint32_t) a.ch[0].size() : 0;
    const uint32_t dataBytes = n * nch * 4, sr = (uint32_t) a.sr, br = sr * nch * 4;
    const uint16_t fmt = 3, c16 = (uint16_t) nch, ba = (uint16_t) (nch * 4), bits = 32;
    const uint32_t riff = 36 + dataBytes, fmtSz = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSz, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&c16, 2, 1, f);
    std::fwrite(&sr, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < nch; ++c) std::fwrite(&a.ch[c][i], 4, 1, f);
    std::fclose(f);
    return true;
}
} // namespace wav
