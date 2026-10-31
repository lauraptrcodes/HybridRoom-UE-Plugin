// FieldView.h — 2D-Draufsicht auf das FDTD-Druckfeld (Schnitt bei fester Höhe) + Einfärbung
// Engine-unabhängig: wird vom Unreal-Visualisierer und von der Python-Anbindung genutzt.
#pragma once
#include "Export.h"
#include "FDTDGrid.h"
#include <cstdint>
#include <vector>

namespace fdtd {

    // Horizontaler Schnitt durch das Rauminnere (ohne die Spiegel-Randknoten).
    // Zelle (i, j) deckt x in [i*dx, (i+1)*dx), y in [j*dx, (j+1)*dx) ab; Zeile j liegt bei p[j*nx + i].
    struct FDTD_API FieldSlice {
        int nx = 0, ny = 0;
        double dx = 0.0;
        std::vector<float> p;      // Druck; NaN = Innenwand
        std::vector<uint8_t> wall; // 1 = Innenwand
    };

    // Schnitt auf Höhe zM (Meter, Raumkoordinaten) aus dem aktuellen Zeitschritt
    FDTD_API void extractSliceZ(const FDTDGrid& grid, double zM, FieldSlice& out);

    // max |p| bzw. Effektivwert im Schnitt (Wände ausgenommen)
    FDTD_API float slicePeak(const FieldSlice& s);
    FDTD_API float sliceRms(const FieldSlice& s);

    // Vorzeichenbehafteter Druck -> BGRA8 (passt zu Unreal PF_B8G8R8A8), nx*ny*4 Bytes.
    // v = p/scale, auf [-1, 1] begrenzt und mit |v|^gamma komprimiert (gamma < 1 hebt leise Anteile an).
    // Farbskala: negativ dunkelrot -> 0 orange -> positiv gelb/weiß; Wände schwarz.
    FDTD_API void colorizeSlice(const FieldSlice& s, float scale, float gamma, uint8_t* bgra);

    // Gleichanteilfreie Anregung für die Visualisierung.
    // Injiziert wird die zweite Differenz einer beschränkten Funktion g(t):
    //   s_n = g(t_{n+1}) - 2 g(t_n) + g(t_{n-1})
    // Im schallharten Raum gilt für den Druckmittelwert M: M_{n+1} - 2 M_n + M_{n-1} = s_n  ->  M_n = g(t_n),
    // also bleibt der Mittelwert beschränkt (keine Drift wie bei einer Quelle mit Gleichanteil).
    struct FDTD_API VisSource {
        enum class Type { Pulse, Sine };
        Type   type = Type::Pulse;
        double fmax = 400.0;   // Pulse: Spektrum bei fmax 20 dB unter dem Maximum
        double interval = 0.05;    // Pulse: Wiederholabstand (s), <= 0 = nur einmal
        double freq = 100.0;   // Sine: Frequenz (Hz)

        [[nodiscard]] double g(double t) const;                       // "Verschiebung"
        [[nodiscard]] double injection(double t, double dt) const {   // in p addieren
            return g(t + dt) - 2.0 * g(t) + g(t - dt);
        }
    };

} // namespace fdtd
