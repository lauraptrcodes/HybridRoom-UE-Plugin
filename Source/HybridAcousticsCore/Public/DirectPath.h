// DirectPath.h — Bausteine für den Direktschall-Pfad im Spatializer (echtzeitfähig, ohne Allokation):
//  - LinkwitzRiley4: Hochpass für den Dry-Pfad, ergänzt den LR4-Tiefpass der FDTD-IR zu flachem Betragsgang
//  - directVisibility: Anteil des Direktschalls, der nicht von Innenwänden verdeckt ist (volumetrisch)
#pragma once
#include "Export.h"
#include "Types.h"
#include <vector>

namespace fdtd {

    // Linkwitz-Riley 4. Ordnung = zwei gleiche Butterworth-Biquads (RBJ, Q = 1/sqrt 2) hintereinander
    class FDTD_API LinkwitzRiley4 {
    public:
        void setup(double fc, double fs, bool highpass);
        void reset();
        float process(float x);
    private:
        double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
        double z_[2][2] = { { 0, 0 }, { 0, 0 } };   // Zustand je Stufe (transponierte Direktform II)
    };

    // Sichtbarkeit Quelle -> Hörer: Die Quelle wird als Kugel mit Radius sourceRadius behandelt; von numSamples
    // gleichmäßig verteilten Punkten (Fibonacci-Kugel) wird je eine Strecke zum Hörer gegen alle Wände getestet.
    // Rückgabe = Anteil freier Strecken (1 = frei, 0 = vollständig verdeckt). Wie Steam Audios "volumetric occlusion".
    FDTD_API float directVisibility(const Vec3& source, const Vec3& listener, const std::vector<Box>& walls,
        double sourceRadius = 0.25, int numSamples = 32);

    // true, wenn die Strecke a -> b den Quader schneidet (Slab-Test)
    FDTD_API bool segmentHitsBox(const Vec3& a, const Vec3& b, const Box& box);

} // namespace fdtd
