#pragma once

// Runways smoothed in the elevation (milestone 4 step 1, D-022). Inside
// each runway's rectangle the finest level's heights are replaced by a
// smooth surface fitted to the sources there: along the runway, a local
// regression over a window of stations (keeping its grade changes); across
// it, a profile with a crown (constant, linear, |v| and v^2 terms, so a
// peaked or rounded crown and a cross slope both fit). Robust weights let
// what stood on the runway when it was surveyed fall out of the fit.
// Beyond the rectangle the heights are the sources', plus the runway's
// correction at its nearest edge point (the surface less the sources
// there, millimetres to centimetres) fading to nothing over `blend_m`: no
// step at the edge, and the shoulders keep their own slope. Coarser levels
// are filtered from the finest as before, so their errors stay true.
//
// Plain C++ (no GDAL): the heights come from a callback, so the fit is
// tested on made-up ground.

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fovea/core/math.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea::compiler {

struct RunwayEnd {
    std::string id;  // "18R"
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double elevation_m = 0.0;  // surveyed, orthometric (NAVD88 for the FAA's)
};

struct Runway {
    std::string name;  // "18R/36L"
    std::array<RunwayEnd, 2> ends;
    double width_m = 0.0;
    double length_m = 0.0;  // as published, if it is: checked against the ends
};

// "34-39.167130N", "086-46.926028W": degrees, minutes and a hemisphere, as
// the FAA publishes them. Nothing if malformed. `latitude` picks N/S or E/W.
std::optional<double> parse_degrees_minutes(std::string_view text, bool latitude);

// Heights (ellipsoidal, metres) at points; false, with why, when one has
// none.
using PointHeights =
    std::function<bool(std::span<const double> lat_deg, std::span<const double> lon_deg,
                       std::vector<float>& out, std::string& error)>;

// A point in a runway's frame: metres along it from the first end, and
// across it (positive to the right, looking from the first end to the
// second).
struct RunwayPoint {
    double along_m = 0.0;
    double across_m = 0.0;
};

class RunwaySurface {
public:
    // Fits the surface to `heights` sampled every metre over the runway.
    static std::optional<RunwaySurface> fit(const Runway& runway, double blend_m,
                                            const PointHeights& heights, std::string& error);

    const Runway& runway() const { return runway_; }
    double length_m() const { return length_; }
    // Whether the rectangle or its blend reaches into `b`.
    bool touches(const GeoBounds& b) const;
    const GeoBounds& reach() const { return reach_; }
    RunwayPoint to_runway(double lat_deg, double lon_deg) const;
    // The fitted surface (ellipsoidal) at a point of the rectangle; points
    // outside take its nearest point's.
    double surface(double along_m, double across_m) const;
    // The height to use at a point whose sources say `raw`: the surface on
    // the runway; beyond it `raw` plus the edge's correction, fading out
    // over the blend distance.
    double blended(double lat_deg, double lon_deg, double raw) const;

    // How the fit went: the sources' heights on the runway against it.
    double rms_m() const { return rms_; }
    double max_abs_m() const { return max_abs_; }

private:
    RunwaySurface() = default;
    struct Station {
        double a = 0.0;  // h = a + b v + c |v| + d v^2 at the station
        double b = 0.0;
        double c = 0.0;
        double d = 0.0;
    };
    DVec3 to_ecef(double along_m, double across_m) const;
    double source_at(double along_m, double across_m) const;  // on the rectangle

    Runway runway_;
    double blend_ = 0.0;
    double length_ = 0.0;
    DVec3 origin_ecef_{0.0};   // the first end, on the ellipsoid
    DMat3 to_local_{1.0};      // ECEF to north, east, down at the midpoint
    double axis_north_ = 0.0;  // the along direction's north and east parts
    double axis_east_ = 0.0;
    GeoBounds reach_;  // the rectangle and its blend
    double station_step_ = 0.0;
    std::vector<Station> stations_;
    // The sources as sampled for the fit: every metre over the rectangle.
    std::vector<float> sources_;
    std::size_t nu_ = 0;
    std::size_t nv_ = 0;
    double du_ = 0.0;
    double dv_ = 0.0;
    double rms_ = 0.0;
    double max_abs_ = 0.0;
};

// Each runway's surface into a finest-level tile's heights (samples x
// samples, row 0 south, ellipsoidal), where it reaches.
void apply_runways(std::span<const RunwaySurface> runways, const TileKey& key,
                   std::uint32_t samples, std::vector<float>& heights);

}  // namespace fovea::compiler
