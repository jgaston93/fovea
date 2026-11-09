#pragma once

// Spatial queries against the terrain database (foundational decision 1).
// CPU-only, with no Vulkan or SDL, so a host can link this library and
// query the same surface the IG draws.

#include <cstdint>
#include <optional>

#include "fovea/core/math.hpp"

namespace fovea {

using MaterialId = std::uint16_t;
inline constexpr MaterialId kUnknownMaterial = 0;

struct DRay {
    DVec3 origin{0.0};     // ECEF
    DVec3 direction{0.0};  // ECEF, need not be unit length
};

struct TerrainHit {
    DVec3 point{0.0};         // ECEF
    double distance_m = 0.0;  // from the ray origin
    MaterialId material = kUnknownMaterial;
};

class TerrainQuery {
public:
    virtual ~TerrainQuery() = default;

    // Terrain height above the WGS-84 ellipsoid at a point, or nothing
    // where the database has no data.
    virtual std::optional<double> height_at(double lat_deg, double lon_deg) const = 0;

    // The terrain's upward unit normal at a point, in local east, north and
    // up components, or nothing where height_at has nothing. From the slope
    // of height_at across kNormalStep each way (one way at the database's
    // edge), so on a sampled grid it is the slope of the cell the point is in.
    std::optional<DVec3> normal_at(double lat_deg, double lon_deg) const;

    static constexpr double kNormalStep = 0.5;  // m

    // First terrain hit along the ray within max_distance_m. A ray that
    // starts below the surface hits at its origin.
    virtual std::optional<TerrainHit> intersect(const DRay& ray, double max_distance_m) const = 0;

    // True if terrain does not block the straight line between a and b.
    // Terrain within kEndpointTolerance of either end does not count, so a
    // point sitting on the ground (a vehicle, a runway light) can see and be
    // seen despite rounding and the database's own approximation.
    bool line_of_sight(const DVec3& a, const DVec3& b) const;

    static constexpr double kEndpointTolerance = 0.1;  // m

protected:
    TerrainQuery() = default;
    TerrainQuery(const TerrainQuery&) = default;
    TerrainQuery& operator=(const TerrainQuery&) = default;
};

// Terrain everywhere at one height above the ellipsoid: for tests, and for
// running before a database is loaded. The surface is modelled as an
// ellipsoid with both semi-axes raised by the height, which is not quite a
// constant height: measured, it is off by up to 1.4 mm per km of height
// (worst at 45 degrees latitude), so 0.3 mm at KHSV's 192 m and 14 mm at
// 10 km.
class ConstantHeightTerrain final : public TerrainQuery {
public:
    explicit ConstantHeightTerrain(double height_m) : height_m_(height_m) {}

    std::optional<double> height_at(double lat_deg, double lon_deg) const override;
    std::optional<TerrainHit> intersect(const DRay& ray, double max_distance_m) const override;

private:
    double height_m_;
};

}  // namespace fovea
