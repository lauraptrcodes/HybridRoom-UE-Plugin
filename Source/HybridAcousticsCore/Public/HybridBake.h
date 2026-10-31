// HybridBake.h — kompletter hybrider Bake in einem Aufruf + Speichern/Laden (Bake-Cache).
// Ablauf: Steam Audio -> T60 -> Schroeder-Frequenz -> FDTD bis FmaxFactor * fc.
// Unreal und Python benutzen dieselbe Funktion und dasselbe Dateiformat.
#pragma once
#include "Baker.h"
#include "Export.h"
#include "GeometricBake.h"
#include "IrGrid.h"
#include "Types.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fdtd {

    // ============================ CONFIG ============================
    struct CrossoverSettings {
        bool   autoFc = true;     // true: fc = Schroeder-Frequenz, sonst fixedFc
        double fixedFc = 200.0;    // Hz
        double fcMin = 80.0;     // Grenzen für autoFc (FDTD-Rechenzeit ~ fmax^4)
        double fcMax = 400.0;
        double fmaxFactor = 2.0;      // FDTD rechnet bis fmaxFactor * fc
        double fallbackT60 = 0.6;      // s, wenn weder Steam Audio noch Eyring ein T60 liefern
        double t60BandLo = 283.0;    // Band für die T60-Messung (Oktave um 400 Hz)
        double t60BandHi = 566.0;
    };

    struct HybridBakeInput {
        SimulationConfig  cfg;         // nur Eingaben (Lx..Lz, c, pointsPerLambda, pmlThickness); fmax kommt aus fc
        BakeSettings      settings;
        GeoSettings       geo;
        bool              useGeo = true;
        CrossoverSettings crossover;
    };
    // ================================================================

    enum class T60Source : int32_t { Geometric = 0, Eyring = 1, Fallback = 2 };

    struct HybridBakeResult {
        SimulationConfig cfg;          // inkl. fmax und abgeleiteter Werte (dx, dt, Nx..Nz) des FDTD-Bakes
        BakeSettings     settings;
        IrGrid           grid;         // FDTD (Tiefton)
        IrGrid           geo;          // Steam Audio (Reflexionen); leer = Rauschnachhall-Fallback
        float            t60 = -1.f;
        T60Source        t60Source = T60Source::Fallback;
        float            fs = 0.f;     // Schroeder-Frequenz (ungeklemmt)
        float            fc = 0.f;     // verwendete Übergangsfrequenz
        BakeStats        stats;        // FDTD
        GeoStats         geoStats;
        bool             aborted = false;
        bool             fromCache = false;
    };

    // 64-bit-Schlüssel über alle Eingaben, die das Ergebnis bestimmen (FNV-1a, plattformunabhängig).
    // Nicht enthalten: Threadanzahl (ändert das Ergebnis nicht).
    FDTD_API uint64_t hashInput(const HybridBakeInput& in);
    FDTD_API std::string describeInput(const HybridBakeInput& in);

    // Kompletter Bake. Fortschritt: geometrisch 0..0.5, FDTD 0.5..1 (ohne Geometrie: FDTD 0..1).
    FDTD_API HybridBakeResult runHybridBake(const HybridBakeInput& in, ParallelFor pfor = nullptr,
        ProgressFn progress = nullptr);

    // Binärformat ("HACB"): Kopf mit Formatversion und Schlüssel, danach Config, Settings, T60/fc und beide Gitter.
    FDTD_API std::vector<uint8_t> serializeBake(uint64_t key, const HybridBakeResult& r);
    // false bei falschem Magic/Version, anderem Schlüssel (Eingaben geändert) oder beschädigten Daten
    FDTD_API bool deserializeBake(const uint8_t* data, size_t size, uint64_t expectedKey, HybridBakeResult& out,
        std::string* why = nullptr);    // Schlüssel einer Datei lesen, ohne sie zu laden; 0 wenn ungültig
    FDTD_API uint64_t peekBakeKey(const uint8_t* data, size_t size);

    // Hybrid-IR für einen Empfänger, bezogen auf den Direktschall:
    //  - FDTD-Anteil über fdtdToUnitIr entzerrt -> tieffrequenter Direktschall = Einheitsimpuls bei preDelaySec
    //    (inkl. Beugung um Innenwände), Reflexionen im physikalisch richtigen Verhältnis dazu
    //  - Steam-Audio-Reflexionen auf dieselbe Zeitachse verschoben und im Band [fc, fmax] angeglichen
    //  - ohne Steam Audio: Rauschnachhall (makeHybrid) ab preDelaySec
    // Der hochfrequente Direktschall gehört in den Dry-Pfad: LR4-Hochpass bei fc, verzögert um preDelaySec (+ Faltungslatenz).
    FDTD_API std::vector<float> receiverIr(const HybridBakeResult& r, int receiver, double preDelaySec,
        float* geoGainOut = nullptr);

    // Variante für Ambisonics (geo mit order = 1, 4 Kanäle):
    //  - low: Tiefton unter fc (FDTD: Direktschall + Reflexionen), mono, für beide Ohren gleich (Kohärenz ~1 unter ~300 Hz)
    //  - hf:  Hochton über fc (Steam-Reflexionen), (order+1)^2 Kanäle im Raumkoordinatensystem, Pegel wie in receiverIr
    // low + Decodierung(hf) ersetzt die mono IR aus receiverIr. false, wenn geo keine Ambisonics-Daten enthält.
    //
    // diffuseFill: Steam rekonstruiert die IR aus der Richtungsverteilung der ENERGIE. Für den isotropen (diffusen) Anteil
    // landet dabei fast alles in W (gemessen: X/Y/Z nur 2-5 % der W-Energie) -> beide Ohren hören nahezu dasselbe
    // (IACC ~0.6). Ein echtes diffuses Feld hat in orthonormalen SH in jedem Kanal dieselbe Energie mit unkorrelierten
    // Signalen (IACC ~0.27 nach Decodierung 1. Ordnung). diffuseFill ergänzt deshalb je Richtungskanal unabhängiges
    // Rauschen, dessen Kurzzeit-Energie (10-ms-Fenster wie Steams Bins) die Lücke E_W - E_c schließt.
    struct FDTD_API AmbiIrSet {
        std::vector<float> low;
        std::vector<std::vector<float>> hf;
    };
    FDTD_API bool receiverAmbiIr(const HybridBakeResult& r, int receiver, double preDelaySec, AmbiIrSet& out,
        float* geoGainOut = nullptr, bool diffuseFill = true);

    // Einfache Dekorrelation (funktioniert mit dem normalen Mono-Bake):
    //  - low:   Tiefton unter fc (FDTD: Direktschall + Reflexionen), mono -> beide Ohren gleich
    //  - hf[0]: Hochton-Nachhall linkes Ohr  = Steam-IR (W/omni), Pegel wie in receiverIr
    //  - hf[1]: Hochton-Nachhall rechtes Ohr = unabhängiges Rauschen mit derselben Kurzzeit-Energie (10-ms-Fenster)
    // Im diffusen Feld ist die Kohärenz zwischen den Ohren oberhalb von ~1 kHz ~0 (sin(kd)/(kd), d = 0.18 m);
    // genau das liefern zwei unabhängige Realisierungen. low + hf[0] bzw. low + hf[1] ersetzen die mono IR.
    struct FDTD_API StereoIrSet {
        std::vector<float> low;
        std::vector<float> hf[2];
    };
    FDTD_API bool receiverStereoIr(const HybridBakeResult& r, int receiver, double preDelaySec, StereoIrSet& out,
        float* geoGainOut = nullptr);

    // Datei-Varianten (std::fstream; Unreal benutzt FFileHelper + die Puffer-Funktionen oben)
    FDTD_API bool saveBakeFile(const std::string& path, uint64_t key, const HybridBakeResult& r);
    FDTD_API bool loadBakeFile(const std::string& path, uint64_t expectedKey, HybridBakeResult& out);

} // namespace fdtd
