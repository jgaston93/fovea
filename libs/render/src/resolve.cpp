#include "fovea/render/resolve.hpp"

#include <algorithm>

namespace fovea {

float luminance(const Vec3& c) {
    return std::max(0.0F, 0.2126F * c.x + 0.7152F * c.y + 0.0722F * c.z);
}

Vec3 weighted_resolve(std::span<const Vec3> samples) {
    Vec3 sum(0.0F);
    float weights = 0.0F;
    for (const Vec3& c : samples) {
        const float w = 1.0F / (1.0F + luminance(c));
        sum += c * w;
        weights += w;
    }
    return weights > 0.0F ? sum / weights : Vec3(0.0F);
}

}  // namespace fovea
