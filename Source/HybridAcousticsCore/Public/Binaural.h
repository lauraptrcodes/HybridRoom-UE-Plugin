// Binaural.h — HRTF-Wiedergabe einer Punktquelle über Steam Audio (iplBinauralEffect), ohne Steam-Typen im Header.
// Ohne SDK (HAC_WITH_STEAMAUDIO = 0) liefert ok() false; der Aufrufer fällt dann auf Panning zurück.
#pragma once
#include "Export.h"
#include "Types.h"

namespace fdtd {

    // Gemeinsam für alle Quellen: Steam-Kontext + HRTF (teuer, einmal pro Audio-Gerät anlegen, nicht im Audio-Thread)
    class FDTD_API HrtfContext {
    public:
        HrtfContext() = default;
        ~HrtfContext();
        HrtfContext(const HrtfContext&) = delete;
        HrtfContext& operator=(const HrtfContext&) = delete;

        // sofaPath = nullptr -> eingebaute Steam-Audio-HRTF; sonst SOFA-Datei (z. B. KEMAR, SADIE II)
        bool create(int sampleRate, int frameSize, const char* sofaPath = nullptr);
        void destroy();
        [[nodiscard]] bool ok() const { return ctx_ != nullptr && hrtf_ != nullptr; }
        [[nodiscard]] int frameSize() const { return frameSize_; }

    private:
        friend class BinauralEffect;
        friend class AmbisonicsBinaural;
        void* ctx_ = nullptr;     // IPLContext
        void* hrtf_ = nullptr;    // IPLHRTF
        int sampleRate_ = 0, frameSize_ = 0;
    };

    // Pro Quelle: Faltung mit der HRTF der aktuellen Richtung (Steam blendet bei Richtungswechsel weich über)
    class FDTD_API BinauralEffect {
    public:
        BinauralEffect() = default;
        ~BinauralEffect();
        BinauralEffect(const BinauralEffect&) = delete;
        BinauralEffect& operator=(const BinauralEffect&) = delete;

        bool create(const HrtfContext& hrtf);
        void destroy();
        void reset();
        [[nodiscard]] bool ok() const { return effect_ != nullptr; }

        // dirListener: Richtung Hörer -> Quelle im Hörer-Koordinatensystem von Unreal (x vorn, y rechts, z oben),
        // muss nicht normiert sein. frames muss genau HrtfContext::frameSize() sein, sonst false (nichts geschrieben).
        bool process(const float* mono, float* left, float* right, int frames, const Vec3& dirListener,
            bool bilinear = false);

    private:
        void* effect_ = nullptr;  // IPLBinauralEffect
        void* hrtf_ = nullptr;    // geborgte IPLHRTF (Steam zählt Referenzen)
        int frameSize_ = 0;
    };

    // Ambisonics -> binaural mit Kopfdrehung (iplAmbisonicsDecodeEffect = Rotation + HRTF-Decodierung).
    // Eingang: (order+1)^2 Kanäle im Raumkoordinatensystem der Steam-Audio-Bakes. Pro Quelle eine Instanz.
    class FDTD_API AmbisonicsBinaural {
    public:
        AmbisonicsBinaural() = default;
        ~AmbisonicsBinaural();
        AmbisonicsBinaural(const AmbisonicsBinaural&) = delete;
        AmbisonicsBinaural& operator=(const AmbisonicsBinaural&) = delete;

        bool create(const HrtfContext& hrtf, int order = 1);
        void destroy();
        void reset();
        [[nodiscard]] bool ok() const { return effect_ != nullptr; }
        [[nodiscard]] int numChannels() const { return (order_ + 1) * (order_ + 1); }

        // forward/up: Blick- und Oben-Richtung des Hörers im Raumkoordinatensystem (wie Unreal: x vorn, y rechts, z oben,
        // aber relativ zum Raum-Actor). frames muss HrtfContext::frameSize() sein.
        bool process(const float* const* ambi, float* left, float* right, int frames, const Vec3& forward, const Vec3& up);

    private:
        void* effect_ = nullptr;  // IPLAmbisonicsDecodeEffect
        void* hrtf_ = nullptr;
        int frameSize_ = 0, order_ = 1;
    };

} // namespace fdtd
