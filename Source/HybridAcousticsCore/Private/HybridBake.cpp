// HybridBake.cpp
#include "HybridBake.h"
#include "Hybrid.h"
#include "DirectPath.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>

#if defined(HAC_WITH_STEAMAUDIO) && HAC_WITH_STEAMAUDIO
#if defined(THIRD_PARTY_INCLUDES_START)
THIRD_PARTY_INCLUDES_START
#endif
#include <phonon_version.h>
#if defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_END
#endif
#define HAC_SA_VERSION ((uint32_t)((STEAMAUDIO_VERSION_MAJOR << 16) | (STEAMAUDIO_VERSION_MINOR << 8) | STEAMAUDIO_VERSION_PATCH))
#else
#define HAC_SA_VERSION 0u
#endif

namespace fdtd {

    namespace {
        constexpr uint32_t kMagic = 0x42434148u;     // "HACB" (little endian)
        constexpr uint32_t kFormatVersion = 3;       // bei jeder Änderung am Format oder am Bake-Algorithmus erhöhen (2: Kanäle, 3: Ordnung)

        // ---------------------------------------------------------------- Hash
        struct Fnv {
            uint64_t h = 1469598103934665603ull;
            void bytes(const void* p, size_t n) {
                const auto* b = static_cast<const uint8_t*>(p);
                for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
            }
            template <class T> void add(const T& v) { bytes(&v, sizeof(T)); }
            void add(const Vec3& v) { add(v.x); add(v.y); add(v.z); }
            // Gleitkommawert vor dem Hashen auf ein Raster runden: Editor- und PIE-Welt berechnen Positionen über
            // leicht unterschiedliche Transform-Wege -> Abweichungen in der letzten Stelle dürfen den Schlüssel nicht ändern
            void addQ(double v, double step) { add((int64_t)std::llround(v / step)); }
            void addQ(const Vec3 & v, double step) { addQ(v.x, step); addQ(v.y, step); addQ(v.z, step); }
        };

        // ---------------------------------------------------------------- Schreiben / Lesen
        struct Writer {
            std::vector<uint8_t> buf;
            template <class T> void put(const T& v) {
                const auto* b = reinterpret_cast<const uint8_t*>(&v);
                buf.insert(buf.end(), b, b + sizeof(T));
            }
            void put(const Vec3& v) { put((double)v.x); put((double)v.y); put((double)v.z); }
            void putFloats(const std::vector<float>& v) {
                put((uint64_t)v.size());
                const auto* b = reinterpret_cast<const uint8_t*>(v.data());
                buf.insert(buf.end(), b, b + v.size() * sizeof(float));
            }
        };

        struct Reader {
            const uint8_t* p; size_t left; bool ok = true;
            template <class T> T get() {
                T v{};
                if (left < sizeof(T)) { ok = false; return v; }
                std::memcpy(&v, p, sizeof(T)); p += sizeof(T); left -= sizeof(T);
                return v;
            }
            Vec3 getVec3() { Vec3 v; v.x = get<double>(); v.y = get<double>(); v.z = get<double>(); return v; }
            void getFloats(std::vector<float>& v) {
                const uint64_t n = get<uint64_t>();
                if (!ok || n > left / sizeof(float)) { ok = false; return; }
                v.resize((size_t)n);
                std::memcpy(v.data(), p, (size_t)n * sizeof(float));
                p += n * sizeof(float); left -= (size_t)n * sizeof(float);
            }
        };

        void putGrid(Writer& w, const IrGrid& g) {
            w.put((int32_t)g.nx); w.put((int32_t)g.ny); w.put((int32_t)g.nz);
            w.put(g.spacing); w.put((int32_t)g.length); w.put((int32_t)g.sampleRate); w.put((int32_t)g.channels);
            w.putFloats(g.data);
        }

