// Binaural.cpp
#include "Binaural.h"
#include <cmath>

#if defined(HAC_WITH_STEAMAUDIO) && HAC_WITH_STEAMAUDIO

#if defined(THIRD_PARTY_INCLUDES_START)
THIRD_PARTY_INCLUDES_START
#endif
#include <phonon.h>
#if defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_END
#endif
#include "SteamContext.h"

namespace fdtd {

    HrtfContext::~HrtfContext() { destroy(); }

    bool HrtfContext::create(int sampleRate, int frameSize, const char* sofaPath) {
        destroy();
        IPLContextSettings cs = detail::steamContextSettings();   // gleiche Einstellungen wie alle anderen Kontexte!
        IPLContext ctx = nullptr;
        if (iplContextCreate(&cs, &ctx) != IPL_STATUS_SUCCESS) return false;

        IPLAudioSettings as{ sampleRate, frameSize };
        IPLHRTFSettings hs{};
        hs.type = sofaPath ? IPL_HRTFTYPE_SOFA : IPL_HRTFTYPE_DEFAULT;
        hs.sofaFileName = sofaPath;
        hs.volume = 1.0f;
        hs.normType = IPL_HRTFNORMTYPE_NONE;
        IPLHRTF hrtf = nullptr;
        if (iplHRTFCreate(ctx, &as, &hs, &hrtf) != IPL_STATUS_SUCCESS) {
            iplContextRelease(&ctx);
            return false;
        }
        ctx_ = ctx; hrtf_ = hrtf;
        sampleRate_ = sampleRate; frameSize_ = frameSize;
        return true;
    }

    void HrtfContext::destroy() {
        if (hrtf_) { IPLHRTF h = static_cast<IPLHRTF>(hrtf_); iplHRTFRelease(&h); hrtf_ = nullptr; }
        if (ctx_) { IPLContext c = static_cast<IPLContext>(ctx_); iplContextRelease(&c); ctx_ = nullptr; }
    }

    BinauralEffect::~BinauralEffect() { destroy(); }

    bool BinauralEffect::create(const HrtfContext& h) {
        destroy();
        if (!h.ok()) return false;
        IPLAudioSettings as{ h.sampleRate_, h.frameSize_ };
        IPLBinauralEffectSettings es{ static_cast<IPLHRTF>(h.hrtf_) };
        IPLBinauralEffect e = nullptr;
        if (iplBinauralEffectCreate(static_cast<IPLContext>(h.ctx_), &as, &es, &e) != IPL_STATUS_SUCCESS) return false;
        effect_ = e;
        hrtf_ = iplHRTFRetain(static_cast<IPLHRTF>(h.hrtf_));   // HRTF lebt mindestens so lange wie der Effekt
        frameSize_ = h.frameSize_;
        return true;
    }

    void BinauralEffect::destroy() {
        if (effect_) { IPLBinauralEffect e = static_cast<IPLBinauralEffect>(effect_); iplBinauralEffectRelease(&e); effect_ = nullptr; }
        if (hrtf_) { IPLHRTF h = static_cast<IPLHRTF>(hrtf_); iplHRTFRelease(&h); hrtf_ = nullptr; }
    }

    void BinauralEffect::reset() {
        if (effect_) iplBinauralEffectReset(static_cast<IPLBinauralEffect>(effect_));
    }

    bool BinauralEffect::process(const float* mono, float* left, float* right, int frames, const Vec3& d, bool bilinear) {
        if (!effect_ || frames != frameSize_) return false;

        // Unreal-Hörerraum (x vorn, y rechts, z oben) -> Steam Audio (x rechts, y oben, -z vorn): (Y, Z, -X)
        float x = (float)d.y, y = (float)d.z, z = (float)-d.x;
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-6f) { x = 0.f; y = 0.f; z = -1.f; }   // Quelle im Kopf: geradeaus
        else { x /= len; y /= len; z /= len; }

        IPLfloat32* inData[1] = { const_cast<IPLfloat32*>(mono) };
        IPLfloat32* outData[2] = { left, right };
        IPLAudioBuffer in{ 1, frames, inData };
        IPLAudioBuffer out{ 2, frames, outData };

