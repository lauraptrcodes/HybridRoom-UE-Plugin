// Raummoden eines schallharten Quaders: Spektrumsspitzen vs. analytische Eigenfrequenzen
//   f(nx,ny,nz) = c/2 * sqrt((nx/Lx)^2 + (ny/Ly)^2 + (nz/Lz)^2)
#include "Baker.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace fdtd;

namespace {
std::vector<double> analyticModes(double Lx, double Ly, double Lz, double c, double fMax) {
    std::vector<double> f;
    for (int a = 0; a < 10; ++a) for (int b = 0; b < 10; ++b) for (int d = 0; d < 10; ++d) {
        if (a + b + d == 0) continue;
        const double v = 0.5 * c * std::sqrt(std::pow(a / Lx, 2) + std::pow(b / Ly, 2) + std::pow(d / Lz, 2));
        if (v < fMax) f.push_back(v);
    }
    std::sort(f.begin(), f.end());
    return f;
}
} // namespace

// fmax = 343 Hz -> dx = 0.1 m teilt 5/4/3 m exakt: Geometriefehler 0, nur numerische Dispersion
TEST(Modes, ShoeboxRigidExactGrid) {
    SimulationConfig cfg; cfg.Lx = 5; cfg.Ly = 4; cfg.Lz = 3; cfg.fmax = 343; cfg.pointsPerLambda = 10;
    SimulationConfig d;
    // Quelle und Empfänger in gegenüberliegenden Ecken -> alle Moden angeregt und hörbar
    const auto h = simulatePoint(cfg, { 0.15, 0.15, 0.15 }, { 4.85, 3.85, 2.85 }, 2.0, 0.0, &d,
                                 testutil::pool().asParallelFor());
    double df;
    const auto mag = testutil::magnitudeSpectrum(h, 1 << 16, 1.0 / d.dt, df);

    // Nur isolierte Moden prüfen (kein Nachbar näher als 2 Hz), damit Spitzen eindeutig zuordenbar sind
    const auto modes = analyticModes(cfg.Lx, cfg.Ly, cfg.Lz, cfg.c, 100.0);
    int checked = 0;
    double worst = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        const bool isolated = (i == 0 || modes[i] - modes[i - 1] > 2.0) &&
                              (i + 1 == modes.size() || modes[i + 1] - modes[i] > 2.0);
        if (!isolated) continue;
        const double fNum = testutil::peakNear(mag, df, modes[i], 0.8);
        const double rel = std::fabs(fNum - modes[i]) / modes[i];
        worst = std::max(worst, rel);
        std::printf("  Mode %6.2f Hz -> FDTD %6.2f Hz (%.3f %%)\n", modes[i], fNum, 100 * rel);
        EXPECT_LT(rel, 0.003) << "analytisch " << modes[i] << " Hz, FDTD " << fNum << " Hz";
        ++checked;
    }
    EXPECT_GE(checked, 8);
    std::printf("  %d Moden geprüft, max. Abweichung %.3f %%\n", checked, 100 * worst);
}

// Nicht exakt teilbare Raummaße: Abweichung muss innerhalb des Geometriefehlers dx/(2L) + Dispersion bleiben
TEST(Modes, ShoeboxRigidGeneralGrid) {
    SimulationConfig cfg; cfg.Lx = 5; cfg.Ly = 4; cfg.Lz = 3; cfg.fmax = 400; cfg.pointsPerLambda = 10;
    SimulationConfig d;
    const auto h = simulatePoint(cfg, { 0.1, 0.1, 0.1 }, { 4.9, 3.9, 2.9 }, 2.0, 0.0, &d,
                                 testutil::pool().asParallelFor());
    double df;
    const auto mag = testutil::magnitudeSpectrum(h, 1 << 16, 1.0 / d.dt, df);

    // Erste axiale Moden: f = c/(2 L_eff), L_eff = (N-2) dx
    const double LxEff = (d.Nx - 2) * d.dx, LyEff = (d.Ny - 2) * d.dx;
    const double fx = 343.0 / (2 * LxEff), fy = 343.0 / (2 * LyEff);
    EXPECT_NEAR(testutil::peakNear(mag, df, fx, 0.8), fx, 0.003 * fx);
    EXPECT_NEAR(testutil::peakNear(mag, df, fy, 0.8), fy, 0.003 * fy);
    // ... und L_eff liegt höchstens dx/2 neben L
    EXPECT_LE(std::fabs(LxEff - cfg.Lx), 0.5 * d.dx + 1e-12);
    EXPECT_LE(std::fabs(LyEff - cfg.Ly), 0.5 * d.dx + 1e-12);
}
