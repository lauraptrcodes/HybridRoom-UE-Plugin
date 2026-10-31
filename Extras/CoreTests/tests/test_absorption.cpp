// Nachhallzeit: kalibrierte Wandabsorption vs. Sabine/Eyring, globale Dämpfung vs. Ziel-T60
#include "Baker.h"
#include "Hybrid.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>

using namespace fdtd;

namespace {
double measureT60(const std::vector<double>& h, double dt) {
    std::vector<float> f(h.begin(), h.end());
    return estimateT60(f.data(), (int)f.size(), (int)std::lround(1.0 / dt));
}
SimulationConfig box() {
    SimulationConfig c; c.Lx = 5; c.Ly = 4; c.Lz = 3; c.fmax = 300; c.pointsPerLambda = 10;
    return c;
}
} // namespace

// T60 muss zwischen Eyring (untere Schranke) und Sabine (obere) liegen, mit Toleranz für das
// nicht-diffuse Schallfeld bei tiefen Frequenzen; außerdem streng fallend in alpha
TEST(Absorption, T60FollowsSabineEyring) {
    const double alphas[] = { 0.1, 0.2, 0.4 };
    double prev = 1e9;
    for (double a : alphas) {
        SimulationConfig d;
        const auto h = simulatePoint(box(), { 1.2, 1.0, 1.5 }, { 3.5, 2.8, 1.7 }, 1.2, a, &d,
                                     testutil::pool().asParallelFor());
        const double t60 = measureT60(h, d.dt);
        const double sab = testutil::sabine(5, 4, 3, a), eyr = testutil::eyring(5, 4, 3, a);
        std::printf("  alpha %.2f: FDTD %.3f s | Sabine %.3f s | Eyring %.3f s\n", a, t60, sab, eyr);
        EXPECT_GT(t60, 0.8 * eyr);
        EXPECT_LT(t60, 1.35 * sab);
        EXPECT_LT(t60, prev);
        prev = t60;
    }
}

// Starre Wände + globale Dämpfung: Abklingen exakt vorgegeben
TEST(Absorption, GlobalDampingMatchesTarget) {
    FDTDGrid g(box());
    g.setParallelFor(testutil::pool().asParallelFor());
    g.setGlobalDamping(0.6);
    const auto& c = g.config();
    const int src = g.indexOf({ 1.2, 1.0, 1.5 }), rec = g.indexOf({ 3.5, 2.8, 1.7 });
    std::vector<double> h((size_t)std::ceil(0.8 / c.dt));
    for (size_t n = 0; n < h.size(); ++n) {
        g.step();
        g.injectSource(src, sourcePulse((n + 1) * c.dt, c.fmax));
        h[n] = g.pressureAt(rec);
    }
    const double t60 = measureT60(h, c.dt);
    std::printf("  globale Dämpfung: Ziel 0.600 s, gemessen %.3f s\n", t60);
    EXPECT_NEAR(t60, 0.6, 0.06);
}

// Ohne Absorption darf (praktisch) nichts abklingen
TEST(Absorption, RigidDoesNotDecay) {
    SimulationConfig d;
    const auto h = simulatePoint(box(), { 1.2, 1.0, 1.5 }, { 3.5, 2.8, 1.7 }, 0.6, 0.0, &d,
                                 testutil::pool().asParallelFor());
    auto energy = [&](size_t a, size_t b) { double e = 0; for (size_t i = a; i < b; ++i) e += h[i] * h[i]; return e; };
    const size_t q = h.size() / 4;
    const double ratioDb = 10 * std::log10(energy(3 * q, 4 * q) / energy(q, 2 * q));
    std::printf("  starr: Energie spätes/frühes Viertel %.2f dB\n", ratioDb);
    EXPECT_GT(ratioDb, -3.0);
}
