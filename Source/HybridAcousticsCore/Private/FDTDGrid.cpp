#include "FDTDGrid.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace fdtd {

	namespace {
		// Diffusfeld-Absorptionsgrad einer lokal reagierenden Wand mit normierter Admittanz b (Paris-Integral):
			// alpha_d = 8b [1 + b - b^2/(1+b) - 2b ln(1 + 1/b)]
		double diffuseAlpha(double b) {
			return 8.0 * b * (1.0 + b - b * b / (1.0 + b) - 2.0 * b * std::log(1.0 + 1.0 / b));
			
		}
		
			// Umkehrung per Bisektion; diffuseAlpha ist für b in [0, 1.5] monoton, max. ~0.95
		double admittanceFromAlpha(double alpha) {
			alpha = std::min(std::max(alpha, 0.0), 0.95);
			if (alpha <= 0.0) return 0.0;
			double lo = 0.0, hi = 1.5;
			for (int it = 0; it < 60; ++it) {
				const double mid = 0.5 * (lo + hi);
				(diffuseAlpha(mid) < alpha ? lo : hi) = mid;
				
			}
			return 0.5 * (lo + hi);
			
		}
		
	} // namespace

	double FDTDGrid::courantLimit(int spatialOrder) {
		return spatialOrder == 4 ? 0.5 : 1.0 / std::sqrt(3.0);
	}
	FDTDGrid::FDTDGrid(const SimulationConfig& cfg) : cfg_(cfg) {
		cfg_.spatialOrder = cfg_.spatialOrder == 4 ? 4 : 2;
		cfg_.dx = cfg_.c / (cfg_.fmax * cfg_.pointsPerLambda);

		// zeitschritt berechnen, 0.99 = 2nd Order
		//const double cflFactor = 0.99 / std::sqrt(3.0);
		//cfg_.dt = cfg_.dx * cflFactor / cfg_.c;
		cfg_.dt = 0.99 * courantLimit(cfg_.spatialOrder) * cfg_.dx / cfg_.c;
		const int pad = cfg_.pmlThickness;
		
		cfg_.Nx = static_cast<int>(std::lround(cfg_.Lx / cfg_.dx)) + 2 * pad + 2;
		cfg_.Ny = static_cast<int>(std::lround(cfg_.Ly / cfg_.dx)) + 2 * pad + 2;
		cfg_.Nz = static_cast<int>(std::lround(cfg_.Lz / cfg_.dx)) + 2 * pad + 2;
		//const size_t N = static_cast<size_t>(cfg_.Nx * cfg_.Ny * cfg_.Nz);
		const size_t N = static_cast<size_t>(cfg_.Nx) * cfg_.Ny * cfg_.Nz;
		for (auto& buf : p_)     buf.assign(N, 0.0);
		sigma_.assign(N, 0.0);
		//absorption_.assign(N, 1.0);
		wallMask_.assign(N, 1.0);
		// Äußerste Knotenebene = Spiegelknoten hinter den Außenwänden -> als Wand markieren.
		// 2. Ordnung: Term entfällt, identisch zur Spiegelung p0 = p1 (dort ist p0 - p1 = 0).
		// 4. Ordnung: die Spiegelung in update4thOrder liest so nie über den Rand hinaus.
		for (int i = 0; i < cfg_.Nx; ++i)
			for (int j = 0; j < cfg_.Ny; ++j)
				for (int k = 0; k < cfg_.Nz; ++k)
					if (i == 0 || j == 0 || k == 0 || i == cfg_.Nx - 1 || j == cfg_.Ny - 1 || k == cfg_.Nz - 1)
						wallMask_[idx(i, j, k)] = 0.0;
	}

	void FDTDGrid::setWallAbsorption(double alpha) {
		const double beta = admittanceFromAlpha(alpha);
		const double perWall = cfg_.c * beta / cfg_.dx;      // sigma je angrenzender Wand
		
			    // Erste Luftschicht hinter jeder Wand (Wand liegt zwischen Knoten lo-1 und lo)
		const int lo = cfg_.pmlThickness + 1;
		const int hx = cfg_.Nx - 1 - lo, hy = cfg_.Ny - 1 - lo, hz = cfg_.Nz - 1 - lo;
		
		for (int i = lo; i <= hx; ++i)
			for (int j = lo; j <= hy; ++j)
				for (int k = lo; k <= hz; ++k) {
					const int M = (i == lo) + (i == hx) + (j == lo) + (j == hy) + (k == lo) + (k == hz);
					if (M > 0) sigma_[idx(i, j, k)] = M * perWall;
			
				}
	}

	void FDTDGrid::setupPML()
	{
		const int    d = cfg_.pmlThickness;
		const double dx = cfg_.dx;
		const double c = cfg_.c;
		// σ_max chosen so round-trip attenuation ≈ -80 dB
		const double sigMax = -3.0 * c * std::log(1e-4) / (2.0 * d * dx);

		auto profile = [&](int dist) -> double {
			if (dist >= d) return 0.0;
			double t = static_cast<double>(d - dist) / d;
			return sigMax * t * t;   // quadratic growth toward boundary
			};

		for (int i = 0; i < cfg_.Nx; ++i)
		for (int j = 0; j < cfg_.Ny; ++j)
		for (int k = 0; k < cfg_.Nz; ++k) {
			double s = profile(i) + profile(cfg_.Nx - 1 - i)
				+ profile(j) + profile(cfg_.Ny - 1 - j)
				+ profile(k) + profile(cfg_.Nz - 1 - k);
			sigma_[idx(i, j, k)] = s;
		}
	}

	void FDTDGrid::injectSource(int index, double value)
	{
		p_[now_][index] += value;
	}

	int FDTDGrid::airIndexOf(Vec3 posM)
	{
		const int c0 = indexOf(posM);
		if (wallMask_[c0] != 0.0) return c0;

		// Zellen in wachsenden Würfelschalen absuchen, nächste Luftzelle (euklidisch) gewinnt
		const int i0 = c0 / (cfg_.Ny * cfg_.Nz), j0 = (c0 / cfg_.Nz) % cfg_.Ny, k0 = c0 % cfg_.Nz;
		const int lo = std::max(1, cfg_.pmlThickness + 1);
		for (int r = 1; r <= 8; ++r) {
			int best = -1, bestD = 1 << 30;
			for (int di = -r; di <= r; ++di)
				for (int dj = -r; dj <= r; ++dj)
					for (int dk = -r; dk <= r; ++dk) {
						const int i = i0 + di, j = j0 + dj, k = k0 + dk;
						if (i < lo || j < lo || k < lo || i > cfg_.Nx - 1 - lo || j > cfg_.Ny - 1 - lo || k > cfg_.Nz - 1 - lo)
							continue;
						const int c = idx(i, j, k);
						const int d = di * di + dj * dj + dk * dk;
						if (wallMask_[c] != 0.0 && d < bestD) { bestD = d; best = c; }

					}
			if (best >= 0) return best;
		}
		return c0;
	}

	Vec3i FDTDGrid::toGrid(Vec3 posM) const {
		const int pad = cfg_.pmlThickness;
		return {
			static_cast<int>(std::lround(posM.x / cfg_.dx + 0.5)) + pad,
			static_cast<int>(std::lround(posM.y / cfg_.dx + 0.5)) + pad,
			static_cast<int>(std::lround(posM.z / cfg_.dx + 0.5)) + pad
		};
	}

	void FDTDGrid::setGlobalDamping(double T60)
	{
		if (T60 <= 0.0) return;
		const double sig = 6.0 * std::log(10.0) / T60;      // sigma = 6 ln10 / T60
		for (double& s : sigma_) s += sig;
	}

	// FDTDGrid.cpp
	int FDTDGrid::indexOf(Vec3 posM) const {
		const Vec3i g = toGrid(posM);
		const int lo = std::max(1, cfg_.pmlThickness + 1);          // erster Knoten im Raum
		auto inside = [&](int v, int n) { return std::clamp(v, lo, n - 1 - lo); };
		return idx(inside(g.x, cfg_.Nx), inside(g.y, cfg_.Ny), inside(g.z, cfg_.Nz));
	}

	void FDTDGrid::setWallRegion(Vec3 minCorner, Vec3 maxCorner) {
		// Statt sigma zu erhöhen: Zellen als Wand markieren (material = 1)
		// Diese Zellen werden in jedem Update auf 0 gesetzt
		auto gc0 = toGrid(minCorner);
		auto gc1 = toGrid(maxCorner);

		int x0 = std::clamp(gc0.x, 0, cfg_.Nx - 1);
		int y0 = std::clamp(gc0.y, 0, cfg_.Ny - 1);
		int z0 = std::clamp(gc0.z, 0, cfg_.Nz - 1);
		int x1 = std::clamp(gc1.x, 0, cfg_.Nx - 1);
		int y1 = std::clamp(gc1.y, 0, cfg_.Ny - 1);
		int z1 = std::clamp(gc1.z, 0, cfg_.Nz - 1);

		// Harte Wand: Druckwerte in dieser Region werden nach jedem Schritt auf 0 gesetzt
		int count = 0;
		for (int i = x0; i <= x1; ++i)
			for (int j = y0; j <= y1; ++j)
				for (int k = z0; k <= z1; ++k) {
					wallMask_[idx(i, j, k)] = 0.0;
					++count;
				}
		//std::cout << "[FDTDGrid] setAbsorptionRegion: " << count << " Zellen auf 0 gesetzt\n";
	}

	void FDTDGrid::step()
	{
		/*
		if (cfg_.scheme == Scheme::Order4)
			update4thOrder();
		else*/
		if (cfg_.spatialOrder == 4) update4thOrder();
		else update2ndOrder();

		applyRigidBoundary();

		// Rotate buffer indices (zero allocation)
		int tmp = past_;
		past_ = now_;
		now_ = future_;
		future_ = tmp;
		++stepCount_;
	}

	void FDTDGrid::update2ndOrder()
	{
		const double lam = cfg_.c * cfg_.dt / cfg_.dx;
		const double r2 = lam * lam;
		const int Nx = cfg_.Nx, Ny = cfg_.Ny, Nz = cfg_.Nz;
		const int sx = Ny * Nz, sy = Nz, sz = 1;          // Schrittweiten passend zu idx()

		const auto& pn = p_[now_];
		const auto& pp = p_[past_];
		auto& pf = p_[future_];
		const auto& m = wallMask_;                       // 1 = Luft, 0 = Wand

		// Eine x-Ebene (i fest); schreibt nur in pf dieser Ebene -> parallel sicher
		auto slab = [&](int i) {
			for (int j = 1; j < Ny - 1; ++j)
				for (int k = 1; k < Nz - 1; ++k) {
					const int c0 = idx(i, j, k);
					if (m[c0] == 0.0) { pf[c0] = 0.0; continue; }   // Zelle liegt in einer Wand

					// Nachbar in einer Wand -> Term entfällt -> schallharte Innenwand
					const double pc = pn[c0];
					const double lap =
						m[c0 + sx] * (pn[c0 + sx] - pc) + m[c0 - sx] * (pn[c0 - sx] - pc) +
						m[c0 + sy] * (pn[c0 + sy] - pc) + m[c0 - sy] * (pn[c0 - sy] - pc) +
						m[c0 + sz] * (pn[c0 + sz] - pc) + m[c0 - sz] * (pn[c0 - sz] - pc);

					// zentrierte Dämpfung: stabil für jedes sigma
					const double a = 0.5 * sigma_[c0] * cfg_.dt;
					pf[c0] = (2.0 * pc - (1.0 - a) * pp[c0] + r2 * lap) / (1.0 + a); // *absorption_[c0];
				}
			};

		if (pfor_) pfor_(Nx - 2, [&](int n) { slab(n + 1); });   // i = 1 .. Nx-2
		else for (int i = 1; i < Nx - 1; ++i) slab(i);
	}

	void FDTDGrid::applyRigidBoundary() {
		auto& p = p_[future_];
		const int Nx = cfg_.Nx, Ny = cfg_.Ny, Nz = cfg_.Nz;

		for (int j = 0; j < Ny; ++j)
			for (int k = 0; k < Nz; ++k) {
				p[idx(0, j, k)] = p[idx(1, j, k)];
				p[idx(Nx - 1, j, k)] = p[idx(Nx - 2, j, k)];
			}
		for (int i = 0; i < Nx; ++i)
			for (int k = 0; k < Nz; ++k) {
				p[idx(i, 0, k)] = p[idx(i, 1, k)];
				p[idx(i, Ny - 1, k)] = p[idx(i, Ny - 2, k)];
			}
		for (int i = 0; i < Nx; ++i)
			for (int j = 0; j < Ny; ++j) {
				p[idx(i, j, 0)] = p[idx(i, j, 1)];
				p[idx(i, j, Nz - 1)] = p[idx(i, j, Nz - 2)];
			}
	}

	void FDTDGrid::update4thOrder() {
		const double lam = cfg_.c * cfg_.dt / cfg_.dx;
		const double r2 = lam * lam;
		constexpr double A = 4.0 / 3.0;    // Nachbar +-1
		constexpr double B = -1.0 / 12.0;   // Nachbar +-2
		constexpr double C3 = -7.5;          // Mitte: 3 Achsen * (-5/2)
		const int Nx = cfg_.Nx, Ny = cfg_.Ny, Nz = cfg_.Nz;
		const int sx = Ny * Nz, sy = Nz, sz = 1;

		const auto& pn = p_[now_];
		const auto& pp = p_[past_];
		auto& pf = p_[future_];
		const auto& m = wallMask_;

		// Summe A*(p+1 + p-1) + B*(p+2 + p-2) entlang einer Achse mit Schrittweite s, mit Spiegelung an Wänden
		auto axis = [&](int c0, int s) {
			double sum = 0.0;
			for (int dir : { s, -s }) {
				const int n1 = c0 + dir;
				double p1, p2;
				if (m[n1] == 0.0) {                       // Wand direkt daneben
					p1 = pn[c0];
					const int b = c0 - dir;               // Spiegelbild von +2 ist -1
					p2 = m[b] != 0.0 ? pn[b] : pn[c0];    // eingeklemmte Zelle (beidseitig Wand): p(0)
				}
				else {
					const int n2 = n1 + dir;
					p1 = pn[n1];
					p2 = m[n2] == 0.0 ? pn[n1] : pn[n2];  // Wand bei +2: Spiegelebene zwischen +1 und +2
				}
				sum += A * p1 + B * p2;
			}
			return sum;
			};

		auto slab = [&](int i) {
			for (int j = 1; j < Ny - 1; ++j)
				for (int k = 1; k < Nz - 1; ++k) {
					const int c0 = idx(i, j, k);
					if (m[c0] == 0.0) { pf[c0] = 0.0; continue; }
					const double pc = pn[c0];
					const double lap = axis(c0, sx) + axis(c0, sy) + axis(c0, sz) + C3 * pc;
					// zentrierte Dämpfung wie in 2. Ordnung: p+ = [2p - (1-a) p- + r^2 L] / (1+a), a = sigma dt / 2
					const double a = 0.5 * sigma_[c0] * cfg_.dt;
					pf[c0] = (2.0 * pc - (1.0 - a) * pp[c0] + r2 * lap) / (1.0 + a);
				}
			};

		if (pfor_) pfor_(Nx - 2, [&](int n) { slab(n + 1); });
		else for (int i = 1; i < Nx - 1; ++i) slab(i);
	}

	void FDTDGrid::reset()
	{
		for (auto& buf : p_) std::fill(buf.begin(), buf.end(), 0.0);
		stepCount_ = 0;
		past_ = 0; now_ = 1; future_ = 2;
	}
}