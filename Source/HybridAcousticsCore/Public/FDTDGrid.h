#pragma once

#include "Types.h"
#include <functional>
#include <vector>

namespace fdtd {


class FDTDGrid {
public:
    using ParallelFor = std::function<void(int count, const std::function<void(int)>& body)>;
	explicit FDTDGrid(const SimulationConfig& cfg);

    void setWallAbsorption(double alpha);
    void setupPML();
    void injectSource(int index, double value);
    void step();
    void setParallelFor(ParallelFor pfor) { pfor_ = std::move(pfor); }
    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] const SimulationConfig& config() const { return cfg_; }
    [[nodiscard]] long long               stepCount() const { return stepCount_; }
    [[nodiscard]] double                  time()      const { return stepCount_ * cfg_.dt; }
    [[nodiscard]] double                  pressureAt(int index) const { return p_[now_][index]; }
    [[nodiscard]] int                     indexOf(Vec3 posM) const;
    [[nodiscard]] int                     index(int i, int j, int k) const { return idx(i, j, k); }
    [[nodiscard]] bool                    isWall(int index) const { return wallMask_[index] == 0.0; }
    [[nodiscard]] int                     airIndexOf(Vec3 posM);
    [[nodiscard]] static double           courantLimit(int spatialOrder);

    [[nodiscard]] Vec3i toGrid(Vec3 posM) const;
    void setGlobalDamping(double T60);
    void setWallRegion(Vec3 minCorner, Vec3 maxCorner); //für tests
    void setAbsorptionRegion(Vec3 minCorner, Vec3 maxCorner, double alpha = 1.0);
    void reset();
private:
	void update2ndOrder();
    void update4thOrder();
    void applyRigidBoundary();

	SimulationConfig cfg_;
    ParallelFor pfor_;
    long long stepCount_{ 0 };

    // Three pressure buffers, rotated each step (no copying).
    std::vector<double> p_[3];
    int past_{ 0 }, now_{ 1 }, future_{ 2 };

    // PML damping coefficients σ(x,y,z).
    std::vector<double> sigma_;

    // Per-cell absorption multiplier (1.0 = no extra absorption).
    //std::vector<double> absorption_;
    // Wall mask: 1.0 = frei, 0.0 = Wand (Druck wird auf 0 gesetzt)
    std::vector<double> wallMask_;
    
    [[nodiscard]] inline int idx(int x, int y, int z) const {
        return x * cfg_.Ny * cfg_.Nz + y * cfg_.Nz + z;
    }
    //[[nodiscard]] Vec3i toGrid(Vec3 posM) const;

};

}