// DirectPath.cpp
#include "DirectPath.h"
#include "Constants.h"
#include <algorithm>
#include <cmath>

namespace fdtd {

    // ---------------------------------------------------------------- Linkwitz-Riley 4
    void LinkwitzRiley4::setup(double fc, double fs, bool highpass) {
        const double w0 = 2.0 * kPi * fc / fs;
        const double cw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;
        if (highpass) { b0_ = (1 + cw) / 2; b1_ = -(1 + cw); b2_ = (1 + cw) / 2; }
        else { b0_ = (1 - cw) / 2; b1_ = (1 - cw); b2_ = (1 - cw) / 2; }
        a1_ = -2.0 * cw;
        a2_ = 1.0 - alpha;
        b0_ /= a0; b1_ /= a0; b2_ /= a0; a1_ /= a0; a2_ /= a0;
    }

    void LinkwitzRiley4::reset() {
        for (auto& s : z_) { s[0] = 0.0; s[1] = 0.0; }
    }

    float LinkwitzRiley4::process(float x) {
        double v = x;
        for (auto& s : z_) {
            const double y = b0_ * v + s[0];
            s[0] = b1_ * v - a1_ * y + s[1];
            s[1] = b2_ * v - a2_ * y;
            v = y;
        }
        return (float)v;
    }

    // ---------------------------------------------------------------- Verdeckung
    bool segmentHitsBox(const Vec3& a, const Vec3& b, const Box& box) {
        // Strecke a + t (b - a), t in [0, 1]; Schnitt mit den drei Plattenpaaren (Slabs) des Quaders
        double t0 = 0.0, t1 = 1.0;
        const double A[3] = { a.x, a.y, a.z };
        const double D[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
        const double lo[3] = { box.min.x, box.min.y, box.min.z };
        const double hi[3] = { box.max.x, box.max.y, box.max.z };
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(D[k]) < 1e-12) {
                if (A[k] < lo[k] || A[k] > hi[k]) return false;    // parallel und außerhalb der Platte
                continue;
            }
            double ta = (lo[k] - A[k]) / D[k], tb = (hi[k] - A[k]) / D[k];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return true;
    }

    float directVisibility(const Vec3& source, const Vec3& listener, const std::vector<Box>& walls,
        double sourceRadius, int numSamples) {
        if (walls.empty()) return 1.f;
        numSamples = std::max(1, numSamples);
        const double golden = kPi * (3.0 - std::sqrt(5.0));         // Fibonacci-Kugel: gleichmäßige Punkte
        int free = 0;
        for (int i = 0; i < numSamples; ++i) {
            Vec3 p = source;
            if (numSamples > 1) {
                const double z = 1.0 - 2.0 * (i + 0.5) / numSamples;
                const double rr = std::sqrt(std::max(0.0, 1.0 - z * z));
                const double phi = golden * i;
                p = Vec3{ source.x + sourceRadius * rr * std::cos(phi), source.y + sourceRadius * rr * std::sin(phi),
                          source.z + sourceRadius * z };
            }
            bool blocked = false;
            for (const Box& w : walls)
                if (segmentHitsBox(p, listener, w)) { blocked = true; break; }
            if (!blocked) ++free;
        }
        return (float)free / (float)numSamples;
    }

} // namespace fdtd
