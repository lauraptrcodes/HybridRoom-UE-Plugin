// bindings.cpp — Python-Modul "hybridcore": derselbe Kern wie im Unreal-Plugin
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "Baker.h"
#include "Hybrid.h"
#include "FieldView.h"
#include "HybridBake.h"
#include "DirectPath.h"
#if HAC_WITH_STEAMAUDIO
#include "GeometricBake.h"
#endif
#include "ThreadPool.h"

#include <array>
#include <fstream>
#include <iterator>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
using namespace fdtd;

namespace {
    ThreadPool& pool() { static ThreadPool p; return p; }

    Vec3 toVec3(const std::array<double, 3>& a) { return Vec3{ a[0], a[1], a[2] }; }

    using PyBox = std::pair<std::array<double, 3>, std::array<double, 3>>;
    std::vector<Box> toBoxes(const std::vector<PyBox>& w) {
        std::vector<Box> out;
        for (const auto& b : w) out.push_back(Box{ toVec3(b.first), toVec3(b.second) });
        return out;
    }

    py::array_t<float> irArray(const IrGrid& g) {
        const int R = g.numReceivers();
        py::array_t<float> irs = g.channels > 1
            ? py::array_t<float>({ (py::ssize_t)R, (py::ssize_t)g.channels, (py::ssize_t)g.length })
            : py::array_t<float>({ (py::ssize_t)R, (py::ssize_t)g.length });
        std::copy(g.data.begin(), g.data.end(), irs.mutable_data());
        return irs;
    }

    py::dict gridToDict(const IrGrid& g) {
        const int R = g.numReceivers();
        py::array_t<float> irs = irArray(g);
        py::array_t<float> pos({ (py::ssize_t)R, (py::ssize_t)3 });
        auto p = pos.mutable_unchecked<2>();
        for (int r = 0; r < R; ++r) {
            float x, y, z; g.position(r, x, y, z);
            p(r, 0) = x; p(r, 1) = y; p(r, 2) = z;
        }
        py::dict out;
        out["ir"] = irs; out["positions"] = pos; out["sample_rate"] = g.sampleRate;
        out["shape"] = py::make_tuple(g.nx, g.ny, g.nz); out["spacing"] = g.spacing;
        return out;
    }

    std::vector<float> toFloatVector(const py::array_t<float, py::array::c_style | py::array::forcecast>& a) {
        return std::vector<float>(a.data(), a.data() + a.size());
    }

    py::array_t<float> toArray(const std::vector<float>& v) {
        py::array_t<float> out((py::ssize_t)v.size());
        std::copy(v.begin(), v.end(), out.mutable_data());
        return out;
    }
} // namespace

