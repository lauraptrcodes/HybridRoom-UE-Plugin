#pragma once
#include <string>

namespace fdtd {
    enum class Scheme {
        Order2,
        Order4
    };

    struct Vec3 {
        double x{ 0.0 }, y{ 0.0 }, z{ 0.0 };
    };
    struct Vec3i {
        int x{ 0 }, y{ 0 }, z{ 0 };
    };

    struct SimulationConfig {
        float Lx = 5.0f, Ly = 4.0f, Lz = 3.0f;          // Raum (m), Ursprung in einer Ecke, Z = Höhe
        //float srcX = 1.2f, srcY = 1.0f, srcZ = 1.5f;    // Quelle (m, raumlokal)

        float fmax = 500.0f; // obere Grenzfrequenz FDTD (Hz)
        float pointsPerLambda = 10.0f;  // Gitterpunkte pro Wellenlänge bei fmax
        float T60 = 0.6f;   // Ziel-Nachhallzeit (s)
        float irLength = 0.8f;   // IR-Länge (s)
        float receiverSpacing = 0.5f;   // Abstand des Empfängergitters (m)
        int   outRate = 48000;  // Ausgabe-Samplerate
        float c = 343.0f; // Schallgeschwindigkeit (m/s)
        int   pmlThickness{ 0 };            // PML absorbing layer cells
        int   spatialOrder = 2;

        Scheme scheme{ Scheme::Order2 };

        int Nx = 0, Ny = 0, Nz = 0;
        float dx = 0, dt = 0, courant = 0, fsSim = 0;
        int upsample = 0, steps = 0;
    };

    enum class SourceSignal {
        GaussPulse,   // Broadband Gaussian pulse
        SincPulse,    // Bandlimited sinc
        Dirac         // Single-sample impulse (ideal but requires care)
    };

    struct SourceDesc {
        Vec3         pos;
        SourceSignal signal{ SourceSignal::GaussPulse };
        double       amplitude{ 1.0 };
    };

    struct ReceiverDesc {
        Vec3        pos;
        std::string label{ "rec" };
    };

    struct Box { Vec3 min, max; };
}