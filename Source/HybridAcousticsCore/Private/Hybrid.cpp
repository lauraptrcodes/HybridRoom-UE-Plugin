
#include <algorithm>
#include <cmath>
#include <random>

#include "Hybrid.h"
#include "Constants.h"
#include "Convolver.h"

namespace fdtd {

    // Biquad, transponierte Direktform II
    namespace {
        struct Biquad {
            double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            double process(double x) {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };

        // Butterworth 2. Ordnung (RBJ-Cookbook, Q = 1/sqrt(2))
        Biquad butterworth(double fc, int fs, bool highpass) {
            const double w0 = 2.0 * kPi * fc / fs;
            const double cw = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
            const double a0 = 1.0 + alpha;
            Biquad q;
            if (highpass) { q.b0 = (1 + cw) / 2; q.b1 = -(1 + cw); q.b2 = (1 + cw) / 2; }
            else { q.b0 = (1 - cw) / 2; q.b1 = (1 - cw); q.b2 = (1 - cw) / 2; }
            q.a1 = -2.0 * cw;
            q.a2 = 1.0 - alpha;
            q.b0 /= a0; q.b1 /= a0; q.b2 /= a0; q.a1 /= a0; q.a2 /= a0;
            return q;
        }

        // Linkwitz-Riley 4. Ordnung = zwei Butterworth-Stufen hintereinander;
        // Tief- und Hochpass addieren sich zu flachem Betragsfrequenzgang
        void linkwitzRiley4(std::vector<float>& x, double fc, int fs, bool highpass) {
            Biquad s1 = butterworth(fc, fs, highpass), s2 = butterworth(fc, fs, highpass);
            for (float& v : x) v = (float)s2.process(s1.process(v));
        }

        double rms(const std::vector<float>& x, int a, int b) {
            a = std::max(a, 0);
            b = std::min(b, (int)x.size());
            if (b <= a) return 0.0;
            double e = 0.0;
            for (int i = a; i < b; ++i) e += (double)x[i] * x[i];
            return std::sqrt(e / (b - a));
        }
    }
    

    float estimateT60(const float* ir, int length, int fs) {
        if (length < 2) return -1.f;
        std::vector<double> E(length);
        double acc = 0.0;
        for (int i = length - 1; i >= 0; --i) { acc += (double)ir[i] * ir[i]; E[i] = acc; }
        if (acc <= 0.0) return -1.f;

        int i5 = -1, i25 = -1;
        for (int i = 0; i < length; ++i) {
            const double L = 10.0 * std::log10(E[i] / E[0] + 1e-30);
            if (i5 < 0 && L < -5.0) i5 = i;
            if (L < -25.0) { i25 = i; break; }
        }
        if (i5 < 0 || i25 <= i5 + 1) return -1.f;

        // lineare Regression L(t) = k*t + b
        double st = 0, sL = 0, stt = 0, stL = 0;
        const int n = i25 - i5;
        for (int i = i5; i < i25; ++i) {
            const double t = (double)i / fs;
            const double L = 10.0 * std::log10(E[i] / E[0] + 1e-30);
            st += t; sL += L; stt += t * t; stL += t * L;
        }
        const double kSlope = (n * stL - st * sL) / (n * stt - st * st);   // dB/s
        return kSlope < 0.0 ? (float)(-60.0 / kSlope) : -1.f;
    }

    double schroederFrequency(double T60, double volume)
    {
        if (T60 <= 0.0 || volume <= 0.0) return 0.0;
        return 2000.0 * std::sqrt(T60 / volume);    // f_S = 2000 sqrt(T60 / V), T60 in s, V in m^3
    }