PYBIND11_MODULE(hybridcore, m) {
    m.doc() = "FDTD-Kern des HybridAcoustics-Plugins (identischer C++-Code wie in Unreal)";

    // ---------------------------------------------------------------- Typen
    py::class_<SimulationConfig>(m, "SimulationConfig")
        .def(py::init([](double Lx, double Ly, double Lz, double fmax, double pointsPerLambda,
            double c, int pmlThickness, int order) {
                SimulationConfig s;
                s.Lx = Lx; s.Ly = Ly; s.Lz = Lz; s.fmax = fmax; s.pointsPerLambda = pointsPerLambda;
                s.c = c; s.pmlThickness = pmlThickness; s.spatialOrder = order;
                return s;
            }),
            py::arg("Lx") = 5.0, py::arg("Ly") = 4.0, py::arg("Lz") = 3.0, py::arg("fmax") = 400.0,
            py::arg("points_per_lambda") = 10.0, py::arg("c") = 343.0, py::arg("pml_thickness") = 0,
            py::arg("order") = 2)
        .def_readwrite("order", &SimulationConfig::spatialOrder)
        .def_readwrite("Lx", &SimulationConfig::Lx)
        .def_readwrite("Ly", &SimulationConfig::Ly)
        .def_readwrite("Lz", &SimulationConfig::Lz)
        .def_readwrite("c", &SimulationConfig::c)
        .def_readwrite("fmax", &SimulationConfig::fmax)
        .def_readwrite("points_per_lambda", &SimulationConfig::pointsPerLambda)
        .def_readwrite("pml_thickness", &SimulationConfig::pmlThickness)
        .def_readonly("dx", &SimulationConfig::dx)
        .def_readonly("dt", &SimulationConfig::dt)
        .def_readonly("Nx", &SimulationConfig::Nx)
        .def_readonly("Ny", &SimulationConfig::Ny)
        .def_readonly("Nz", &SimulationConfig::Nz)
        .def("__repr__", [](const SimulationConfig& s) {
        return "SimulationConfig(L=" + std::to_string(s.Lx) + "x" + std::to_string(s.Ly) + "x" +
            std::to_string(s.Lz) + " m, Ordnung " + std::to_string(s.spatialOrder) +
            ", fmax=" + std::to_string(s.fmax) + " Hz, dx=" +
            std::to_string(s.dx) + ", grid=" + std::to_string(s.Nx) + "x" + std::to_string(s.Ny) +
            "x" + std::to_string(s.Nz) + ")";
            });

    py::class_<BakeSettings>(m, "BakeSettings")
        .def(py::init([](std::array<double, 3> source, double irLength, double receiverSpacing,
            int outRate, double wallAlpha, std::vector<PyBox> walls) {
                BakeSettings b;
                b.source = toVec3(source); b.irLength = irLength; b.receiverSpacing = receiverSpacing;
                b.outRate = outRate; b.wallAlpha = wallAlpha; b.innerWalls = toBoxes(walls);
                return b;
            }),
            py::arg("source") = std::array<double, 3>{ 1.2, 1.0, 1.5 }, py::arg("ir_length") = 0.8,
            py::arg("receiver_spacing") = 0.5, py::arg("out_rate") = 48000, py::arg("wall_alpha") = 0.0,
            py::arg("walls") = std::vector<PyBox>{})
        .def_readwrite("ir_length", &BakeSettings::irLength)
        .def_readwrite("receiver_spacing", &BakeSettings::receiverSpacing)
        .def_readwrite("out_rate", &BakeSettings::outRate)
        .def_readwrite("wall_alpha", &BakeSettings::wallAlpha);

    py::class_<BakeStats>(m, "BakeStats")
        .def_readonly("Nx", &BakeStats::Nx).def_readonly("Ny", &BakeStats::Ny).def_readonly("Nz", &BakeStats::Nz)
        .def_readonly("dx", &BakeStats::dx).def_readonly("dt", &BakeStats::dt)
        .def_readonly("courant", &BakeStats::courant).def_readonly("steps", &BakeStats::steps)
        .def_readonly("receivers", &BakeStats::receivers).def_readonly("peak", &BakeStats::peak)
        .def_readonly("aborted", &BakeStats::aborted);

    // ---------------------------------------------------------------- Simulation
    m.def("simulate_point",
        [](const SimulationConfig& cfg, std::array<double, 3> source, std::array<double, 3> receiver,
            double duration, double wallAlpha) {
                SimulationConfig derived;
                std::vector<double> h;
                {
                    py::gil_scoped_release release;   // Python bleibt während der Rechnung bedienbar
                    h = simulatePoint(cfg, toVec3(source), toVec3(receiver), duration, wallAlpha, &derived,
                        pool().asParallelFor());
                }
                py::array_t<double> ir((py::ssize_t)h.size());
                std::copy(h.begin(), h.end(), ir.mutable_data());
                return py::make_tuple(ir, 1.0 / derived.dt, derived);
        },
        py::arg("cfg"), py::arg("source"), py::arg("receiver"), py::arg("duration"), py::arg("wall_alpha") = 0.0,
        "Ein Empfänger, Rohdaten in Simulationsrate. Rückgabe: (ir, fs_sim, abgeleitete Config)");

    m.def("bake",
        [](const SimulationConfig& cfg, const BakeSettings& bs) {
            BakeStats st;
            IrGrid g;
            {
                py::gil_scoped_release release;
                g = fdtd::bake(cfg, bs, &st, pool().asParallelFor());
            }
            py::dict out = gridToDict(g);
            out["stats"] = st;
            return out;
        },
        py::arg("cfg"), py::arg("settings"),
        "Voller Bake. Rückgabe: dict mit ir (R x L), positions (R x 3), sample_rate, shape, spacing, stats");

    // ---------------------------------------------------------------- Hybrid / Auswertung
    m.def("make_hybrid",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> low, int fs, float fc, float T60,
            float directDelay, float tailGain, unsigned seed) {
                const auto v = toFloatVector(low);
                return toArray(makeHybrid(v.data(), (int)v.size(), fs, fc, T60, directDelay, tailGain, seed));
        },
        py::arg("low_ir"), py::arg("fs"), py::arg("fc"), py::arg("t60"), py::arg("direct_delay"),
        py::arg("tail_gain") = 1.0f, py::arg("seed") = 1u);

    m.def("estimate_t60",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> ir, int fs) {
            return estimateT60(ir.data(), (int)ir.size(), fs);
        },
        py::arg("ir"), py::arg("fs"), "T20-basierte T60 (s); <= 0 wenn nicht messbar");

    m.def("apply_decay",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> ir, int fs, float T60) {
            auto v = toFloatVector(ir);
            applyDecay(v.data(), (int)v.size(), fs, T60);
            return toArray(v);
        },
        py::arg("ir"), py::arg("fs"), py::arg("t60"));

    // ---------------------------------------------------------------- Visualisierung
    m.def("simulate_field",
        [](const SimulationConfig& cfg, std::array<double, 3> source, double z, int frames, int stepsPerFrame,
            double wallAlpha, std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> walls,
            const std::string& excitation, double freq, double interval) {
                FDTDGrid grid(cfg);
                grid.setParallelFor(pool().asParallelFor());
                if (wallAlpha > 0.0) grid.setWallAbsorption(wallAlpha);
                for (const auto& w : walls) grid.setWallRegion(toVec3(w.first), toVec3(w.second));

                VisSource src;
                src.type = excitation == "sine" ? VisSource::Type::Sine : VisSource::Type::Pulse;
                src.fmax = grid.config().fmax; src.freq = freq; src.interval = interval;
                const int si = grid.indexOf(toVec3(source));
                const double dt = grid.config().dt;

                FieldSlice sl;
                extractSliceZ(grid, z, sl);
                py::array_t<float> out({ (py::ssize_t)frames, (py::ssize_t)sl.ny, (py::ssize_t)sl.nx });
                py::array_t<double> mean((py::ssize_t)frames);
                {
                    py::gil_scoped_release release;
                    float* o = out.mutable_data();
                    for (int f = 0; f < frames; ++f) {
                        for (int s = 0; s < stepsPerFrame; ++s) {
                            grid.injectSource(si, src.injection(grid.time(), dt));
                            grid.step();
                        }
                        extractSliceZ(grid, z, sl);
                        std::copy(sl.p.begin(), sl.p.end(), o + (size_t)f * sl.p.size());
                        double m = 0; int cnt = 0;
                        for (size_t q = 0; q < sl.p.size(); ++q) if (!sl.wall[q]) { m += sl.p[q]; ++cnt; }
                        mean.mutable_data()[f] = cnt ? m / cnt : 0.0;
                    }
                }
                py::dict d;
                d["field"] = out; d["dx"] = sl.dx; d["dt"] = dt; d["frame_dt"] = dt * stepsPerFrame;
                d["slice_mean"] = mean; d["cfg"] = grid.config();
                return d;
        },
        py::arg("cfg"), py::arg("source"), py::arg("z"), py::arg("frames"), py::arg("steps_per_frame"),
        py::arg("wall_alpha") = 0.0,
        py::arg("walls") = std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>>{},
        py::arg("excitation") = "pulse", py::arg("freq") = 100.0, py::arg("interval") = 0.0,
        "Draufsicht-Film des Druckfelds. Rückgabe: dict mit field (frames x ny x nx, NaN = Wand), dx, dt, frame_dt");

    m.def("colorize",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> field, float scale, float gamma) {
            if (field.ndim() != 2) throw std::runtime_error("field muss 2D sein (ny x nx)");
            FieldSlice s;
            s.ny = (int)field.shape(0); s.nx = (int)field.shape(1);
            s.p.assign(field.data(), field.data() + field.size());
            s.wall.resize(s.p.size());
            for (size_t q = 0; q < s.p.size(); ++q) s.wall[q] = std::isnan(s.p[q]) ? 1 : 0;
            py::array_t<uint8_t> rgba({ (py::ssize_t)s.ny, (py::ssize_t)s.nx, (py::ssize_t)4 });
            colorizeSlice(s, scale, gamma, rgba.mutable_data());
            auto r = rgba.mutable_unchecked<3>();          // BGRA -> RGBA für matplotlib
            for (int j = 0; j < s.ny; ++j) for (int i = 0; i < s.nx; ++i) std::swap(r(j, i, 0), r(j, i, 2));
            return rgba;
        },
        py::arg("field"), py::arg("scale"), py::arg("gamma") = 0.5f,
        "Gleiche Farbskala wie in Unreal; Rückgabe RGBA uint8 (ny x nx x 4)");

    m.def("make_hybrid_geo",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> low,
            py::array_t<float, py::array::c_style | py::array::forcecast> high,
            int fs, float fc, float fmaxLow, float directDelay) {
                float g = 0.f;
                auto h = makeHybridGeo(low.data(), (int)low.size(), high.data(), (int)high.size(), fs, fc, fmaxLow,
                    directDelay, &g);
                return py::make_tuple(toArray(h), g);
        },
        py::arg("low_ir"), py::arg("high_ir"), py::arg("fs"), py::arg("fc"), py::arg("fmax_low"),
        py::arg("direct_delay"), "Rückgabe: (hybride IR mit Länge von low_ir, Pegelfaktor g der geometrischen IR)");

