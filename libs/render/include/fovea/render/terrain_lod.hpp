#pragma once

// Which terrain tiles a view draws (D-016): from the roots down, a tile
// splits into its children where its geometric error would show on screen
// as more than `pixel_error` pixels, and tiles outside the view are left
// out. CPU only and allocation-free, for the render thread; the renderer
// keeps the tiles and draws what this picks.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "fovea/core/math.hpp"

namespace fovea {

inline constexpr std::uint32_t kNoTile = 0xFFFFFFFFU;

struct LodTile {
    DVec3 centre_ecef{0.0};  // of a sphere around the tile, skirts included
    double radius_m = 0.0;
    double error_m = 0.0;  // the compiler's geometric error (0 at the finest level)
    // Whether it has children that can be loaded (none past the database's
    // finest level, nor past the configured max_level): only then are they
    // wanted, or their absence a miss.
    bool can_split = true;
    // South-west, south-east, north-west, north-east; kNoTile where a child
    // isn't loaded (then the tile never splits).
    std::array<std::uint32_t, 4> children{kNoTile, kNoTile, kNoTile, kNoTile};
};

struct LodView {
    DVec3 eye_ecef{0.0};
    Mat4 ecef_to_clip{1.0F};  // as SceneView: directions relative to the eye
    // Pixels per unit of tangent vertically: render height / tan_height.
    double pixels_per_tangent = 1.0;
    double pixel_error = 1.0;
    // For paging (step 6): where the eye is expected to be shortly, and the
    // error above which a tile's children are wanted (seen from either), so
    // they load before they're needed. 0: request_error is pixel_error.
    std::optional<DVec3> lookahead_ecef;
    double request_error = 0.0;
};

struct TileSelection {
    std::size_t drawn = 0;
    // Tiles drawn whole whose children are wanted: they should split, or
    // soon will (their error past request_error), whether the children are
    // there yet or not (those that are must stay). As many as fit in
    // `wants`.
    std::size_t wanted = 0;
    // Of those, the ones drawn coarser than pixel_error now because their
    // children aren't all there.
    std::size_t misses = 0;
    // Tiles that split (their children used, whether in view or not), as
    // many as fit in `split`.
    std::size_t split = 0;
};

// Error `error_m` seen from `distance_m` away, in pixels.
double screen_error_px(double error_m, double distance_m, double pixels_per_tangent);

// Whether a sphere (centre relative to the eye) is at least partly inside
// the view: in front of the near plane and within the four sides (the far
// plane is at infinity, reverse-Z).
bool sphere_visible(const Mat4& ecef_to_clip, const DVec3& centre_rel_eye, double radius_m);

// Writes the tiles to draw into `out` and returns how many. `stack` is
// scratch; both need room for every tile (tiles.size()). No tile drawn is
// an ancestor of another, and together they cover every visible part of
// the roots.
std::size_t select_tiles(std::span<const LodTile> tiles, std::span<const std::uint32_t> roots,
                         const LodView& view, std::span<std::uint32_t> stack,
                         std::span<std::uint32_t> out);
// The same, also listing the tiles whose children are wanted (most wanted
// first is the caller's to sort: `wants` is in drawing order).
TileSelection select_tiles(std::span<const LodTile> tiles, std::span<const std::uint32_t> roots,
                           const LodView& view, std::span<std::uint32_t> stack,
                           std::span<std::uint32_t> out, std::span<std::uint32_t> wants,
                           std::span<std::uint32_t> split = {});

}  // namespace fovea
