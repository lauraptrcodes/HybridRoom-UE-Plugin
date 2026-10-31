// test_order4.cpp — FDTD 4. Ordnung im Raum: Zeitschritt, Moden, Stabilität mit Wänden, Direktschall
#include "Baker.h"
#include "FDTDGrid.h"
#include "Hybrid.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>

using namespace fdtd;

TEST(Order4, CourantLimitAndTimeStep) {
    SimulationConfig c; c.fmax = 343; c.pointsPerLambda = 10; c.spatialOrder = 4;
    FDTDGrid g(c);
    const double lam = g.config().c * g.config().dt / g.config().dx;
    EXPECT_NEAR(lam, 0.99 * 0.5, 1e-12);                      // lambda <= 1/2 (von Neumann)
    EXPECT_NEAR(FDTDGrid::courantLimit(2), 1.0 / std::sqrt(3.0), 1e-12);
    EXPECT_EQ(g.config().spatialOrder, 4);
    c.spatialOrder = 3;                                       // ungültig -> 2
    EXPECT_EQ(FDTDGrid(c).config().spatialOrder, 2);
}

// Schallharter Quader auf exaktem Gitter: Moden 150..230 Hz mit < 0.3 % Abweichung
TEST(Order4, ShoeboxModes) {
    SimulationConfig cfg; cfg.Lx = 5; cfg.Ly = 4; cfg.Lz = 3; cfg.fmax = 343; cfg.pointsPerLambda = 10;
    cfg.spatialOrder = 4;
    SimulationConfig d;
    const auto h = simulatePoint(cfg, { 0.15, 0.15, 0.15 }, { 4.85, 3.85, 2.85 }, 2.0, 0.0, &d,
        testutil::pool().asParallelFor());
    double df;
    const auto mag = testutil::magnitudeSpectrum(h, 1 << 17, 1.0 / d.dt, df);
    // isolierte Moden (Abstand > 1.5 Hz zu Nachbarn), analytisch c/2 sqrt(sum (n/L)^2)
    const double modes[] = { 156.63, 161.80, 164.71, 188.13, 196.59, 198.10 };
    for (double fm : modes) {
        const double fn = testutil::peakNear(mag, df, fm, 0.8);
        EXPECT_LT(std::fabs(fn - fm) / fm, 0.003) << fm << " Hz -> " << fn << " Hz";
    }
}

// Lange Simulation mit Innenwand, dünnem Spalt und Wandabsorption: keine Instabilität, Energie fällt
TEST(Order4, StableWithWallsAndAbsorption) {
    SimulationConfig cfg; cfg.Lx = 2.0; cfg.Ly = 1.5; cfg.Lz = 1.2; cfg.fmax = 400; cfg.spatialOrder = 4;
    FDTDGrid g(cfg);
    g.setParallelFor(testutil::pool().asParallelFor());
    g.setWallAbsorption(0.2);
    g.setWallRegion(Vec3{ 0.9, 0.0, 0.0 }, Vec3{ 1.0, 1.2, 1.2 });     // Wand mit Öffnung bei y > 1.2
    g.setWallRegion(Vec3{ 0.3, 0.6, 0.0 }, Vec3{ 0.35, 0.65, 1.2 });   // dünner Pfosten (eine Zelle)
    const auto& c = g.config();
    const int src = g.airIndexOf(Vec3{ 0.4, 0.4, 0.6 });
    auto energy = [&]() {
        double e = 0;
        for (int i = 1; i < c.Nx - 1; ++i) for (int j = 1; j < c.Ny - 1; ++j) for (int k = 1; k < c.Nz - 1; ++k) {
            const double p = g.pressureAt(g.index(i, j, k)); e += p * p;
        }
        return e;
        };
    double e1 = 0, eEnd = 0;
    for (int n = 0; n < 12000; ++n) {
        g.injectSource(src, sourcePulse(g.time(), c.fmax));
        g.step();
        if (n == 1000) e1 = energy();
    }
    eEnd = energy();
    ASSERT_TRUE(std::isfinite(eEnd));
    EXPECT_LT(eEnd, 0.5 * e1);   // absorbierende Wände -> Energie nimmt ab
}

// Freifeld-Entzerrung gilt auch für 4. Ordnung (gleiche Quellen-Äquivalenz dx^3 / dt^2)
TEST(Order4, UnitIrDirectSound) {
    SimulationConfig cfg; cfg.Lx = 10; cfg.Ly = 10; cfg.Lz = 8; cfg.fmax = 200.0; cfg.spatialOrder = 4;
    BakeSettings bs; bs.source = Vec3{ 5.0, 5.0, 4.0 }; bs.irLength = 0.05; bs.receiverSpacing = 1.0;
    BakeStats st;
    const IrGrid g = bake(cfg, bs, &st, testutil::pool().asParallelFor());
    const double dx = st.dx;
    auto snap = [&](double v) { return (std::round(v / dx + 0.5) - 0.5) * dx; };
    const int r = g.nearest(6.5f, 5.5f, 4.5f);
    float x, y, z; g.position(r, x, y, z);
    const double d = std::sqrt(std::pow(snap(x) - snap(5.0), 2) + std::pow(snap(y) - snap(5.0), 2) + std::pow(snap(z) - snap(4.0), 2));
    const double P = 0.005;
    const auto h = fdtdToUnitIr(g.ir(r), g.length, g.sampleRate, cfg.fmax, dx, st.dt, cfg.c, st.peak, d, P);
    int imax = 0;
    for (int n = 0; n < g.length; ++n) if (std::fabs(h[n]) > std::fabs(h[imax])) imax = n;
    EXPECT_NEAR(imax / (double)g.sampleRate, P, 2e-4);
    const int nEnd = (int)((P + 0.012) * g.sampleRate);
    for (double f : { 60.0, 100.0, 150.0 }) {
        std::complex<double> H = 0;
        for (int n = 0; n < nEnd; ++n) H += (double)h[n] * std::polar(1.0, -2.0 * 3.141592653589793 * f * n / g.sampleRate);
        EXPECT_NEAR(20 * std::log10(std::abs(H)), 0.0, 1.5) << f << " Hz";
    }
}