#if HAC_WITH_STEAMAUDIO
    m.def("bake_geometric",
        [](const SimulationConfig& cfg, const BakeSettings& bs, std::array<float, 3> absorption,
            float scattering, int numRays, int numBounces, int numThreads) {
                GeoSettings gs;
                for (int b = 0; b < 3; ++b) gs.absorption[b] = absorption[b];
                gs.scattering = scattering; gs.numRays = numRays; gs.numBounces = numBounces;
                gs.numThreads = numThreads;
                GeoStats st;
                IrGrid g;
                {
                    py::gil_scoped_release release;
                    g = bakeGeometric(cfg, bs, gs, &st);
                }
                if (!st.ok) throw std::runtime_error("Steam-Audio-Bake fehlgeschlagen");
                py::dict out = gridToDict(g);
                out["bounces"] = st.bounces;
                return out;
        },
        py::arg("cfg"), py::arg("settings"), py::arg("absorption") = std::array<float, 3>{ -1.f, -1.f, -1.f },
        py::arg("scattering") = 0.05f, py::arg("num_rays") = 16384, py::arg("num_bounces") = 0,
        py::arg("num_threads") = 4,
        "Steam-Audio-Reflexions-IRs (ohne Direktschall) am selben Empfängergitter wie bake()");
#endif
    // ---------------------------------------------------------------- Gesamtablauf + Cache
    struct PyBake { std::shared_ptr<HybridBakeResult> r; };
    py::class_<PyBake>(m, "Bake")
        .def("receiver_ir",
            [](const PyBake& b, int receiver, double preDelay) {
                float g = 0.f;
                auto h = receiverIr(*b.r, receiver, preDelay, &g);
                if (h.empty()) throw std::runtime_error("ungültiger Empfänger");
                return py::make_tuple(toArray(h), g);
            },
            py::arg("receiver"), py::arg("pre_delay") = 0.005,
            "Hybrid-IR wie im Plugin (Direktschall = Einheitsimpuls bei pre_delay). Rückgabe: (ir, g_steam)")
        .def("receiver_stereo_ir",
            [](const PyBake& b, int receiver, double preDelay) {
                StereoIrSet s;
                float g = 0.f;
                if (!receiverStereoIr(*b.r, receiver, preDelay, s, &g))
                    throw std::runtime_error("Bake enthält keinen Steam-Audio-Anteil");
                return py::make_tuple(toArray(s.low), toArray(s.hf[0]), toArray(s.hf[1]), g);
            },
            py::arg("receiver"), py::arg("pre_delay") = 0.005,
            "Einfache Dekorrelation. Rückgabe: (low mono, hf_links, hf_rechts, g_steam)")
        .def("receiver_ambi_ir",
            [](const PyBake& b, int receiver, double preDelay) {
                AmbiIrSet s;
                float g = 0.f;
                if (!receiverAmbiIr(*b.r, receiver, preDelay, s, &g))
                    throw std::runtime_error("Bake enthält keine Ambisonics-IRs (order = 1 backen)");
                py::array_t<float> hf({ (py::ssize_t)s.hf.size(), (py::ssize_t)s.low.size() });
                for (size_t c = 0; c < s.hf.size(); ++c)
                    std::copy(s.hf[c].begin(), s.hf[c].end(), hf.mutable_data() + c * s.low.size());
                return py::make_tuple(toArray(s.low), hf, g);
            },
            py::arg("receiver"), py::arg("pre_delay") = 0.005,
            "Rückgabe: (low mono, hf (4 x L) Ambisonics ACN/orthonormal in Steam-Koordinaten, g_steam)");

    auto resultToDict = [](const HybridBakeResult& r) {
        py::dict d = gridToDict(r.grid);
        if (r.geo.numReceivers() > 0) {
            d["geo_ir"] = irArray(r.geo);                  // (R, L) bzw. (R, 4, L) bei Ambisonics
            d["geo_channels"] = r.geo.channels;
        }
        else {
            d["geo_ir"] = py::none();
        }
        static const char* srcNames[] = { "geometric", "eyring", "fallback" };
        d["t60"] = r.t60; d["t60_source"] = srcNames[(int)r.t60Source];
        d["fs"] = r.fs; d["fc"] = r.fc; d["fmax"] = r.cfg.fmax; d["cfg"] = r.cfg;
        d["source"] = py::make_tuple(r.settings.source.x, r.settings.source.y, r.settings.source.z);
        d["wall_alpha"] = r.settings.wallAlpha;
        d["from_cache"] = r.fromCache; d["bounces"] = r.geoStats.bounces;
        return d;
        };

    m.def("hybrid_bake",
        [resultToDict](const SimulationConfig& cfg, const BakeSettings& bs, bool useGeo,
            std::array<float, 3> absorption, float scattering, int numRays, int numThreads, int order,
            bool autoFc, double fixedFc, double fcMin, double fcMax, double fmaxFactor,
            double fallbackT60, const std::string& cachePath) {
                HybridBakeInput in;
                in.cfg = cfg; in.settings = bs; in.useGeo = useGeo;
                for (int b = 0; b < 3; ++b) in.geo.absorption[b] = absorption[b];
                in.geo.scattering = scattering; in.geo.numRays = numRays; in.geo.numThreads = numThreads;
                in.geo.order = order;
                in.crossover.autoFc = autoFc; in.crossover.fixedFc = fixedFc; in.crossover.fcMin = fcMin;
                in.crossover.fcMax = fcMax; in.crossover.fmaxFactor = fmaxFactor; in.crossover.fallbackT60 = fallbackT60;
                const uint64_t key = hashInput(in);

                HybridBakeResult r;
                {
                    py::gil_scoped_release release;
                    if (cachePath.empty() || !loadBakeFile(cachePath, key, r)) {
                        r = runHybridBake(in, pool().asParallelFor());
                        if (!r.aborted && !cachePath.empty()) saveBakeFile(cachePath, key, r);
                    }
                }
                if (r.aborted) throw std::runtime_error("Bake abgebrochen");
                py::dict d = resultToDict(r);
                d["key"] = key;
                d["bake"] = PyBake{ std::make_shared<HybridBakeResult>(std::move(r)) };
                return d;
        },
        py::arg("cfg"), py::arg("settings"), py::arg("use_geo") = true,
        py::arg("absorption") = std::array<float, 3>{ -1.f, -1.f, -1.f }, py::arg("scattering") = 0.05f,
        py::arg("num_rays") = 16384, py::arg("num_threads") = 4, py::arg("order") = 0,
        py::arg("auto_fc") = true, py::arg("fixed_fc") = 200.0, py::arg("fc_min") = 80.0, py::arg("fc_max") = 400.0,
        py::arg("fmax_factor") = 2.0, py::arg("fallback_t60") = 0.6, py::arg("cache_path") = std::string(),
        "Gesamtablauf wie in Unreal (Steam -> T60 -> f_S -> FDTD). Mit cache_path: laden, wenn die Eingaben "
        "gleich sind, sonst backen und speichern.");

    m.def("load_bake",
        [resultToDict](const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            if (!f) throw std::runtime_error("Datei nicht lesbar: " + path);
            const std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const uint64_t key = peekBakeKey(buf.data(), buf.size());
            HybridBakeResult r;
            if (key == 0 || !deserializeBake(buf.data(), buf.size(), key, r))
                throw std::runtime_error("keine gültige Bake-Datei (Format/Version): " + path);
            py::dict d = resultToDict(r);
            d["key"] = key;
            d["bake"] = PyBake{ std::make_shared<HybridBakeResult>(std::move(r)) };
            return d;
        },
        py::arg("path"), "Liest eine Bake-Datei (z. B. aus Unreal: Saved/HybridAcoustics/*.hacbake)");

    m.attr("has_steam_audio") =
