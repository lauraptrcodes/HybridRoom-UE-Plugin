// GeometricBake.cpp
// Ohne Steam-Audio-SDK (HAC_WITH_STEAMAUDIO = 0) wird nur ein Platzhalter gebaut, der ok = false meldet.
// Dann fällt das Plugin auf den synthetischen Rauschnachhall zurück.
#include "GeometricBake.h"

#if defined(HAC_WITH_STEAMAUDIO) && HAC_WITH_STEAMAUDIO

#if defined(THIRD_PARTY_INCLUDES_START)
THIRD_PARTY_INCLUDES_START
#endif
#include <phonon.h>
#if defined(THIRD_PARTY_INCLUDES_END)
THIRD_PARTY_INCLUDES_END
#endif
#include "SteamContext.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace fdtd {

    namespace {
        // Raumkoordinaten (x, y horizontal, z oben; Händigkeit wie Unreal) -> Steam Audio (rechtshändig, y oben, -z vorn).
        // Gleiche Abbildung wie im offiziellen Steam-Audio-UE-Plugin: (X, Y, Z)_UE -> (Y, Z, -X).
        IPLVector3 toIpl(const Vec3& v) { return IPLVector3{ (float)v.y, (float)v.z, (float)-v.x }; }

        IPLVector3 sub(IPLVector3 a, IPLVector3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        IPLVector3 cross(IPLVector3 a, IPLVector3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        float dot(IPLVector3 a, IPLVector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        struct Mesh {
            std::vector<IPLVector3> v;
            std::vector<IPLTriangle> t;
            std::vector<IPLint32> mat;
        };

        // Achsparalleler Quader als 12 Dreiecke. inward = true: Normalen zeigen ins Innere (Raumhülle),
        // sonst nach außen (Innenwand). Steam Audio: Gegen-Uhrzeigersinn, Normale = (b-a) x (c-a).
        void addBox(Mesh& m, const Box& b, bool inward, int material) {
            const int base = (int)m.v.size();
            Vec3 c[8];
            for (int i = 0; i < 8; ++i)
                c[i] = Vec3{ (i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z };
            IPLVector3 centre = toIpl(Vec3{ 0.5 * (b.min.x + b.max.x), 0.5 * (b.min.y + b.max.y), 0.5 * (b.min.z + b.max.z) });
            for (const Vec3& p : c) m.v.push_back(toIpl(p));

            // je Fläche 4 Ecken (Indizes in c), Reihenfolge beliebig umlaufend
            const int faces[6][4] = {
                { 0, 2, 6, 4 }, { 1, 3, 7, 5 },   // x = min / max
                { 0, 1, 5, 4 }, { 2, 3, 7, 6 },   // y = min / max
                { 0, 1, 3, 2 }, { 4, 5, 7, 6 },   // z = min / max
            };
            for (const auto& f : faces) {
                const int quads[2][3] = { { f[0], f[1], f[2] }, { f[0], f[2], f[3] } };
                for (const auto& q : quads) {
                    IPLTriangle tr{ { base + q[0], base + q[1], base + q[2] } };
                    const IPLVector3 a = m.v[tr.indices[0]], bb = m.v[tr.indices[1]], cc = m.v[tr.indices[2]];
                    const IPLVector3 n = cross(sub(bb, a), sub(cc, a));
                    // gewünschte Richtung: vom Quader-Mittelpunkt weg (außen) bzw. zu ihm hin (innen)
                    const IPLVector3 out = sub(a, centre);
                    const bool pointsOut = dot(n, out) > 0.f;
                    if (pointsOut == inward) std::swap(tr.indices[1], tr.indices[2]);
                    m.t.push_back(tr);
                    m.mat.push_back(material);
                }
            }
        }

        struct ProgressCtx { ProgressFn fn; IPLContext ctx; std::atomic<bool> cancelled{ false }; };

        void IPLCALL onProgress(IPLfloat32 p, void* user) {
            auto* pc = static_cast<ProgressCtx*>(user);
            if (pc->fn && !pc->fn(p) && !pc->cancelled.exchange(true)) iplReflectionsBakerCancelBake(pc->ctx);
        }
    } // namespace

    IrGrid bakeGeometric(const SimulationConfig& cfg, const BakeSettings& bs, const GeoSettings& gs,
        GeoStats* stats, ProgressFn progress) {
        GeoStats st;
        IrGrid out;
        out.spacing = (float)bs.receiverSpacing;
        out.nx = std::max(1, (int)std::floor(cfg.Lx / bs.receiverSpacing));
        out.ny = std::max(1, (int)std::floor(cfg.Ly / bs.receiverSpacing));
        out.nz = std::max(1, (int)std::floor(cfg.Lz / bs.receiverSpacing));
        out.sampleRate = bs.outRate;
        out.length = (int)std::lround(bs.irLength * bs.outRate);
        const int order = std::clamp(gs.order, 0, 1);
        out.channels = (order + 1) * (order + 1);
        const int R = out.numReceivers();
        st.probes = R;

        // --- 1) Kontext und Szene ---
        IPLContextSettings cs = detail::steamContextSettings();   // gleiche Einstellungen wie alle anderen Kontexte!
        IPLContext ctx = nullptr;
        if (iplContextCreate(&cs, &ctx) != IPL_STATUS_SUCCESS) { if (stats) *stats = st; return IrGrid{}; }

        IPLSceneSettings ss{};
        ss.type = IPL_SCENETYPE_DEFAULT;
        IPLScene scene = nullptr;
        iplSceneCreate(ctx, &ss, &scene);

        // Material 0 = Außenwände, Material 1 = Innenwände (gleiche Werte, aber getrennt einstellbar)
        IPLMaterial wall{};
        for (int b = 0; b < 3; ++b) {
            const float a = gs.absorption[b] >= 0.f ? gs.absorption[b] : (float)bs.wallAlpha;
            wall.absorption[b] = std::clamp(a, 0.f, 1.f);
            wall.transmission[b] = 0.f;
        }
        wall.scattering = gs.scattering;
        IPLMaterial materials[2] = { wall, wall };

        Mesh mesh;
        addBox(mesh, Box{ Vec3{ 0, 0, 0 }, Vec3{ cfg.Lx, cfg.Ly, cfg.Lz } }, true, 0);
        for (const Box& w : bs.innerWalls) addBox(mesh, w, false, 1);

        IPLStaticMeshSettings ms{};
        ms.numVertices = (int)mesh.v.size();
        ms.numTriangles = (int)mesh.t.size();
        ms.numMaterials = 2;
        ms.vertices = mesh.v.data();
        ms.triangles = mesh.t.data();
        ms.materialIndices = mesh.mat.data();
        ms.materials = materials;
        IPLStaticMesh staticMesh = nullptr;
        iplStaticMeshCreate(scene, &ms, &staticMesh);
        iplStaticMeshAdd(staticMesh, scene);
        iplSceneCommit(scene);

        // --- 2) Probes = Empfängergitter (gleiche Reihenfolge wie IrGrid) ---
        IPLProbeBatch batch = nullptr;
        iplProbeBatchCreate(ctx, &batch);
        for (int r = 0; r < R; ++r) {
            float x, y, z;
            out.position(r, x, y, z);
            iplProbeBatchAddProbe(batch, IPLSphere{ toIpl(Vec3{ x, y, z }), out.spacing });
        }
        iplProbeBatchCommit(batch);

        // --- 3) Reflexions-Bake: Quelle fest, Hörer an jeder Probe ---
        // Abprallzahl: in T_IR legt der Schall c*T_IR zurück, pro Reflexion im Mittel l = 4V/S
        const double V = cfg.Lx * cfg.Ly * cfg.Lz;
        const double S = 2.0 * (cfg.Lx * cfg.Ly + cfg.Lx * cfg.Lz + cfg.Ly * cfg.Lz);
        st.bounces = gs.numBounces > 0 ? gs.numBounces
            : std::clamp((int)std::ceil(cfg.c * bs.irLength / (4.0 * V / S)), 8, 256);

        IPLBakedDataIdentifier id{};
        id.type = IPL_BAKEDDATATYPE_REFLECTIONS;
        id.variation = IPL_BAKEDDATAVARIATION_STATICSOURCE;
        id.endpointInfluence = IPLSphere{ toIpl(bs.source), (float)(2.0 * std::sqrt(cfg.Lx * cfg.Lx + cfg.Ly * cfg.Ly + cfg.Lz * cfg.Lz)) };

        IPLReflectionsBakeParams bp{};
        bp.scene = scene;
        bp.probeBatch = batch;
        bp.sceneType = IPL_SCENETYPE_DEFAULT;
        bp.identifier = id;
        bp.bakeFlags = IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION;
        bp.numRays = gs.numRays;
        bp.numDiffuseSamples = gs.numDiffuseSamples;
        bp.numBounces = st.bounces;
        bp.simulatedDuration = (float)bs.irLength;
        bp.savedDuration = (float)bs.irLength;
        bp.order = order;                   // 0 = omni wie die FDTD-Empfänger, 1 = mit Einfallsrichtung
        bp.numThreads = std::max(1, gs.numThreads);
        bp.rayBatchSize = 1;
        bp.irradianceMinDistance = gs.irradianceMinDistance;
        bp.bakeBatchSize = 1;

        ProgressCtx pc{ progress, ctx };
        iplReflectionsBakerBake(ctx, &bp, &onProgress, &pc);
        st.aborted = pc.cancelled.load();

        // --- 4) Energiefeld je Probe -> IR rekonstruieren ---
        if (!st.aborted) {
            IPLEnergyFieldSettings es{ (float)bs.irLength, order };
            IPLEnergyField ef = nullptr;
            iplEnergyFieldCreate(ctx, &es, &ef);

            IPLReconstructorSettings rs{ (float)bs.irLength, order, bs.outRate };
            IPLReconstructor rec = nullptr;
            iplReconstructorCreate(ctx, &rs, &rec);

            IPLImpulseResponseSettings is{ (float)bs.irLength, order, bs.outRate };
            IPLImpulseResponse ir = nullptr;
            iplImpulseResponseCreate(ctx, &is, &ir);

            const int n = std::min(out.length, (int)iplImpulseResponseGetNumSamples(ir));
            out.data.assign((size_t)R * out.channels * out.length, 0.f);
            IPLReconstructorInputs in{ ef };
            IPLReconstructorSharedInputs shared{ (float)bs.irLength, order };
            IPLReconstructorOutputs o{ ir };
            for (int r = 0; r < R; ++r) {
                iplEnergyFieldReset(ef);
                iplProbeBatchGetEnergyField(batch, &id, r, ef);
                iplReconstructorReconstruct(rec, 1, &in, &shared, &o);
                for (int ch = 0; ch < out.channels; ++ch) {
                    const float* h = iplImpulseResponseGetChannel(ir, ch);
                    std::copy(h, h + n, out.data.begin() + ((size_t)r * out.channels + ch) * out.length);
                }
            }
            iplImpulseResponseRelease(&ir);
            iplReconstructorRelease(&rec);
            iplEnergyFieldRelease(&ef);
            st.ok = true;
        }

        iplProbeBatchRelease(&batch);
        iplStaticMeshRemove(staticMesh, scene);
        iplStaticMeshRelease(&staticMesh);
        iplSceneRelease(&scene);
        iplContextRelease(&ctx);

        if (stats) *stats = st;
        return st.ok ? out : IrGrid{};
    }

} // namespace fdtd

#else // ohne Steam Audio

namespace fdtd {
    IrGrid bakeGeometric(const SimulationConfig&, const BakeSettings&, const GeoSettings&, GeoStats* stats, ProgressFn) {
        if (stats) *stats = GeoStats{};
        return IrGrid{};
    }
} // namespace fdtd

#endif
