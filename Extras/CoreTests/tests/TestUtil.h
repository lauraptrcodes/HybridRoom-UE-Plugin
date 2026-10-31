// TestUtil.h — gemeinsame Hilfen für die Tests
#pragma once
#include "Convolver.h"
#include "ThreadPool.h"
#include <cmath>
#include <vector>

namespace testutil {

// Ein Pool für alle Tests (Thread-Erzeugung nur einmal)
inline ThreadPool& pool() { static ThreadPool p; return p; }

// Theoretische Nachhallzeiten eines Quaders mit überall gleichem alpha
inline double sabine(double Lx, double Ly, double Lz, double alpha) {
    const double V = Lx * Ly * Lz, S = 2 * (Lx * Ly + Lx * Lz + Ly * Lz);
    return 0.161 * V / (alpha * S);
}
inline double eyring(double Lx, double Ly, double Lz, double alpha) {
    const double V = Lx * Ly * Lz, S = 2 * (Lx * Ly + Lx * Lz + Ly * Lz);
    return 0.161 * V / (-S * std::log(1.0 - alpha));
}

// Betragsspektrum mit Zero-Padding auf Zweierpotenz >= minLen
inline std::vector<double> magnitudeSpectrum(const std::vector<double>& x, int minLen, double fs, double& df) {
    int N = 1; while (N < minLen) N <<= 1;
    std::vector<fdtd::cf> buf(N, fdtd::cf(0.f, 0.f));
    for (size_t i = 0; i < x.size() && (int)i < N; ++i) buf[i] = fdtd::cf((float)x[i], 0.f);
    fdtd::Fft(N).forward(buf.data());
    std::vector<double> mag(N / 2);
    for (int k = 0; k < N / 2; ++k) mag[k] = std::abs(buf[k]);
    df = fs / N;
    return mag;
}

// Größtes Maximum im Bereich [f0 - halfWidth, f0 + halfWidth], parabolisch interpoliert
inline double peakNear(const std::vector<double>& mag, double df, double f0, double halfWidth) {
    const int k0 = std::max(1, (int)std::floor((f0 - halfWidth) / df));
    const int k1 = std::min((int)mag.size() - 2, (int)std::ceil((f0 + halfWidth) / df));
    int best = k0;
    for (int k = k0; k <= k1; ++k) if (mag[k] > mag[best]) best = k;
    const double a = std::log(mag[best - 1] + 1e-30), b = std::log(mag[best] + 1e-30), c = std::log(mag[best + 1] + 1e-30);
    const double denom = a - 2 * b + c;
    const double off = std::fabs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
    return (best + off) * df;
}

} // namespace testutil