#if HAC_WITH_STEAMAUDIO
        true;
#else
        false;
#endif

    m.def("fdtd_to_unit_ir",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> ir, int fs, double fmax, double dx,
            double dt, double c, double peak, double dist, double preDelay, double fLow) {
                return toArray(fdtdToUnitIr(ir.data(), (int)ir.size(), fs, fmax, dx, dt, c, peak, dist, preDelay, fLow));
        },
        py::arg("ir"), py::arg("fs"), py::arg("fmax"), py::arg("dx"), py::arg("dt"), py::arg("c"), py::arg("peak"),
        py::arg("dist"), py::arg("pre_delay"), py::arg("f_low") = 20.0);
    m.def("direct_visibility",
        [](std::array<double, 3> source, std::array<double, 3> listener, std::vector<PyBox> walls,
            double radius, int samples) {
                return directVisibility(toVec3(source), toVec3(listener), toBoxes(walls), radius, samples);
        },
        py::arg("source"), py::arg("listener"), py::arg("walls"), py::arg("source_radius") = 0.25,
        py::arg("num_samples") = 32, "Anteil des unverdeckten Direktschalls (0..1)");
    m.def("eyring_t60", &eyringT60, py::arg("Lx"), py::arg("Ly"), py::arg("Lz"), py::arg("alpha"), py::arg("c") = 343.0);
    m.def("median_band_t60",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> irs, int fs, float fLo, float fHi) {
            if (irs.ndim() != 2) throw std::runtime_error("irs muss 2D sein (Empfänger x Samples)");
            return medianBandT60(irs.data(), (int)irs.shape(0), (int)irs.shape(1), fs, fLo, fHi);
        },
        py::arg("irs"), py::arg("fs"), py::arg("f_lo"), py::arg("f_hi"),
        "Median-T60 im Band [f_lo, f_hi] über alle Zeilen; <= 0 wenn nicht messbar");

    m.def("schroeder_frequency", &schroederFrequency, py::arg("t60"), py::arg("volume"));
    m.def("source_pulse", &sourcePulse, py::arg("t"), py::arg("fmax"));
}