        bool getGrid(Reader& r, IrGrid& g) {
            g.nx = r.get<int32_t>(); g.ny = r.get<int32_t>(); g.nz = r.get<int32_t>();
            g.spacing = r.get<float>(); g.length = r.get<int32_t>(); g.sampleRate = r.get<int32_t>();
            g.channels = r.get<int32_t>();
            r.getFloats(g.data);
            // Konsistenz: Datenmenge muss zu Gittergröße, Kanälen und Länge passen
            return r.ok && g.nx >= 0 && g.ny >= 0 && g.nz >= 0 && g.length >= 0 && g.channels >= 1 && g.channels <= 16 &&
                g.data.size() == (size_t)g.nx * g.ny * g.nz * (size_t)g.channels * (size_t)g.length;
        }

        ProgressFn subRange(const ProgressFn& progress, float from, float to) {
            if (!progress) return nullptr;
            return [progress, from, to](float x) { return progress(from + (to - from) * x); };
        }
    } // namespace

    // ---------------------------------------------------------------- Schlüssel
    uint64_t hashInput(const HybridBakeInput& in) {
        constexpr double kLen = 1e-4;   // Längen und Positionen: 0.1 mm
        constexpr double kVal = 1e-6;   // dimensionslose Werte, Zeiten, Frequenzen
        Fnv f;
        f.add(kFormatVersion);
        f.add((uint32_t)HAC_SA_VERSION);
        // Raum (nur Eingaben; fmax ergibt sich aus der Übergangsfrequenz)
        f.addQ(in.cfg.Lx, kLen); f.addQ(in.cfg.Ly, kLen); f.addQ(in.cfg.Lz, kLen); f.addQ(in.cfg.c, kVal);
        f.addQ(in.cfg.pointsPerLambda, kVal); f.add((int32_t)in.cfg.pmlThickness); f.add((int32_t)in.cfg.spatialOrder);
        // Bake
        const BakeSettings& s = in.settings;
        f.addQ(s.source, kLen); f.addQ(s.irLength, kVal); f.addQ(s.receiverSpacing, kLen);
        f.add((int32_t)s.outRate); f.addQ(s.wallAlpha, kVal);
        f.add((uint64_t)s.innerWalls.size());
        for (const Box& b : s.innerWalls) { f.addQ(b.min, kLen); f.addQ(b.max, kLen); }
        // Steam Audio
        f.add((int32_t)(in.useGeo ? 1 : 0));
        if (in.useGeo) {
            for (float a : in.geo.absorption) f.addQ(a, kVal);
            f.addQ(in.geo.scattering, kVal); f.add((int32_t)in.geo.numRays); f.add((int32_t)in.geo.numDiffuseSamples);
            f.add((int32_t)in.geo.numBounces); f.addQ(in.geo.irradianceMinDistance, kVal); f.add((int32_t)in.geo.order);
        }
        // Übergangsfrequenz
        const CrossoverSettings& x = in.crossover;
        f.add((int32_t)(x.autoFc ? 1 : 0)); f.addQ(x.fixedFc, kVal); f.addQ(x.fcMin, kVal); f.addQ(x.fcMax, kVal);
        f.addQ(x.fmaxFactor, kVal); f.addQ(x.fallbackT60, kVal); f.addQ(x.t60BandLo, kVal); f.addQ(x.t60BandHi, kVal);
        return f.h == 0 ? 1 : f.h;   // 0 ist für "ungültig" reserviert
    }
    // ---------------------------------------------------------------- Bake
    HybridBakeResult runHybridBake(const HybridBakeInput& in, ParallelFor pfor, ProgressFn progress) {
        HybridBakeResult r;
        r.settings = in.settings;
        const float split = in.useGeo ? 0.5f : 0.f;

        // 1) Geometrisch zuerst: hängt nicht von fmax ab und liefert das T60 für die Schroeder-Frequenz
        if (in.useGeo) {
            r.geo = bakeGeometric(in.cfg, in.settings, in.geo, &r.geoStats, subRange(progress, 0.f, split));
            if (r.geoStats.aborted) { r.aborted = true; return r; }
        }

        // 2) T60 -> Schroeder-Frequenz -> Übergangsfrequenz
        const CrossoverSettings& x = in.crossover;
        r.t60Source = T60Source::Geometric;
        r.t60 = r.geo.numReceivers() > 0
            ? medianBandT60(r.geo.data.data(), r.geo.numReceivers(), r.geo.length, r.geo.sampleRate,
                (float)x.t60BandLo, (float)x.t60BandHi, r.geo.channels * r.geo.length)   // nur W/omni
            : -1.f;
        if (r.t60 <= 0.f) {
            r.t60 = (float)eyringT60(in.cfg.Lx, in.cfg.Ly, in.cfg.Lz, in.settings.wallAlpha, in.cfg.c);
            r.t60Source = T60Source::Eyring;
        }
        if (r.t60 <= 0.f) { r.t60 = (float)x.fallbackT60; r.t60Source = T60Source::Fallback; }

        const double V = in.cfg.Lx * in.cfg.Ly * in.cfg.Lz;
        r.fs = (float)schroederFrequency(r.t60, V);
        r.fc = x.autoFc ? (float)std::clamp((double)r.fs, x.fcMin, std::max(x.fcMin, x.fcMax)) : (float)x.fixedFc;

        // 3) FDTD bis fmax = fmaxFactor * fc
        SimulationConfig c = in.cfg;
        c.fmax = r.fc * x.fmaxFactor;
        r.grid = bake(c, in.settings, &r.stats, pfor, subRange(progress, split, 1.f));
        if (r.stats.aborted) { r.aborted = true; return r; }

        r.cfg = c;
        r.cfg.dx = r.stats.dx; r.cfg.dt = r.stats.dt;
        r.cfg.Nx = r.stats.Nx; r.cfg.Ny = r.stats.Ny; r.cfg.Nz = r.stats.Nz;
        return r;
    }

