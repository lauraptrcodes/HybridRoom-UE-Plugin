// Partitionierte Faltung vs. direkte Faltung, IR-Wechsel, Reset
#include "Convolver.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace fdtd;

namespace {
std::vector<float> direct(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size(), 0.f);
    for (size_t n = 0; n < x.size(); ++n) {
        double s = 0;
        for (size_t k = 0; k < h.size() && k <= n; ++k) s += (double)h[k] * x[n - k];
        y[n] = (float)s;
    }
    return y;
}
double relErr(const std::vector<float>& y, const std::vector<float>& ref, size_t from, int lat) {
    double e = 0, r = 0;
    for (size_t n = from; n < y.size(); ++n) { e += std::pow(y[n] - ref[n - lat], 2); r += std::pow(ref[n - lat], 2); }
    return std::sqrt(e / r);
}
struct Signals {
    std::vector<float> x, h1, h2;
    Signals() : x(20000), h1(3000), h2(1777) {
        std::mt19937 rng(7); std::normal_distribution<float> nd;
        for (auto& v : x) v = nd(rng);
        for (size_t i = 0; i < h1.size(); ++i) h1[i] = nd(rng) * std::exp(-(float)i / 800.f);
        for (size_t i = 0; i < h2.size(); ++i) h2[i] = nd(rng) * std::exp(-(float)i / 500.f);
    }
};
} // namespace

TEST(Convolver, MatchesDirectWithRandomBlockSizes) {
    const Signals s; const int B = 256;
    const auto ref = direct(s.x, s.h1);
    Convolver c(B);
    c.setIr(partitionIr(s.h1.data(), (int)s.h1.size(), B));
    std::vector<float> y(s.x.size());
    std::mt19937 rng(1); std::uniform_int_distribution<int> chunk(1, 700);
    for (size_t p = 0; p < s.x.size();) {
        const int n = std::min<int>(chunk(rng), (int)(s.x.size() - p));
        c.process(&s.x[p], &y[p], n);
        p += n;
    }
    EXPECT_EQ(c.latency(), B);
    EXPECT_LT(relErr(y, ref, B, B), 1e-5);
}

TEST(Convolver, IrSwitchIsExactAfterCrossfade) {
    const Signals s; const int B = 256;
    const auto ref2 = direct(s.x, s.h2);
    Convolver c(B);
    c.setIr(partitionIr(s.h1.data(), (int)s.h1.size(), B));
    const int at = 30 * B;
    std::vector<float> y(s.x.size());
    c.process(s.x.data(), y.data(), at);
    c.setIr(partitionIr(s.h2.data(), (int)s.h2.size(), B));
    c.process(&s.x[at], &y[at], (int)s.x.size() - at);
    EXPECT_LT(relErr(y, ref2, at + 2 * B, B), 1e-5);
}

TEST(Convolver, ResetClearsTail) {
    const Signals s; const int B = 128;
    Convolver c(B);
    c.setIr(partitionIr(s.h1.data(), (int)s.h1.size(), B));
    std::vector<float> y(4096);
    c.process(s.x.data(), y.data(), 4096);
    c.reset();
    std::vector<float> zeros(8192, 0.f), out(8192);
    c.process(zeros.data(), out.data(), 8192);
    for (float v : out) ASSERT_EQ(v, 0.f);
}
