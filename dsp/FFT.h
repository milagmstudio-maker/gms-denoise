#pragma once
// Small self-contained radix-2 FFT used by the DSP core.
// Kept dependency-free so the core can be unit-tested outside JUCE.
// Real transforms use the standard "N/2 complex FFT + split" trick.

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
        // complex FFT of length h
        bitrev.resize(h);
        int bits = 0;
        while ((1 << bits) < h) ++bits;
        for (int i = 0; i < h; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            bitrev[i] = r;
        }
        tw.resize(h / 2 > 0 ? h / 2 : 1);
        for (int i = 0; i < (int) tw.size(); ++i) {
            double a = -2.0 * M_PI * i / h;
            tw[i] = cpx((float) std::cos(a), (float) std::sin(a));
        }
        // split twiddles for real FFT
        rtw.resize(h + 1);
        for (int k = 0; k <= h; ++k) {
            double a = -2.0 * M_PI * k / n;
            rtw[k] = cpx((float) std::cos(a), (float) std::sin(a));
        }
        work.resize(h);
    }

    int size() const { return n; }

    // in: n real samples. out: n/2+1 complex bins (unnormalised).
    void forwardReal(const float* in, cpx* out)
    {
        for (int i = 0; i < h; ++i) work[bitrev[i]] = cpx(in[2 * i], in[2 * i + 1]);
        butterflies(work.data(), false);
        // split: X[k] = (Z[k] + conj(Z[h-k]))/2 + W^k (Z[k] - conj(Z[h-k]))/(2i)
        out[0] = cpx(work[0].real() + work[0].imag(), 0.0f);
        out[h] = cpx(work[0].real() - work[0].imag(), 0.0f);
        for (int k = 1; k < h; ++k) {
            cpx a = work[k];
            cpx b = std::conj(work[h - k]);
            cpx e = 0.5f * (a + b);
            cpx o = cpx(0.0f, -0.5f) * (a - b);
            out[k] = e + rtw[k] * o;
        }
    }

    // in: n/2+1 complex bins. out: n real samples, scaled by 1/n (true inverse).
    void inverseReal(const cpx* in, float* out)
    {
        // rebuild Z[k] = E[k] + i*O[k], with E/O from Hermitian spectrum
        for (int k = 0; k < h; ++k) {
            cpx a = in[k];
            cpx b = std::conj(in[h - k]);
            cpx e = 0.5f * (a + b);
            cpx o = 0.5f * (a - b) * std::conj(rtw[k]);
            work[bitrev[k]] = e + cpx(0.0f, 1.0f) * o;
        }
        butterflies(work.data(), true);
        const float s = 1.0f / (float) h;
        for (int i = 0; i < h; ++i) {
            out[2 * i] = work[i].real() * s;
            out[2 * i + 1] = work[i].imag() * s;
        }
    }

private:
    void butterflies(cpx* d, bool inverse)
    {
        for (int len = 2; len <= h; len <<= 1) {
            const int half = len >> 1;
            const int step = h / len;
            for (int i = 0; i < h; i += len) {
                for (int j = 0; j < half; ++j) {
                    cpx w = tw[j * step];
                    if (inverse) w = std::conj(w);
                    cpx u = d[i + j];
                    cpx v = d[i + j + half] * w;
                    d[i + j] = u + v;
                    d[i + j + half] = u - v;
                }
            }
        }
    }

    int n = 0, h = 0;
    std::vector<int> bitrev;
    std::vector<cpx> tw, rtw, work;
};

} // namespace zd