    // ---------------------------------------------------------------- Hilfen: Abstand und Zeitachse
    namespace {
        struct Distances { double grid = 0.0, truth = 0.0; };

        Distances receiverDistances(const HybridBakeResult& r, int receiver) {
            const SimulationConfig& c = r.cfg;
            const Vec3& src = r.settings.source;
            float x, y, z;
            r.grid.position(receiver, x, y, z);
            // FDTD rechnet auf Gitterknoten: Raumkoordinate des Knotens = (round(v/dx + 0.5) - 0.5) dx
            auto snap = [&](double v) { return (std::round(v / c.dx + 0.5) - 0.5) * c.dx; };
            const double ex = snap(x) - snap(src.x), ey = snap(y) - snap(src.y), ez = snap(z) - snap(src.z);
            const double tx = x - src.x, ty = y - src.y, tz = z - src.z;
            Distances d;
            d.grid = std::max(std::sqrt(ex * ex + ey * ey + ez * ez), (double)c.dx);   // Nahfeld: nicht durch 0 teilen
            d.truth = std::sqrt(tx * tx + ty * ty + tz * tz);
            return d;
        }

        // Steam-IR (Zeitachse ab Abstrahlung) -> Zeitachse mit Direktschall bei preDelay; davor stumm, 1 ms Einblendung
        std::vector<float> alignGeo(const float* h, int geoLength, int fs, int outLength, double distTrue, double c,
            double preDelaySec) {
            const long shift = std::lround((distTrue / c - preDelaySec) * fs);
            std::vector<float> out(outLength, 0.f);
            for (int n = 0; n < outLength; ++n) {
                const long m = n + shift;
                if (m >= 0 && m < geoLength) out[n] = h[m];
            }
            const int n0 = std::clamp((int)std::lround(preDelaySec * fs), 0, outLength);
            const int ramp = std::max(1, fs / 1000);
            for (int n = 0; n < std::min(n0 + ramp, outLength); ++n)
                out[n] *= n < n0 ? 0.f : (float)(0.5 - 0.5 * std::cos(3.14159265358979 * (n - n0) / ramp));
            return out;
        }

