// Bake.cpp
#include "Baker.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "Constants.h"

namespace fdtd {

    namespace {
        constexpr double kPi = 3.14159265358979323846;

        // 1. Ableitung einer Gaußfunktion (gleichanteilfrei); |S(f)| fällt bei fmax um 20 dB
        struct GaussianPulse {
            double tau, t0;
            explicit GaussianPulse(double fmax)
                : tau(std::sqrt(std::log(10.0)) / (kPi * fmax)), t0(4.0 * tau) {
            }
            double operator()(double t) const {
                const double u = (t - t0) / tau;
                return -u * std::exp(-u * u);
            }
        };
    } // namespace

    // ---------------------------------------------------------------- Konfiguration
    SimulationConfig makeSimulationConfig(double Lx, double Ly, double Lz, double fmax, int outRate,
        double pointsPerLambda, double c) {

        SimulationConfig cfg;
        cfg.Lx = Lx; cfg.Ly = Ly; cfg.Lz = Lz; cfg.c = c;

        cfg.dx = c / (fmax * pointsPerLambda);
        cfg.Nx = (int)std::lround(Lx / cfg.dx) + 1;
        cfg.Ny = (int)std::lround(Ly / cfg.dx) + 1;
        cfg.Nz = (int)std::lround(Lz / cfg.dx) + 1;

        // CFL (3D, 2. Ordnung): c*dt/dx <= 1/sqrt(3); dt auf ganzzahliges Vielfaches von 1/outRate
        const double dtMax = 0.99 * cfg.dx / (c * std::sqrt(3.0));
        const int U = std::max(1, (int)std::floor(dtMax * outRate));
        cfg.dt = (double)U / outRate;
        return cfg;
    }

    // ---------------------------------------------------------------- Bake
    IrGrid bake(const SimulationConfig& cfgIn, const BakeSettings& bs,
        BakeStats* stats, ParallelFor pfor, ProgressFn progress) {
        // 1) Solver vorbereiten
        FDTDGrid grid(cfgIn);
        const SimulationConfig& cfg = grid.config();
        grid.setParallelFor(pfor);
        grid.setWallAbsorption(bs.wallAlpha);
        for (const Box& w : bs.innerWalls) grid.setWallRegion(w.min, w.max);

        // 2) Empfängergitter (Zellmitten) -> FDTD-Indizes
        IrGrid out;
        out.spacing = (float)bs.receiverSpacing;
        out.nx = std::max(1, (int)std::floor(cfg.Lx / bs.receiverSpacing));
        out.ny = std::max(1, (int)std::floor(cfg.Ly / bs.receiverSpacing));
        out.nz = std::max(1, (int)std::floor(cfg.Lz / bs.receiverSpacing));
        const int R = out.numReceivers();

        std::vector<int> recIdx(R);
        for (int r = 0; r < R; ++r) {
            float x, y, z;
            out.position(r, x, y, z);
            //recIdx[r] = grid.indexOf(Vec3{ x, y, z });
            recIdx[r] = grid.airIndexOf(Vec3{ x, y, z });
        }
        //const int srcIdx = grid.indexOf(bs.source);   // einmal umrechnen, nicht pro Schritt
        const int srcIdx = grid.airIndexOf(bs.source);   // einmal umrechnen, nicht pro Schritt

        // 3) Zeitschleife: Schritt -> Quelle einspeisen -> alle Empfänger aufnehmen
        const int steps = (int)std::ceil(bs.irLength / cfg.dt);
        const GaussianPulse pulse(cfg.fmax);
        std::vector<float> rec((size_t)R * steps, 0.f);

        for (int n = 0; n < steps; ++n) {
            grid.step();                                       // neuester Zustand = Zeit (n+1)*dt
            grid.injectSource(srcIdx, pulse((n + 1) * cfg.dt));
            for (int r = 0; r < R; ++r)
                rec[(size_t)r * steps + n] = (float)grid.pressureAt(recIdx[r]);

            if (progress && (n % 256 == 0) && !progress((float)n / steps)) {
                if (stats) stats->aborted = true;
                return IrGrid{};
            }
        }

        // 4) Resampling auf outRate (lineare Interpolation; funktioniert für beliebiges dt)
        //    Aufnahme n gehört zur Zeit (n+1)*dt; vor dem ersten Schritt ist der Raum still (0).
        out.sampleRate = bs.outRate;
        out.length = (int)std::floor(steps * cfg.dt * bs.outRate);
        out.data.assign((size_t)R * out.length, 0.f);

        float peak = 1e-20f;
        for (int r = 0; r < R; ++r) {
            const float* s = &rec[(size_t)r * steps];
            float* d = &out.data[(size_t)r * out.length];
            for (int m = 0; m < out.length; ++m) {
                const double pos = (double)m / bs.outRate / cfg.dt - 1.0;   // in Aufnahme-Indizes
                const int n0 = (int)std::floor(pos);
                const float f = (float)(pos - n0);
                const float v0 = (n0 >= 0 && n0 < steps) ? s[n0] : 0.f;
                const float v1 = (n0 + 1 >= 0 && n0 + 1 < steps) ? s[n0 + 1] : 0.f;
                d[m] = (1.f - f) * v0 + f * v1;
                peak = std::max(peak, std::fabs(d[m]));
            }
        }

        // 5) Globale Normierung -> relative Pegel zwischen Empfängern bleiben erhalten
        for (float& v : out.data) v /= peak;

        if (stats) {
            stats->Nx = cfg.Nx; stats->Ny = cfg.Ny; stats->Nz = cfg.Nz;
            stats->dx = cfg.dx; stats->dt = cfg.dt; stats->courant = cfg.c * cfg.dt / cfg.dx;
            stats->steps = steps; stats->receivers = R; stats->peak = peak;
            stats->aborted = false;
        }
        return out;
    }

    double sourcePulse(double t, double fmax) {
        // |G(f)| ~ exp(-(pi f tau)^2): -20 dB bei fmax -> tau = sqrt(ln 10) / (pi fmax)
        const double tau = std::sqrt(std::log(10.0)) / (fdtd::kPi * fmax);
        const double t0 = 4.0 * tau;
        const double u = (t - t0) / tau;
        return -u * std::exp(-u * u);
    }

    std::vector<double> simulatePoint(const SimulationConfig& cfgIn, Vec3 source, Vec3 receiver, double duration, double wallAlpha, SimulationConfig* derived, ParallelFor pfor)
    {
        FDTDGrid grid(cfgIn);
        const SimulationConfig& cfg = grid.config();
        grid.setParallelFor(pfor);
        if (wallAlpha > 0.0) grid.setWallAbsorption(wallAlpha);
        if (derived) *derived = cfg;

        const int srcIdx = grid.indexOf(source);
        const int recIdx = grid.indexOf(receiver);
        const int steps = (int)std::ceil(duration / cfg.dt);

        std::vector<double> h(steps);
        for (int n = 0; n < steps; ++n) {
            grid.step();
            grid.injectSource(srcIdx, sourcePulse((n + 1) * cfg.dt, cfg.fmax));
            h[n] = grid.pressureAt(recIdx);
        }
        return h;;
    }

} // namespace fdtd