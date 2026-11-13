#include "engine/renderer/GaussianCloud.h"

#include <algorithm>
#include <limits>

namespace gs {

void GaussianCloud::RecomputeBounds() {
    if (splats.empty()) {
        for (int i = 0; i < 3; ++i) boundsMin[i] = boundsMax[i] = 0.0f;
        return;
    }
    float lo[3] = { std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max() };
    float hi[3] = { -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max() };
    for (const auto& s : splats) {
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], s.position[i]);
            hi[i] = std::max(hi[i], s.position[i]);
        }
    }
    for (int i = 0; i < 3; ++i) { boundsMin[i] = lo[i]; boundsMax[i] = hi[i]; }
}

}  // namespace gs
