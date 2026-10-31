// FieldView.cpp
#include "FieldView.h"
#include "Constants.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fdtd {

    void extractSliceZ(const FDTDGrid& grid, double zM, FieldSlice& out) {
        const SimulationConfig& c = grid.config();
        const int pad = c.pmlThickness;
        // Rauminneres: Knoten pad+1 .. N-2-pad  ->  N-2-2*pad Zellen je Achse
        out.nx = c.Nx - 2 - 2 * pad;
        out.ny = c.Ny - 2 - 2 * pad;
        out.dx = c.dx;
        const size_t n = (size_t)out.nx * out.ny;
        out.p.resize(n);
        out.wall.resize(n);

        const int k = std::clamp(grid.toGrid(Vec3{ 0.0, 0.0, zM }).z, pad + 1, c.Nz - 2 - pad);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (int j = 0; j < out.ny; ++j)
            for (int i = 0; i < out.nx; ++i) {
                const int g = grid.index(i + 1 + pad, j + 1 + pad, k);
                const size_t o = (size_t)j * out.nx + i;
                const bool w = grid.isWall(g);
                out.wall[o] = w ? 1 : 0;
                out.p[o] = w ? nan : (float)grid.pressureAt(g);
            }
    }

    float slicePeak(const FieldSlice& s) {
        float m = 0.f;
        for (size_t o = 0; o < s.p.size(); ++o)
            if (!s.wall[o]) m = std::max(m, std::fabs(s.p[o]));
        return m;
    }

    float sliceRms(const FieldSlice& s) {
        double e = 0.0; size_t n = 0;
        for (size_t o = 0; o < s.p.size(); ++o)
            if (!s.wall[o]) { e += (double)s.p[o] * s.p[o]; ++n; }
        return n ? (float)std::sqrt(e / (double)n) : 0.f;
    }

    namespace {
        struct Stop { float t, r, g, b; };
        // t in [0,1]: 0 = stark negativ, 0.5 = Ruhe, 1 = stark positiv
        constexpr Stop kMap[] = {
            { 0.00f,  70.f,   0.f,   0.f },   // dunkelrot
            { 0.25f, 200.f,  25.f,   0.f },   // rot
            { 0.50f, 245.f, 120.f,  10.f },   // orange (Ruhedruck)
            { 0.75f, 255.f, 205.f,  20.f },   // gelb
            { 1.00f, 255.f, 255.f, 190.f },   // hellgelb
        };
    } // namespace

    void colorizeSlice(const FieldSlice& s, float scale, float gamma, uint8_t* bgra) {
        const float inv = scale > 0.f ? 1.f / scale : 0.f;
        constexpr int numStops = (int)(sizeof(kMap) / sizeof(kMap[0]));
        for (size_t o = 0; o < s.p.size(); ++o) {
            uint8_t* px = bgra + 4 * o;
            if (s.wall[o]) { px[0] = px[1] = px[2] = 0; px[3] = 255; continue; }

            float v = std::clamp(s.p[o] * inv, -1.f, 1.f);
            v = std::copysign(std::pow(std::fabs(v), gamma), v);
            const float t = 0.5f + 0.5f * v;

            int a = 0;
            while (a < numStops - 2 && t > kMap[a + 1].t) ++a;
            const Stop& A = kMap[a];
            const Stop& B = kMap[a + 1];
            const float w = (t - A.t) / (B.t - A.t);
            px[2] = (uint8_t)(A.r + w * (B.r - A.r));   // R
            px[1] = (uint8_t)(A.g + w * (B.g - A.g));   // G
            px[0] = (uint8_t)(A.b + w * (B.b - A.b));   // B
            px[3] = 255;
        }
    }

    double VisSource::g(double t) const {
        if (t < 0.0) return 0.0;
        if (type == Type::Pulse) {
            // g = exp(-((t-t0)/tau)^2). Injiziert wird ~ dt^2 g'', Spektrum ~ f^2 exp(-(pi f tau)^2).
            // Maximum bei x = pi f tau = 1; -20 dB bei x^2 exp(1-x^2) = 0.1 -> x = 2.21 -> tau = 2.21/(pi fmax)
            const double tau = 2.21 / (kPi * fmax); 
            const double t0 = 4.0 * tau;
            if (interval > 8.0 * tau) t = std::fmod(t, interval);
            const double u = (t - t0) / tau;
            return std::exp(-u * u);
        }
        // Sinus mit weichem Einschalten über 3 Perioden (vermeidet Breitband-Knack)
        const double ramp = 3.0 / freq;
        const double env = t < ramp ? 0.5 * (1.0 - std::cos(kPi * t / ramp)) : 1.0;
        return env * std::sin(2.0 * kPi * freq * t);
    }

} // namespace fdtd