        void lr4(std::vector<float>& x, double fc, int fs, bool highpass) {
            LinkwitzRiley4 f;
            f.setup(fc, fs, highpass);
            for (float& v : x) v = f.process(v);
        }

        // Kurzzeit-Energie je Fenster (Summe x^2 über win Samples)
        std::vector<double> windowEnergy(const std::vector<float>& x, int win) {
            std::vector<double> e((x.size() + win - 1) / win, 0.0);
            for (size_t i = 0; i < x.size(); ++i) e[i / win] += (double)x[i] * x[i];
            return e;
        }

        // Diffuse Ergänzung eines Richtungskanals: unabhängiges Rauschen mit Fensterenergie max(0, E_W - E_c),
        // Hüllkurve linear zwischen den Fenstermitten interpoliert (keine Sprünge an den Fenstergrenzen)
        void diffuseFillChannel(std::vector<float>& ch, const std::vector<float>& w, int win, unsigned seed, size_t start) {
            const std::vector<double> ew = windowEnergy(w, win), ec = windowEnergy(ch, win);
            std::vector<double> amp(ew.size());
            for (size_t k = 0; k < ew.size(); ++k) amp[k] = std::sqrt(std::max(0.0, ew[k] - ec[k]) / win);   // RMS je Sample
            std::mt19937 rng(seed);
            std::normal_distribution<float> nd(0.f, 1.f);
            for (size_t i = start; i < ch.size(); ++i) {
                const double pos = ((double)i - 0.5 * win) / win;            // Lage relativ zu den Fenstermitten
                const size_t k0 = (size_t)std::clamp(std::floor(pos), 0.0, (double)amp.size() - 1);
                const size_t k1 = std::min(k0 + 1, amp.size() - 1);
                const double t = std::clamp(pos - (double)k0, 0.0, 1.0);
                ch[i] += (float)((amp[k0] * (1.0 - t) + amp[k1] * t) * nd(rng));
            }
        }
    } // namespace

    // ---------------------------------------------------------------- IR für einen Empfänger
    std::vector<float> receiverIr(const HybridBakeResult& r, int receiver, double preDelaySec, float* geoGainOut) {
        const IrGrid& G = r.grid;
        if (receiver < 0 || receiver >= G.numReceivers()) return {};
        const SimulationConfig& c = r.cfg;
        const Distances d = receiverDistances(r, receiver);
        const double distGrid = d.grid, distTrue = d.truth;

        // 1) Tiefton: FDTD-Antwort -> Einheits-Punktquelle, Direktschall bei preDelay
        const std::vector<float> low = fdtdToUnitIr(G.ir(receiver), G.length, G.sampleRate, c.fmax, c.dx, c.dt, c.c,
            r.stats.peak, distGrid, preDelaySec);

        // 2) Hochton: Steam-Audio-Reflexionen auf dieselbe Zeitachse (Direktschall bei preDelay)
        if (r.geo.numReceivers() == G.numReceivers()) {
            const std::vector<float> geo = alignGeo(r.geo.ir(receiver), r.geo.length, G.sampleRate, G.length, distTrue,
                c.c, preDelaySec);
            return makeHybridGeo(low.data(), G.length, geo.data(), G.length, G.sampleRate, r.fc, (float)c.fmax,
                (float)preDelaySec, geoGainOut);
        }
        if (geoGainOut) *geoGainOut = 0.f;
        const float t60 = r.t60 > 0.f ? r.t60 : 0.6f;
        return makeHybrid(low.data(), G.length, G.sampleRate, r.fc, t60, (float)preDelaySec);
    }