    std::vector<float> makeHybrid(const float* lowIr, int length, int fs, float fc, float T60,
        float directDelaySec, float tailGain, unsigned seed) {
        // 1) Tiefton: FDTD-IR, LR4-Tiefpass bei fc
        std::vector<float> low(lowIr, lowIr + length);
        linkwitzRiley4(low, fc, fs, false);

        // 2) Hochton: Rauschen * exp(-3 ln10 t / T60) ab Direktschall, LR4-Hochpass bei fc
        std::vector<float> tail(length, 0.f);
        std::mt19937 rng(seed);
        std::normal_distribution<float> nd(0.f, 1.f);
        const int n0 = std::clamp((int)(directDelaySec * fs), 0, length);
        const double k = 3.0 * std::log(10.0) / T60;
        for (int n = n0; n < length; ++n)
            tail[n] = nd(rng) * (float)std::exp(-k * (n - n0) / fs);
        linkwitzRiley4(tail, fc, fs, true);

        // 3) Pegelanpassung (Heuristik): gleicher RMS im Fenster 20..100 ms nach Direktschall
        const int wa = n0 + fs / 50, wb = n0 + fs / 10;
        const double rTail = rms(tail, wa, wb);
        const float g = rTail > 1e-12 ? (float)(tailGain * rms(low, wa, wb) / rTail) : 0.f;

        for (int n = 0; n < length; ++n) low[n] += g * tail[n];
        return low;
    }

    void applyDecay(float* ir, int length, int fs, float T60) {
        if (T60 <= 0.f) return;
        const double k = 3.0 * std::log(10.0) / T60;     // = sigma/2
        for (int n = 0; n < length; ++n)
            ir[n] *= (float)std::exp(-k * n / fs);
    }
    FDTD_API std::vector<float> makeHybridGeo(const float* lowIr, int lowLength, const float* highIr, int highLength, int fs, float fc, float fmaxLow, float directDelaySec, float* gainOut)
    {
        const int length = lowLength;
        std::vector<float> geo(length, 0.f);                     // auf FDTD-Länge kürzen bzw. mit Nullen auffüllen
        std::copy(highIr, highIr + std::min(lowLength, highLength), geo.begin());
        
                // Steam Audio rekonstruiert aus 10-ms-Energiebins -> Rauschen auch vor der ersten möglichen Reflexion.
                // Physikalisch kann vor dem Direktschall nichts ankommen: bis dahin stumm, danach 1 ms Halb-Hann-Einblendung.
        const int n0 = std::clamp((int)(directDelaySec * fs), 0, length);
        const int ramp = std::max(1, fs / 1000);
        for (int n = 0; n < std::min(n0 + ramp, length); ++n)
            geo[n] *= n < n0 ? 0.f : (float)(0.5 - 0.5 * std::cos(kPi * (n - n0) / ramp));
        
        // 1) Pegel im Überlappband [fc, fmaxLow] vergleichen
        std::vector<float> bl(lowIr, lowIr + length), bh = geo;
        linkwitzRiley4(bl, fc, fs, true);  linkwitzRiley4(bl, fmaxLow, fs, false);
        linkwitzRiley4(bh, fc, fs, true);  linkwitzRiley4(bh, fmaxLow, fs, false);
        const int wa = n0 + fs / 50, wb = n0 + fs / 5;          // +20 ms .. +200 ms
        const double rh = rms(bh, wa, wb);
        const float g = rh > 1e-20 ? (float)(rms(bl, wa, wb) / rh) : 0.f;
        if (gainOut) *gainOut = g;
        // 2) Weiche: LR4-Tiefpass + LR4-Hochpass addieren sich zu flachem Betragsgang
        std::vector<float> low(lowIr, lowIr + length), high = std::move(geo);
        linkwitzRiley4(low, fc, fs, false);
        linkwitzRiley4(high, fc, fs, true);
        for (int n = 0; n < length; ++n) low[n] += g * high[n];
        return low;
    }

    double eyringT60(double Lx, double Ly, double Lz, double alpha, double c) {
        if (alpha <= 0.0) return -1.0;
        const double V = Lx * Ly * Lz;
        const double S = 2.0 * (Lx * Ly + Lx * Lz + Ly * Lz);
        return 24.0 * std::log(10.0) * V / (c * S * -std::log(1.0 - std::min(alpha, 0.999)));
        
    }
    
