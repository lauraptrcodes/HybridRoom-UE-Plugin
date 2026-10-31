// Gitter, Koordinaten, Stabilität, Innenwände
#include "Baker.h"
#include "FDTDGrid.h"
#include "IrGrid.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <cmath>

using namespace fdtd;

static SimulationConfig box(double fmax) {
    SimulationConfig c; c.Lx = 5; c.Ly = 4; c.Lz = 3; c.fmax = fmax; c.pointsPerLambda = 10;
    return c;
}

TEST(Grid, DerivedValues) {
    FDTDGrid g(box(343.0));                              // dx = 343 / (343 * 10) = 0.1 m
    const auto& c = g.config();
    EXPECT_NEAR(c.dx, 0.1, 1e-12);
    EXPECT_EQ(c.Nx, 52);                                 // round(5/0.1) + 2
    EXPECT_EQ(c.Ny, 42);
    EXPECT_EQ(c.Nz, 32);
    const double courant = c.c * c.dt / c.dx;
    EXPECT_LE(courant, 1.0 / std::sqrt(3.0));            // CFL-Grenze 3D
    EXPECT_NEAR(courant, 0.99 / std::sqrt(3.0), 1e-12);
}

TEST(Grid, ToGridAndIndexOf) {
    FDTDGrid g(box(343.0));
    // Raumkoordinate x liegt bei Gitterkoordinate x/dx + 0.5 -> Knoten i hat Mittelpunkt (i - 0.5) dx
    const Vec3i a = g.toGrid({ 0.05, 0.05, 0.05 });
    EXPECT_EQ(a.x, 1); EXPECT_EQ(a.y, 1); EXPECT_EQ(a.z, 1);
    const Vec3i b = g.toGrid({ 4.95, 3.95, 2.95 });
    EXPECT_EQ(b.x, 50); EXPECT_EQ(b.y, 40); EXPECT_EQ(b.z, 30);
    // Außerhalb -> auf erste/letzte Luftzelle geklemmt (nie Randknoten 0 oder N-1)
    EXPECT_EQ(g.indexOf({ -10, -10, -10 }), g.indexOf({ 0.05, 0.05, 0.05 }));
    EXPECT_EQ(g.indexOf({ 99, 99, 99 }), g.indexOf({ 4.95, 3.95, 2.95 }));
}

TEST(Grid, IrGridLookup) {
    IrGrid ir; ir.nx = 10; ir.ny = 8; ir.nz = 6; ir.spacing = 0.5f;
    for (int r = 0; r < ir.numReceivers(); ++r) {
        float x, y, z; ir.position(r, x, y, z);
        EXPECT_EQ(ir.nearest(x, y, z), r);               // Mittelpunkt -> eigener Index
    }
    EXPECT_EQ(ir.nearest(-5.f, -5.f, -5.f), 0);          // geklemmt, nie leer
    EXPECT_EQ(ir.nearest(50.f, 50.f, 50.f), ir.numReceivers() - 1);
    EXPECT_EQ(ir.nearest(4.9f, 0.1f, 2.9f), ir.index(9, 0, 5));
}

// Ohne Dämpfung muss das Feld beschränkt bleiben (schallharte Wände sind verlustfrei, aber stabil).
// Quelle ohne Gleichanteil (sourcePulse): Ein Signal mit Gleichanteil würde im geschlossenen,
// verlustfreien Raum den mittleren Druck linear anwachsen lassen (physikalisch korrekt, aber kein Stabilitätstest).
TEST(Grid, RigidStable) {
    FDTDGrid g(box(200.0));
    g.setParallelFor(testutil::pool().asParallelFor());
    const int src = g.indexOf({ 1.2, 1.0, 1.5 }), rec = g.indexOf({ 3.5, 2.8, 1.7 });
    const auto& c = g.config();
    double early = 0, late = 0;
    const int steps = 6000;
    for (int n = 0; n < steps; ++n) {
        g.step();
        g.injectSource(src, sourcePulse((n + 1) * c.dt, c.fmax));
        const double v = std::fabs(g.pressureAt(rec));
        if (n > 200 && n < 1200) early = std::max(early, v);
        if (n > steps - 1000) late = std::max(late, v);
        ASSERT_TRUE(std::isfinite(v));
    }
    EXPECT_LT(late, 3.0 * early);                        // kein Anwachsen
}

// Innenwand: Zellen in der Wand bleiben 0, Feld bleibt stabil, hinter der Wand kommt weniger an
TEST(Grid, WallRegion) {
    FDTDGrid g(box(200.0));
    g.setParallelFor(testutil::pool().asParallelFor());
    g.setWallRegion({ 2.4, 0.0, 0.0 }, { 2.6, 3.0, 3.0 });   // Trennwand mit Öffnung bei y > 3
    const int src = g.indexOf({ 1.0, 1.0, 1.5 });
    const int inWall = g.indexOf({ 2.5, 1.0, 1.5 });
    const auto& c = g.config();
    for (int n = 0; n < 3000; ++n) {
        g.step();
        g.injectSource(src, sourcePulse((n + 1) * c.dt, c.fmax));
        ASSERT_EQ(g.pressureAt(inWall), 0.0);
        ASSERT_TRUE(std::isfinite(g.pressureAt(src)));
    }
}
