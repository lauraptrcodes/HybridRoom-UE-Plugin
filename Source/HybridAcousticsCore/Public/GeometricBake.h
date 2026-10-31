// GeometricBake.h — Geometrischer Anteil der hybriden Simulation mit Steam Audio (Raytracing).
// Gleiche Szene wie der FDTD-Bake (Quader-Raum + Innenwände aus BakeSettings), gleiches Empfängergitter
// (Steam-Audio-Probes = FDTD-Empfänger, gleiche Reihenfolge) -> IR r beider Bakes gehören zusammen.
#pragma once
#include "Baker.h"
#include "Export.h"
#include "IrGrid.h"
#include "Types.h"

namespace fdtd {

    // ============================ CONFIG ============================
    struct GeoSettings {
        // Absorption je Steam-Audio-Band (Mitten 400 Hz, 2.5 kHz, 15 kHz). < 0 -> BakeSettings::wallAlpha für alle Bänder
        float absorption[3] = { -1.f, -1.f, -1.f };
        float scattering = 0.05f;   // Anteil diffus reflektierter Energie (0 = rein spiegelnd)
        int   numRays = 16384;   // Strahlen pro Probe
        int   numDiffuseSamples = 1024;
        int   numBounces = 0;       // 0 = automatisch aus IR-Länge und mittlerer freier Weglänge
        int   numThreads = 4;
        float irradianceMinDistance = 1.0f;
        int   order = 0;       // Ambisonics-Ordnung der IRs: 0 = omni, 1 = 4 Kanäle mit Einfallsrichtung
    };
    // ================================================================

    struct GeoStats {
        int  probes = 0;
        int  bounces = 0;
        bool ok = false;        // false: Steam Audio nicht initialisiert / Fehler / abgebrochen
        bool aborted = false;
    };

    // Reflexions-IRs (ohne Direktschall!) an allen Empfängern, Abtastrate bs.outRate, Länge bs.irLength.
    // Mit order = 1: (order+1)^2 = 4 Kanäle je Empfänger in Steam-Audio-Koordinaten (rechtshändig, y oben, -z vorn).
    // Nicht normiert: absolute Steam-Audio-Skala; Pegelanpassung an die FDTD-IR macht makeHybridGeo.
    FDTD_API IrGrid bakeGeometric(const SimulationConfig& cfg, const BakeSettings& bs, const GeoSettings& gs,
        GeoStats* stats = nullptr, ProgressFn progress = nullptr);

} // namespace fdtd
