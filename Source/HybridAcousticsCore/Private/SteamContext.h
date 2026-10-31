// SteamContext.h — interne Hilfe (nur in .cpp-Dateien nach <phonon.h> einbinden)
#pragma once

namespace fdtd::detail {

    // ALLE Steam-Audio-Kontexte im Prozess müssen dieselben Einstellungen haben.
    // Steam Audio wählt die SIMD-Variante global beim Anlegen eines Kontexts. Wird später ein Kontext mit anderer
    // SIMD-Stufe angelegt (z. B. für einen Bake), liefern bereits bestehende Binaural-/Ambisonics-Effekte Stille oder
    // falsche Pegel (gemessen in test_ambisonics). Deshalb erzeugt jede Stelle im Kern ihre Einstellungen hier.
    inline IPLContextSettings steamContextSettings() {
        IPLContextSettings cs{};
        cs.version = STEAMAUDIO_VERSION;
        cs.simdLevel = IPL_SIMDLEVEL_AVX2;   // Obergrenze; {} hieße SSE2
        return cs;
    }

} // namespace fdtd::detail
