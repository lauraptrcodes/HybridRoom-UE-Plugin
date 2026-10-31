// test_hybridbake.cpp — Gesamtablauf, Schlüssel und Bake-Cache
#include "HybridBake.h"
#include "Hybrid.h"
#include "DirectPath.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>

using namespace fdtd;

namespace {
    HybridBakeInput smallInput(bool useGeo) {
        HybridBakeInput in;
        in.cfg.Lx = 2.0; in.cfg.Ly = 1.6; in.cfg.Lz = 1.2;
        in.settings.source = Vec3{ 0.5, 0.5, 0.6 };
        in.settings.irLength = 0.15;
        in.settings.receiverSpacing = 0.5;
        in.settings.wallAlpha = 0.2;
        in.geo.numRays = 1024;
        in.useGeo = useGeo;
        in.crossover.fcMax = 300.0;
        return in;
    }
} // namespace

TEST(HybridBake, KeyTracksInputsButNotThreads) {
    const HybridBakeInput a = smallInput(true);
    HybridBakeInput b = a;
    EXPECT_EQ(hashInput(a), hashInput(b));

    b.geo.numThreads = 16;                       // ändert das Ergebnis nicht
    EXPECT_EQ(hashInput(a), hashInput(b));

    b = a; b.settings.wallAlpha = 0.21;          EXPECT_NE(hashInput(a), hashInput(b));
    b = a; b.settings.source.x += 0.01;          EXPECT_NE(hashInput(a), hashInput(b));
    b = a; b.settings.innerWalls.push_back(Box{ Vec3{ 1, 0, 0 }, Vec3{ 1.1, 1, 1 } });
    EXPECT_NE(hashInput(a), hashInput(b));
    b = a; b.crossover.fmaxFactor = 2.5;         EXPECT_NE(hashInput(a), hashInput(b));
    b = a; b.useGeo = false;                     EXPECT_NE(hashInput(a), hashInput(b));
    b = a; b.cfg.fmax = 1234.0;                  EXPECT_EQ(hashInput(a), hashInput(b));   // fmax folgt aus fc
}

TEST(HybridBake, CrossoverFromEyringWithoutGeometry) {
    const HybridBakeInput in = smallInput(false);
    const HybridBakeResult r = runHybridBake(in, testutil::pool().asParallelFor());
    ASSERT_FALSE(r.aborted);
    EXPECT_EQ(r.t60Source, T60Source::Eyring);
    const double t60 = eyringT60(in.cfg.Lx, in.cfg.Ly, in.cfg.Lz, in.settings.wallAlpha);
    EXPECT_NEAR(r.t60, t60, 1e-4);
    const double fs = 2000.0 * std::sqrt(t60 / (in.cfg.Lx * in.cfg.Ly * in.cfg.Lz));
    EXPECT_NEAR(r.fs, fs, 1e-2);
    EXPECT_NEAR(r.fc, std::min(fs, in.crossover.fcMax), 1e-2);
    EXPECT_NEAR(r.cfg.fmax, r.fc * in.crossover.fmaxFactor, 1e-3);
    EXPECT_GT(r.grid.numReceivers(), 0);
    EXPECT_EQ(r.geo.numReceivers(), 0);
}

TEST(HybridBake, CacheRoundTripAndRejects) {
    HybridBakeInput in = smallInput(false);
    in.settings.innerWalls.push_back(Box{ Vec3{ 1.0, 0.0, 0.0 }, Vec3{ 1.1, 1.0, 1.2 } });
    const HybridBakeResult r = runHybridBake(in, testutil::pool().asParallelFor());
    const uint64_t key = hashInput(in);
    const std::vector<uint8_t> buf = serializeBake(key, r);

    HybridBakeResult l;
    ASSERT_TRUE(deserializeBake(buf.data(), buf.size(), key, l));
    EXPECT_TRUE(l.fromCache);
    EXPECT_EQ(l.grid.data, r.grid.data);
    EXPECT_EQ(l.grid.nx, r.grid.nx); EXPECT_EQ(l.grid.length, r.grid.length);
    EXPECT_EQ(l.cfg.Nx, r.cfg.Nx);   EXPECT_DOUBLE_EQ(l.cfg.dt, r.cfg.dt);
    EXPECT_FLOAT_EQ(l.fc, r.fc);     EXPECT_FLOAT_EQ(l.t60, r.t60);
    ASSERT_EQ(l.settings.innerWalls.size(), 1u);
    EXPECT_DOUBLE_EQ(l.settings.innerWalls[0].max.x, 1.1);

    HybridBakeResult x;
    EXPECT_FALSE(deserializeBake(buf.data(), buf.size(), key + 1, x));         // Eingaben geändert
    EXPECT_FALSE(deserializeBake(buf.data(), buf.size() - 100, key, x));       // abgeschnitten
    EXPECT_FALSE(deserializeBake(buf.data(), 10, key, x));
    std::vector<uint8_t> bad = buf; bad[0] ^= 0xFF;
    EXPECT_FALSE(deserializeBake(bad.data(), bad.size(), key, x));             // falsches Magic
}

