#include "fovea/render/terrain_lod.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fovea {

double screen_error_px(double error_m, double distance_m, double pixels_per_tangent) {
    // An error at right angles to the line of sight spans error / distance
    // in tangent; along it, less: this is the most it can show.
    return error_m * pixels_per_tangent / std::max(distance_m, 1e-3);
}

bool sphere_visible(const Mat4& ecef_to_clip, const DVec3& centre_rel_eye, double radius_m) {
    const Mat4& m = ecef_to_clip;
    const DVec3& c = centre_rel_eye;
    // Clip-space rows (glm is column-major: m[column][row]). A point is
    // inside where w + x, w - x, w + y, w - y and w - z are all at least 0
    // (Vulkan's z from 0 to w; reverse-Z puts the near plane at z = w).
    const auto row = [&](int r) { return glm::dvec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
    const glm::dvec4 x = row(0);
    const glm::dvec4 y = row(1);
    const glm::dvec4 z = row(2);
    const glm::dvec4 w = row(3);
    const std::array<glm::dvec4, 5> planes = {w + x, w - x, w + y, w - y, w - z};
    return std::ranges::all_of(planes, [&](const glm::dvec4& plane) {
        const DVec3 n(plane);
        const double len = glm::length(n);
        return len == 0.0 || (glm::dot(n, c) + plane.w) / len >= -radius_m;
    });
}

std::size_t select_tiles(std::span<const LodTile> tiles, std::span<const std::uint32_t> roots,
                         const LodView& view, std::span<std::uint32_t> stack,
                         std::span<std::uint32_t> out) {
    return select_tiles(tiles, roots, view, stack, out, {}).drawn;
}

TileSelection select_tiles(std::span<const LodTile> tiles, std::span<const std::uint32_t> roots,
                           const LodView& view, std::span<std::uint32_t> stack,
                           std::span<std::uint32_t> out, std::span<std::uint32_t> wants,
                           std::span<std::uint32_t> split) {
    TileSelection sel;
    std::size_t top = 0;
    // Roots pushed in reverse, so they come off in order.
    for (std::size_t i = roots.size(); i-- > 0;) {
        if (top < stack.size()) {
            stack[top++] = roots[i];
        }
    }
    const double request_error = view.request_error > 0.0 ? view.request_error : view.pixel_error;
    while (top > 0) {
        const std::uint32_t t = stack[--top];
        const LodTile& tile = tiles[t];
        const DVec3 c = tile.centre_ecef - view.eye_ecef;
        if (!sphere_visible(view.ecef_to_clip, c, tile.radius_m)) {
            continue;
        }
        const bool all_children = std::none_of(tile.children.begin(), tile.children.end(),
                                               [](std::uint32_t k) { return k == kNoTile; });
        const double distance = glm::length(c) - tile.radius_m;
        const double error = screen_error_px(tile.error_m, distance, view.pixels_per_tangent);
        if (all_children && top + 4 <= stack.size() && error > view.pixel_error) {
            if (sel.split < split.size()) {
                split[sel.split++] = t;
            }
            for (std::size_t k = 4; k-- > 0;) {
                stack[top++] = tile.children[k];
            }
            continue;
        }
        if (sel.drawn < out.size()) {
            out[sel.drawn++] = t;
        }
        if (tile.can_split && tile.error_m > 0.0) {
            double ahead = distance;
            if (view.lookahead_ecef) {
                ahead = std::min(
                    ahead, glm::length(tile.centre_ecef - *view.lookahead_ecef) - tile.radius_m);
            }
            if (screen_error_px(tile.error_m, ahead, view.pixels_per_tangent) > request_error) {
                if (sel.wanted < wants.size()) {
                    wants[sel.wanted++] = t;
                }
                sel.misses += !all_children && error > view.pixel_error ? 1U : 0U;
            }
        }
    }
    return sel;
}

}  // namespace fovea