        IPLBinauralEffectParams p{};
        p.direction = IPLVector3{ x, y, z };
        p.interpolation = bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
        p.spatialBlend = 1.0f;
        p.hrtf = static_cast<IPLHRTF>(hrtf_);
        p.peakDelays = nullptr;
        iplBinauralEffectApply(static_cast<IPLBinauralEffect>(effect_), &p, &in, &out);
        return true;
    }

    // ---------------------------------------------------------------- Ambisonics -> binaural
    AmbisonicsBinaural::~AmbisonicsBinaural() { destroy(); }

    bool AmbisonicsBinaural::create(const HrtfContext& h, int order) {
        destroy();
        if (!h.ok()) return false;
        IPLAudioSettings as{ h.sampleRate_, h.frameSize_ };
        IPLAmbisonicsDecodeEffectSettings es{};
        es.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
        es.hrtf = static_cast<IPLHRTF>(h.hrtf_);
        es.maxOrder = order;
        IPLAmbisonicsDecodeEffect e = nullptr;
        if (iplAmbisonicsDecodeEffectCreate(static_cast<IPLContext>(h.ctx_), &as, &es, &e) != IPL_STATUS_SUCCESS) return false;
        effect_ = e;
        hrtf_ = iplHRTFRetain(static_cast<IPLHRTF>(h.hrtf_));
        frameSize_ = h.frameSize_;
        order_ = order;
        return true;
    }

    void AmbisonicsBinaural::destroy() {
        if (effect_) { IPLAmbisonicsDecodeEffect e = static_cast<IPLAmbisonicsDecodeEffect>(effect_); iplAmbisonicsDecodeEffectRelease(&e); effect_ = nullptr; }
        if (hrtf_) { IPLHRTF h = static_cast<IPLHRTF>(hrtf_); iplHRTFRelease(&h); hrtf_ = nullptr; }
    }

    void AmbisonicsBinaural::reset() {
        if (effect_) iplAmbisonicsDecodeEffectReset(static_cast<IPLAmbisonicsDecodeEffect>(effect_));
    }

    namespace {
        // Raum/Unreal (x vorn, y rechts, z oben, linkshändig) -> Steam Audio (x rechts, y oben, -z vorn): (Y, Z, -X)
        IPLVector3 toSteamDir(const Vec3& v) { return IPLVector3{ (float)v.y, (float)v.z, (float)-v.x }; }
        IPLVector3 normalized(IPLVector3 v) {
            const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return l > 1e-6f ? IPLVector3{ v.x / l, v.y / l, v.z / l } : v;
        }
        IPLVector3 crossIpl(IPLVector3 a, IPLVector3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    } // namespace

    bool AmbisonicsBinaural::process(const float* const* ambi, float* left, float* right, int frames,
        const Vec3& forward, const Vec3& up) {
        if (!effect_ || frames != frameSize_) return false;

        // Hörer-Koordinatensystem in Steam-Koordinaten (rechtshändig): rechts = vorn x oben
        const IPLVector3 ahead = normalized(toSteamDir(forward));
        const IPLVector3 upS = normalized(toSteamDir(up));
        const IPLVector3 rightS = normalized(crossIpl(ahead, upS));

        IPLfloat32* inData[16];
        const int nch = numChannels();
        for (int c = 0; c < nch; ++c) inData[c] = const_cast<IPLfloat32*>(ambi[c]);
        IPLfloat32* outData[2] = { left, right };
        IPLAudioBuffer in{ nch, frames, inData };
        IPLAudioBuffer out{ 2, frames, outData };

        IPLAmbisonicsDecodeEffectParams p{};
        p.order = order_;
        p.hrtf = static_cast<IPLHRTF>(hrtf_);
        p.orientation.right = rightS;
        p.orientation.up = upS;
        p.orientation.ahead = ahead;
        p.orientation.origin = IPLVector3{ 0.f, 0.f, 0.f };
        p.binaural = IPL_TRUE;
        iplAmbisonicsDecodeEffectApply(static_cast<IPLAmbisonicsDecodeEffect>(effect_), &p, &in, &out);
        return true;
    }

} // namespace fdtd

#else // ohne Steam Audio: Platzhalter

namespace fdtd {
    HrtfContext::~HrtfContext() {}
    bool HrtfContext::create(int, int, const char*) { return false; }
    void HrtfContext::destroy() {}
    BinauralEffect::~BinauralEffect() {}
    bool BinauralEffect::create(const HrtfContext&) { return false; }
    void BinauralEffect::destroy() {}
    void BinauralEffect::reset() {}
    bool BinauralEffect::process(const float*, float*, float*, int, const Vec3&, bool) { return false; }
    AmbisonicsBinaural::~AmbisonicsBinaural() {}
    bool AmbisonicsBinaural::create(const HrtfContext&, int) { return false; }
    void AmbisonicsBinaural::destroy() {}
    void AmbisonicsBinaural::reset() {}
    bool AmbisonicsBinaural::process(const float* const*, float*, float*, int, const Vec3&, const Vec3&) { return false; }
} // namespace fdtd

#endif
