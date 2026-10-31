#pragma once
#include "Export.h"
#include "FDTDGrid.h"
#include "IrGrid.h"
#include "Types.h"
#include <functional>

namespace fdtd {

    using ParallelFor = FDTDGrid::ParallelFor;

    // Fortschritt 0..1; Rückgabe false = Bake abbrechen
    using ProgressFn = std::function<bool(float progress)>;

    // ============================ CONFIG ============================
    struct BakeSettings {
        Vec3   source{ 1.2, 1.0, 1.5 };  // Quellposition (m, raumlokal)
        // double fmax = 400.0;  // Bandbreite des Quellpulses (Hz, -20 dB)
        double irLength = 0.8;    // IR-Länge (s)
        double receiverSpacing = 0.5;    // Abstand des Empfängergitters (m)
        int    outRate = 48000;  // Samplerate der IRs (Hz)
        double wallAlpha = 0.17;   // Absorptionsgrad der Wände
        std::vector<Box> innerWalls;
    };
    // ================================================================

    struct BakeStats {
        int    Nx = 0, Ny = 0, Nz = 0;
        double dx = 0, dt = 0, courant = 0;
        int    steps = 0, receivers = 0;
        float  peak = 0.f;               // Normierungsfaktor (IRs wurden durch peak geteilt)
        bool   aborted = false;
    };

    // Gitter aus Raumgröße und fmax ableiten:
    // dx = c / (fmax * pointsPerLambda), dt = U / outRate mit größtem U unter der CFL-Grenze
    SimulationConfig makeSimulationConfig(double Lx, double Ly, double Lz, double fmax, int outRate,
        double pointsPerLambda = 10.0, double c = 343.0);

    // Leeres IrGrid (numReceivers() == 0), wenn der Bake über progress abgebrochen wurde
    FDTD_API IrGrid bake(const SimulationConfig& cfgIn, const BakeSettings& bs,
        BakeStats* stats = nullptr, ParallelFor pfor = nullptr, ProgressFn progress = nullptr);

    FDTD_API double sourcePulse(double t, double fmax);
    FDTD_API std::vector<double> simulatePoint(const SimulationConfig& cfgIn, Vec3 source, Vec3 receiver, 
        double duration, double wallAlpha, SimulationConfig * derived = nullptr, ParallelFor pfor = nullptr);
}