#if HAC_WITH_STEAMAUDIO
TEST(HybridBake, WithSteamAudioCacheKeepsGeometricGrid) {
    const HybridBakeInput in = smallInput(true);
    const HybridBakeResult r = runHybridBake(in, testutil::pool().asParallelFor());
    ASSERT_FALSE(r.aborted);
    ASSERT_EQ(r.geo.numReceivers(), r.grid.numReceivers());
    EXPECT_EQ(r.t60Source, T60Source::Geometric);

    const uint64_t key = hashInput(in);
    const std::vector<uint8_t> buf = serializeBake(key, r);
    HybridBakeResult l;
    ASSERT_TRUE(deserializeBake(buf.data(), buf.size(), key, l));
    EXPECT_EQ(l.geo.data, r.geo.data);
    EXPECT_EQ(l.t60Source, T60Source::Geometric);
}
#endif

#if HAC_WITH_STEAMAUDIO
// Einfache Dekorrelation: Hochton links/rechts gleich laut, aber unkorreliert; Tiefton gemeinsam
TEST(HybridBake, StereoIrIsDecorrelated) {
    HybridBakeInput in = smallInput(true);
    in.settings.irLength = 0.4;
    const HybridBakeResult res = runHybridBake(in, testutil::pool().asParallelFor());
    const int r = res.grid.nearest(1.6f, 1.2f, 0.6f);
    StereoIrSet s;
    ASSERT_TRUE(receiverStereoIr(res, r, 0.005, s));
    ASSERT_EQ(s.hf[0].size(), s.low.size());

    // Energie je Ohr im Vergleich zum Hochton des Mono-Pfads
    std::vector<float> mono = receiverIr(res, r, 0.005);
    LinkwitzRiley4 hp; hp.setup(res.fc, res.grid.sampleRate, true);
    double em = 0, el = 0, er = 0;
    for (size_t i = 0; i < mono.size(); ++i) { const float m = hp.process(mono[i]); em += (double)m * m; }
    for (size_t i = 0; i < s.hf[0].size(); ++i) { el += (double)s.hf[0][i] * s.hf[0][i]; er += (double)s.hf[1][i] * s.hf[1][i]; }
    const double lvL = 10 * std::log10(el / em), lvR = 10 * std::log10(er / em);

    // Kohärenz über 1 kHz (normierte Kreuzkorrelation, Maximum über +-1 ms)
    LinkwitzRiley4 a, b; a.setup(1000.0, res.grid.sampleRate, true); b.setup(1000.0, res.grid.sampleRate, true);
    std::vector<double> l(s.hf[0].size()), rr(s.hf[1].size());
    double nl = 0, nr = 0;
    for (size_t i = 0; i < l.size(); ++i) { l[i] = a.process(s.hf[0][i]); rr[i] = b.process(s.hf[1][i]); nl += l[i] * l[i]; nr += rr[i] * rr[i]; }
    double iacc = 0;
    for (int lag = -48; lag <= 48; ++lag) {
        double sum = 0;
        for (size_t i = 48; i + 48 < l.size(); ++i) sum += l[i] * rr[i + lag];
        iacc = std::max(iacc, std::fabs(sum) / std::sqrt(nl * nr));
    }
    printf("Stereo-IR: Hochton links %+.1f dB, rechts %+.1f dB gg. Mono-Pfad, IACC > 1 kHz = %.2f\n", lvL, lvR, iacc);
    EXPECT_NEAR(lvL, 0.0, 1.0);
    EXPECT_NEAR(lvR, 0.0, 1.5);
    EXPECT_LT(iacc, 0.2);
}
#endif
