// Bake: Kenngrößen, Normierung, Abbruch, Übereinstimmung mit simulatePoint
#include "Baker.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

using namespace fdtd;

namespace {
SimulationConfig smallBox() {
    SimulationConfig c; c.Lx = 5; c.Ly = 4; c.Lz = 3; c.fmax = 200; c.pointsPerLambda = 10;
    return c;
}
} // namespace

TEST(Bake, StatsAndNormalization) {
    BakeSettings bs; bs.irLength = 0.3; bs.receiverSpacing = 0.5; bs.outRate = 48000;
    BakeStats st;
    const IrGrid g = bake(smallBox(), bs, &st, testutil::pool().asParallelFor());
    EXPECT_FALSE(st.aborted);
    EXPECT_EQ(st.receivers, 10 * 8 * 6);
    EXPECT_EQ(g.numReceivers(), 480);
    EXPECT_EQ(g.sampleRate, 48000);
    EXPECT_EQ(g.length, (int)std::floor(st.steps * st.dt * 48000));
    float mx = 0; for (float v : g.data) { ASSERT_TRUE(std::isfinite(v)); mx = std::max(mx, std::fabs(v)); }
    EXPECT_NEAR(mx, 1.0f, 1e-6f);                         // global peak-normiert
    EXPECT_LE(st.courant, 1.0 / std::sqrt(3.0));
}

TEST(Bake, AbortReturnsEmpty) {
    BakeSettings bs; bs.irLength = 0.5;
    BakeStats st;
    int calls = 0;
    const IrGrid g = bake(smallBox(), bs, &st, testutil::pool().asParallelFor(),
                          [&](float) { return ++calls < 3; });
    EXPECT_TRUE(st.aborted);
    EXPECT_EQ(g.numReceivers(), 0);
    EXPECT_EQ(calls, 3);
}

// Die gebakte IR eines Empfängers muss (bis auf Normierung und Resampling) simulatePoint entsprechen
TEST(Bake, MatchesSimulatePoint) {
    BakeSettings bs; bs.irLength = 0.3; bs.source = { 1.2, 1.0, 1.5 };
    BakeStats st;
    const IrGrid g = bake(smallBox(), bs, &st, testutil::pool().asParallelFor());
    const int r = g.nearest(3.5f, 2.8f, 1.7f);
    float x, y, z; g.position(r, x, y, z);

    SimulationConfig d;
    const auto h = simulatePoint(smallBox(), bs.source, { x, y, z }, bs.irLength, 0.0, &d);
    // simulatePoint wie im Bake auf 48 kHz interpolieren
    std::vector<double> up(g.length);
    for (int m = 0; m < g.length; ++m) {
        const double pos = (double)m / 48000 / d.dt - 1.0;
        const int n0 = (int)std::floor(pos); const double f = pos - n0;
        const double v0 = (n0 >= 0 && n0 < (int)h.size()) ? h[n0] : 0.0;
        const double v1 = (n0 + 1 >= 0 && n0 + 1 < (int)h.size()) ? h[n0 + 1] : 0.0;
        up[m] = (1 - f) * v0 + f * v1;
    }
    double ab = 0, aa = 0, bb = 0;
    for (int m = 0; m < g.length; ++m) { const double a = g.ir(r)[m]; ab += a * up[m]; aa += a * a; bb += up[m] * up[m]; }
    EXPECT_GT(ab / std::sqrt(aa * bb), 0.9999);
}
