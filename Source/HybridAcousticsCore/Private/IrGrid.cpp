#include "IrGrid.h"
#include <algorithm>
#include <cmath>

namespace fdtd {

    void IrGrid::position(int idx, float& x, float& y, float& z) const {
        const int ix = idx % nx;
        const int iy = (idx / nx) % ny;
        const int iz = idx / (nx * ny);
        x = (ix + 0.5f) * spacing;
        y = (iy + 0.5f) * spacing;
        z = (iz + 0.5f) * spacing;
    }

    int IrGrid::nearest(float x, float y, float z) const {
        auto q = [&](float v, int n) {
            return std::max(0, std::min((int)std::floor(v / spacing), n - 1));
            };
        return index(q(x, nx), q(y, ny), q(z, nz));
    }

    //wtf is happening

} // namespace fdtd