    bool receiverAmbiIr(const HybridBakeResult& r, int receiver, double preDelaySec, AmbiIrSet& out, float* geoGainOut,
        bool diffuseFill) {
        const IrGrid& G = r.grid;
        if (receiver < 0 || receiver >= G.numReceivers() || r.geo.numReceivers() != G.numReceivers() || r.geo.channels < 4)
            return false;
        const SimulationConfig& c = r.cfg;
        const Distances d = receiverDistances(r, receiver);

        // Tiefton: entzerrte FDTD-IR; Pegelfaktor g der Steam-IR über den W-Kanal wie in receiverIr
        std::vector<float> low = fdtdToUnitIr(G.ir(receiver), G.length, G.sampleRate, c.fmax, c.dx, c.dt, c.c,
            r.stats.peak, d.grid, preDelaySec);
        const std::vector<float> w = alignGeo(r.geo.ir(receiver, 0), r.geo.length, G.sampleRate, G.length, d.truth, c.c,
            preDelaySec);
        float g = 0.f;
        (void)makeHybridGeo(low.data(), G.length, w.data(), G.length, G.sampleRate, r.fc, (float)c.fmax,
            (float)preDelaySec, &g);
        if (geoGainOut) *geoGainOut = g;

        lr4(low, r.fc, G.sampleRate, false);
        out.low = std::move(low);

        // Hochton: alle Ambisonics-Kanäle gleich behandelt (Verschiebung, Einblendung, g, LR4-Hochpass)
        out.hf.assign(r.geo.channels, {});
        for (int ch = 0; ch < r.geo.channels; ++ch)
            out.hf[ch] = alignGeo(r.geo.ir(receiver, ch), r.geo.length, G.sampleRate, G.length, d.truth, c.c, preDelaySec);

        // Diffuse Ergänzung vor dem Hochpass (Rauschen wird mitgefiltert); erst ab dem Direktschall
        if (diffuseFill) {
            const int win = std::max(1, G.sampleRate / 100);                       // 10 ms
            const size_t start = (size_t)std::lround(preDelaySec * G.sampleRate);
            for (int ch = 1; ch < r.geo.channels; ++ch)
                diffuseFillChannel(out.hf[ch], out.hf[0], win, 1000u * (unsigned)receiver + (unsigned)ch, start);
        }
        for (auto& h : out.hf) {
            lr4(h, r.fc, G.sampleRate, true);
            for (float& v : h) v *= g;
        }
        return true;
    }

    bool receiverStereoIr(const HybridBakeResult& r, int receiver, double preDelaySec, StereoIrSet& out,
        float* geoGainOut) {
        const IrGrid& G = r.grid;
        if (receiver < 0 || receiver >= G.numReceivers() || r.geo.numReceivers() != G.numReceivers()) return false;
        const SimulationConfig& c = r.cfg;
        const Distances d = receiverDistances(r, receiver);

        // Tiefton und Pegelfaktor g genau wie in receiverIr
        std::vector<float> low = fdtdToUnitIr(G.ir(receiver), G.length, G.sampleRate, c.fmax, c.dx, c.dt, c.c,
            r.stats.peak, d.grid, preDelaySec);
        std::vector<float> w = alignGeo(r.geo.ir(receiver, 0), r.geo.length, G.sampleRate, G.length, d.truth, c.c,
            preDelaySec);
        float g = 0.f;
        (void)makeHybridGeo(low.data(), G.length, w.data(), G.length, G.sampleRate, r.fc, (float)c.fmax,
            (float)preDelaySec, &g);
        if (geoGainOut) *geoGainOut = g;
        lr4(low, r.fc, G.sampleRate, false);
        out.low = std::move(low);

        // Rechtes Ohr: unabhängiges Rauschen, Kurzzeit-Energie wie W (Ziel-Energie E_W - 0 = E_W)
        std::vector<float> right(G.length, 0.f);
        diffuseFillChannel(right, w, std::max(1, G.sampleRate / 100), 7919u * (unsigned)receiver + 17u,
            (size_t)std::lround(preDelaySec * G.sampleRate));

        out.hf[0] = std::move(w);
        out.hf[1] = std::move(right);
        for (auto& h : out.hf) {
            lr4(h, r.fc, G.sampleRate, true);
            for (float& v : h) v *= g;
        }
        return true;
    }

