// Hybrid-Hilfsfunktionen: T60-Schätzung, Abklingen, Schroeder-Frequenz, makeHybrid
#include "Hybrid.h"
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

using namespace fdtd;

namespace {
std::vector<float> decayingNoise(double T60, int fs, double seconds, unsigned seed = 3) {
    std::mt19937 rng(seed); std::normal_distribution<float> nd;
    std::vector<float> h((size_t)(seconds * fs));
    const double k = 3.0 * std::log(10.0) / T60;
    for (size_t n = 0; n < h.size(); ++n) h[n] = nd(rng) * (float)std::exp(-k * n / fs);
    return h;
}
} // namespace

TEST(Hybrid, SchroederFrequency) {
    EXPECT_NEAR(schroederFrequency(0.6, 60.0), 200.0, 1e-9);   // 2000 * sqrt(0.6/60)
}

TEST(Hybrid, EstimateT60OnSyntheticDecay) {
    for (double T : { 0.3, 0.6, 1.2 }) {
        const auto h = decayingNoise(T, 48000, 2.0 * T + 0.2);
        EXPECT_NEAR(estimateT60(h.data(), (int)h.size(), 48000), T, 0.03 * T);
    }
}

TEST(Hybrid, EstimateT60FailsOnSilence) {
    std::vector<float> zero(4800, 0.f);
    EXPECT_LT(estimateT60(zero.data(), (int)zero.size(), 48000), 0.f);
}

// Zwei Abklingvorgänge addieren sich in den Raten: 1/T = 1/T1 + 1/T2
TEST(Hybrid, ApplyDecayCombinesRates) {
    auto h = decayingNoise(0.6, 48000, 1.0);
    applyDecay(h.data(), (int)h.size(), 48000, 0.3f);
    EXPECT_NEAR(estimateT60(h.data(), (int)h.size(), 48000), 1.0 / (1.0 / 0.6 + 1.0 / 0.3), 0.01);
}

TEST(Hybrid, MakeHybridKeepsDecay) {
    const auto low = decayingNoise(0.6, 48000, 0.8, 7);
    const auto hy = makeHybrid(low.data(), (int)low.size(), 48000, 200.f, 0.6f, 0.01f);
    ASSERT_EQ(hy.size(), low.size());
    for (float v : hy) ASSERT_TRUE(std::isfinite(v));
    EXPECT_NEAR(estimateT60(hy.data(), (int)hy.size(), 48000), 0.6, 0.06);
}

TEST(Hybrid, MakeHybridDirectBeyondEnd) {
    const auto low = decayingNoise(0.6, 48000, 0.1);
    const auto hy = makeHybrid(low.data(), (int)low.size(), 48000, 200.f, 0.6f, 10.0f);   // t_d > IR-Länge
    for (float v : hy) ASSERT_TRUE(std::isfinite(v));
}
