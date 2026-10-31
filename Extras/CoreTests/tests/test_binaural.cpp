// test_binaural.cpp — Steam-Audio-HRTF: Richtungen kommen auf dem richtigen Ohr an
#include "Binaural.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace fdtd;

#if HAC_WITH_STEAMAUDIO
namespace {
    // Pegeldifferenz rechts - links (dB) für weißes Rauschen aus Richtung d (Unreal-Hörerraum)
    double ildDb(BinauralEffect& e, const Vec3& d, int frame) {
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0.f, 0.3f);
        std::vector<float> in(frame), l(frame), r(frame);
        double el = 0, er = 0;
        e.reset();
        for (int b = 0; b < 40; ++b) {
            for (float& v : in) v = nd(rng);
            EXPECT_TRUE(e.process(in.data(), l.data(), r.data(), frame, d));
            if (b < 4) continue;                                       // Einschwingen
            for (int i = 0; i < frame; ++i) { el += (double)l[i] * l[i]; er += (double)r[i] * r[i]; }
        }
        return 10.0 * std::log10(er / el);
    }
} // namespace

TEST(Binaural, DirectionsLandOnCorrectEar) {
    HrtfContext h;
    ASSERT_TRUE(h.create(48000, 512));
    BinauralEffect e;
    ASSERT_TRUE(e.create(h));
    const double right = ildDb(e, Vec3{ 0, 1, 0 }, 512);    // y = rechts in Unreal
    const double left = ildDb(e, Vec3{ 0, -1, 0 }, 512);
    const double front = ildDb(e, Vec3{ 1, 0, 0 }, 512);
    EXPECT_GT(right, 6.0);
    EXPECT_LT(left, -6.0);
    EXPECT_LT(std::fabs(front), 3.0);   // gemessene HRTF ist nicht exakt symmetrisch (~2 dB)
    printf("ILD rechts %+.1f dB, links %+.1f dB, vorn %+.1f dB\n", right, left, front);

    std::vector<float> in(256), l(256), r(256);
    EXPECT_FALSE(e.process(in.data(), l.data(), r.data(), 256, Vec3{ 1, 0, 0 }));   // falsche Blockgröße
}
#endif