    // ---------------------------------------------------------------- Serialisierung
    std::vector<uint8_t> serializeBake(uint64_t key, const HybridBakeResult& r) {
        Writer w;
        w.buf.reserve(64 + (r.grid.data.size() + r.geo.data.size()) * sizeof(float) + 1024);
        w.put(kMagic); w.put(kFormatVersion); w.put(key);

        const SimulationConfig& c = r.cfg;
        w.put((double)c.Lx); w.put((double)c.Ly); w.put((double)c.Lz); w.put((double)c.c);
        w.put((double)c.fmax); w.put((double)c.pointsPerLambda);
        w.put((int32_t)c.pmlThickness); w.put((int32_t)c.spatialOrder); w.put((double)c.dx); w.put((double)c.dt);
        w.put((int32_t)c.Nx); w.put((int32_t)c.Ny); w.put((int32_t)c.Nz);

        const BakeSettings& s = r.settings;
        w.put(s.source); w.put(s.irLength); w.put(s.receiverSpacing); w.put((int32_t)s.outRate); w.put(s.wallAlpha);
        w.put((uint64_t)s.innerWalls.size());
        for (const Box& b : s.innerWalls) { w.put(b.min); w.put(b.max); }

        w.put((float)r.t60); w.put((int32_t)r.t60Source); w.put((float)r.fs); w.put((float)r.fc);
        w.put((double)r.stats.courant); w.put((int32_t)r.stats.steps); w.put((float)r.stats.peak);
        w.put((int32_t)r.geoStats.bounces);

        putGrid(w, r.grid);
        putGrid(w, r.geo);
        return std::move(w.buf);
    }

    uint64_t peekBakeKey(const uint8_t* data, size_t size) {
        Reader rd{ data, size };
        if (rd.get<uint32_t>() != kMagic || rd.get<uint32_t>() != kFormatVersion) return 0;
        const uint64_t key = rd.get<uint64_t>();
        return rd.ok ? key : 0;
    }

    bool deserializeBake(const uint8_t* data, size_t size, uint64_t expectedKey, HybridBakeResult& out, std::string* why) {
        auto fail = [&](const char* reason) { if (why) *why = reason; return false; };
        if (!data || size < 16) return fail("Datei zu klein");
        const uint64_t fileKey = peekBakeKey(data, size);
        if (fileKey == 0) return fail("falsches Magic oder andere Formatversion");
        if (fileKey != expectedKey || expectedKey == 0) return fail("anderer Schlüssel (Eingaben geändert)");

        Reader rd{ data + 16, size - 16 };
        HybridBakeResult r;

        SimulationConfig& c = r.cfg;
        c.Lx = (decltype(c.Lx))rd.get<double>(); c.Ly = (decltype(c.Ly))rd.get<double>();
        c.Lz = (decltype(c.Lz))rd.get<double>(); c.c = (decltype(c.c))rd.get<double>();
        c.fmax = (decltype(c.fmax))rd.get<double>(); c.pointsPerLambda = (decltype(c.pointsPerLambda))rd.get<double>();
        c.pmlThickness = rd.get<int32_t>(); c.spatialOrder = rd.get<int32_t>();
        c.dx = (decltype(c.dx))rd.get<double>(); c.dt = (decltype(c.dt))rd.get<double>();
        c.Nx = rd.get<int32_t>(); c.Ny = rd.get<int32_t>(); c.Nz = rd.get<int32_t>();
        if (!rd.ok) return fail("Kopf (Config) abgeschnitten");

        BakeSettings& s = r.settings;
        s.source = rd.getVec3();
        s.irLength = (decltype(s.irLength))rd.get<double>(); s.receiverSpacing = (decltype(s.receiverSpacing))rd.get<double>();
        s.outRate = rd.get<int32_t>(); s.wallAlpha = (decltype(s.wallAlpha))rd.get<double>();
        const uint64_t nWalls = rd.get<uint64_t>();
        if (!rd.ok || nWalls > 100000) return fail("Settings/Wandanzahl ungültig");
        for (uint64_t i = 0; i < nWalls; ++i) { Box b; b.min = rd.getVec3(); b.max = rd.getVec3(); s.innerWalls.push_back(b); }

        r.t60 = rd.get<float>(); r.t60Source = (T60Source)rd.get<int32_t>(); r.fs = rd.get<float>(); r.fc = rd.get<float>();
        r.stats.courant = (decltype(r.stats.courant))rd.get<double>(); r.stats.steps = rd.get<int32_t>();
        r.stats.peak = (decltype(r.stats.peak))rd.get<float>();
        r.geoStats.bounces = rd.get<int32_t>();
        if (!rd.ok) return fail("Kenndaten abgeschnitten");
        r.stats.Nx = c.Nx; r.stats.Ny = c.Ny; r.stats.Nz = c.Nz;
        r.stats.dx = (decltype(r.stats.dx))c.dx; r.stats.dt = (decltype(r.stats.dt))c.dt;
        if (!getGrid(rd, r.grid)) return fail("FDTD-IRs abgeschnitten oder inkonsistent (Datei unvollständig?)");
        if (!getGrid(rd, r.geo))  return fail("Steam-IRs abgeschnitten oder inkonsistent (Datei unvollständig?)");
        r.stats.receivers = r.grid.numReceivers();
        r.geoStats.probes = r.geo.numReceivers();
        r.geoStats.ok = r.geo.numReceivers() > 0;
        r.fromCache = true;
        out = std::move(r);
        return true;
    }

