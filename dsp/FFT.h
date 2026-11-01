#pragma once
// Small self-contained radix-2 FFT used by the DSP core.
// Kept dependency-free so the core can be unit-tested outside JUCE.
// Real transforms use the standard "N/2 complex FFT + split" trick.
// Complex arithmetic is written out by hand: std::complex multiplication
// without -ffast-math goes through a slow NaN-checking path.

#include <cmath>
#include <complex>
#include <vector>

namespace zd {

class FFT {
public:
    using cpx = std::complex<float>;

    explicit FFT(int size = 0) { if (size > 0) prepare(size); }

    void prepare(int size)
    {
        n = size;
        h = n / 2;
        bitrev.assign((size_t) h, 0);
        int bits = 0;
        while ((1 << bits) < h) ++bits;
        for (int i = 0; i < h; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            bitrev[(size_t) i] = r;
        }
        // per-stage twiddles stored contiguously: stage len uses half = len/2 entries
        twr.clear(); twi.clear();
        for (int len = 2; len <= h; len <<= 1) {
            const int half = len >> 1;
            for (int j = 0; j < half; ++j) {
                const double a = -2.0 * M_PI * j / len;
                twr.push_back((float) std::cos(a));
                twi.push_back((float) std::sin(a));
            }
        }
        rr.assign((size_t) h + 1, 0.0f); ri.assign((size_t) h + 1, 0.0f);
        for (int k = 0; k <= h; ++k) {
            const double a = -2.0 * M_PI * k / n;
            rr[(size_t) k] = (float) std::cos(a);
            ri[(size_t) k] = (float) std::sin(a);
        }
        wr.assign((size_t) h, 0.0f); wi.assign((size_t) h, 0.0f);
    }

    int size() const { return n; }

    // in: n real samples. out: n/2+1 complex bins (unnormalised).
    void forwardReal(const float* in, cpx* out)
    {
        for (int i = 0; i < h; ++i) {
            const int r = bitrev[(size_t) i];
            wr[(size_t) r] = in[2 * i];
            wi[(size_t) r] = in[2 * i + 1];
        }
        butterflies(false);
        out[0] = cpx(wr[0] + wi[0], 0.0f);
        out[h] = cpx(wr[0] - wi[0], 0.0f);
        for (int k = 1; k < h; ++k) {
            const float ar = wr[(size_t) k], ai = wi[(size_t) k];
            const float br = wr[(size_t) (h - k)], bi = -wi[(size_t) (h - k)]; // conj(Z[h-k])
            const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
            // o = -0.5i * (a - b)
            const float dr = ar - br, di = ai - bi;
            const float orr = 0.5f * di, oi = -0.5f * dr;
            const float tr = rr[(size_t) k], ti = ri[(size_t) k];
            out[k] = cpx(er + tr * orr - ti * oi, ei + tr * oi + ti * orr);
        }
    }

    // in: n/2+1 complex bins. out: n real samples, scaled by 1/n (true inverse).
    void inverseReal(const cpx* in, float* out)
    {
        for (int k = 0; k < h; ++k) {
            const float ar = in[k].real(), ai = in[k].imag();
            const float br = in[h - k].real(), bi = -in[h - k].imag();
            const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
            const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
            // o = d * conj(W^k)
            const float tr = rr[(size_t) k], ti = -ri[(size_t) k];
            const float orr = dr * tr - di * ti, oi = dr * ti + di * tr;
            // Z = e + i*o
            const int r = bitrev[(size_t) k];
            wr[(size_t) r] = er - oi;
            wi[(size_t) r] = ei + orr;
        }
        butterflies(true);
        const float s = 1.0f / (float) h;
        for (int i = 0; i < h; ++i) {
            out[2 * i] = wr[(size_t) i] * s;
            out[2 * i + 1] = wi[(size_t) i] * s;
        }
    }

private:
    void butterflies(bool inverse)
    {
        float* xr = wr.data();
        float* xi = wi.data();
        const float sgn = inverse ? -1.0f : 1.0f;
        size_t off = 0;
        for (int len = 2; len <= h; len <<= 1) {
            const int half = len >> 1;
            const float* tR = twr.data() + off;
            const float* tI = twi.data() + off;
            for (int i = 0; i < h; i += len) {
                float* ar = xr + i; float* ai = xi + i;
                float* br = xr + i + half; float* bi = xi + i + half;
                for (int j = 0; j < half; ++j) {
                    const float cr = tR[j], ci = sgn * tI[j];
                    const float vr = br[j] * cr - bi[j] * ci;
                    const float vi = br[j] * ci + bi[j] * cr;
                    const float ur = ar[j], ui = ai[j];
                    ar[j] = ur + vr; ai[j] = ui + vi;
                    br[j] = ur - vr; bi[j] = ui - vi;
                }
            }
            off += (size_t) half;
        }
    }

    int n = 0, h = 0;
    std::vector<int> bitrev;
    std::vector<float> twr, twi, rr, ri, wr, wi;
};

} // namespace zd