    float medianBandT60(const float* irs, int numIrs, int length, int fs, float fLo, float fHi, int stride) {
       if (stride <= 0) stride = length;
       std::vector<float> t60s;
       std::vector<float> x;
       for (int r = 0; r < numIrs; ++r) {
            x.assign(irs + (size_t)r * stride, irs + (size_t)r * stride + length);
            linkwitzRiley4(x, fLo, fs, true);
            linkwitzRiley4(x, fHi, fs, false);
            const float t = estimateT60(x.data(), length, fs);
            if (t > 0.f) t60s.push_back(t);
            
        }
        if (t60s.empty()) return -1.f;
        const size_t mid = t60s.size() / 2;                       // Median: robust gegen einzelne Ausreißer
        std::nth_element(t60s.begin(), t60s.begin() + (long)mid, t60s.end());
        return t60s[mid];
        
    }

    std::vector<float> fdtdToUnitIr(const float* ir, int length, int fs, double fmax, double dx, double dt,
        double c, double peak, double dist, double preDelaySec, double fLow) {
        std::vector<float> out(length, 0.f);
        if (length <= 0 || peak <= 0.0 || dist <= 0.0) return out;
        
        int N = 1;
        while (N < 2 * length) N <<= 1;                           // Nullen gegen zyklische Überlappung
        std::vector<cf> X(N, cf(0.f, 0.f));
        for (int n = 0; n < length; ++n) X[n] = cf(ir[n], 0.f);
        const Fft fft(N);
        fft.forward(X.data());
        
         // Pulsspektrum: s(t) = -u e^{-u^2}, u = (t - t0)/tau  ->  S(f) = i pi^1.5 tau^2 f e^{-(pi f tau)^2} e^{-i 2 pi f t0}
        const double tau = std::sqrt(std::log(10.0)) / (kPi * fmax);
        const double t0 = 4.0 * tau;
        // Skalierung: X[k] ~ fs * (K / r) S(f),  K = dx^3 / (4 pi c^2 dt^2) / peak  ->  Einheitsimpuls: * r / (K fs)
        const double scale = dist * peak * 4.0 * kPi * c * c * dt * dt / (dx * dx * dx * (double)fs);
        // Im Bake-IR liegt die Pulsmitte bei r/c + t0 - dt (Aufnahme n gilt als Zeit (n+1) dt, der Puls wirkt einen
            // Schritt später, gemessen gegen die analytische Lösung) -> nach Division durch S bei r/c - dt
        const double shift = dist / c - dt - preDelaySec;
        const double fTop = 1.3 * fmax;
        
        for (int k = 0; k < N; ++k) {
            const double f = (k <= N / 2 ? k : k - N) * (double)fs / N;   // vorzeichenbehaftet -> hermitesch
            const double af = std::fabs(f);
            if (af < 1e-9) { X[k] = cf(0.f, 0.f); continue; }
            const double hp = std::pow(af / fLow, 4.0) / (1.0 + std::pow(af / fLow, 4.0));
            const double top = std::exp(-std::pow(af / fTop, 8.0));
            const double mag = kPi * std::sqrt(kPi) * tau * tau * f * std::exp(-(kPi * f * tau) * (kPi * f * tau));
            if (hp * top < 1e-9) { X[k] = cf(0.f, 0.f); continue; }
                    // 1/S = -i e^{+i 2 pi f t0} / mag ; dazu Verschiebung e^{+i 2 pi f shift}
            const double ph = 2.0 * kPi * f * (t0 + shift);
            const std::complex<double> H = std::complex<double>(0.0, -1.0) * std::polar(1.0, ph) * (scale * hp * top / mag);
            X[k] = cf((float)(X[k].real() * H.real() - X[k].imag() * H.imag()),
            (float)(X[k].real() * H.imag() + X[k].imag() * H.real()));
        
        }
        fft.inverse(X.data());
        const float invN = 1.f / (float)N;
        for (int n = 0; n < length; ++n) out[n] = X[n].real() * invN;
        return out;
        
    }
}