    // ---------------------------------------------------------------- Dateien
    bool saveBakeFile(const std::string& path, uint64_t key, const HybridBakeResult& r) {
        const std::vector<uint8_t> buf = serializeBake(key, r);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
        return (bool)f;
    }

    bool loadBakeFile(const std::string& path, uint64_t expectedKey, HybridBakeResult& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        const std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return deserializeBake(buf.data(), buf.size(), expectedKey, out);
    }

    std::string describeInput(const HybridBakeInput& in) {
        std::string out;
        char line[320];
        const SimulationConfig& c = in.cfg;
        const BakeSettings& s = in.settings;
        std::snprintf(line, sizeof(line), "Format %u, Steam-Version %u\n", (unsigned)kFormatVersion, (unsigned)HAC_SA_VERSION);
        out += line;
        std::snprintf(line, sizeof(line), "Raum %.6f x %.6f x %.6f m, c %.3f, Punkte/Lambda %.3f, Ordnung %d\n",
            c.Lx, c.Ly, c.Lz, c.c, c.pointsPerLambda, c.spatialOrder);
        out += line;
        std::snprintf(line, sizeof(line), "Quelle %.6f %.6f %.6f m, IR %.4f s, Empfaengerabstand %.4f m, fs %d, alpha %.6f\n",
            s.source.x, s.source.y, s.source.z, s.irLength, s.receiverSpacing, s.outRate, s.wallAlpha);
        out += line;
        for (size_t i = 0; i < s.innerWalls.size(); ++i) {
            const Box& b = s.innerWalls[i];
            std::snprintf(line, sizeof(line), "Wand %d: %.6f %.6f %.6f .. %.6f %.6f %.6f\n", (int)i,
                b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
            out += line;
        }
        std::snprintf(line, sizeof(line), "Steam %d: Absorption %.4f %.4f %.4f, Streuung %.4f, Strahlen %d, Ordnung %d\n",
            in.useGeo ? 1 : 0, in.geo.absorption[0], in.geo.absorption[1], in.geo.absorption[2],
            in.geo.scattering, in.geo.numRays, in.geo.order);
        out += line;
        const CrossoverSettings& x = in.crossover;
        std::snprintf(line, sizeof(line), "Uebergang auto %d, fest %.2f, %.2f..%.2f Hz, Faktor %.3f, FallbackT60 %.3f\n",
            x.autoFc ? 1 : 0, x.fixedFc, x.fcMin, x.fcMax, x.fmaxFactor, x.fallbackT60);
        out += line;
        return out;
    }

} // namespace fdtd
