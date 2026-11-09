#pragma once

// The atmosphere the sky is drawn through (milestone 6, D-030): Hillaire's
// model of the Earth's, with its usual values (his 2020 paper's, after
// Bruneton). The shaders' copy is shaders/include/atmosphere.slang; the
// tests check the GPU's tables against a CPU integral of the same model.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace fovea {

struct Atmosphere {
    double bottom_radius_km = 6371.0;  // the WGS-84 mean radius; the eye's height is added
    double top_radius_km = 6471.0;
    std::array<double, 3> rayleigh_scattering{5.802e-3, 13.558e-3, 33.1e-3};  // per km
    double rayleigh_scale_height_km = 8.0;
    double mie_scattering = 3.996e-3;  // per km
    double mie_extinction = 4.440e-3;
    double mie_scale_height_km = 1.2;
    double mie_g = 0.8;
    std::array<double, 3> ozone_absorption{0.650e-3, 1.881e-3, 0.085e-3};  // per km, at the peak
    double ozone_centre_km = 25.0;
    double ozone_half_width_km = 15.0;
    std::array<double, 3> ground_albedo{0.3, 0.3, 0.3};
    // The sun's illuminance at the top of the atmosphere, in the renderer's
    // light units. Only ratios show (exposure follows the light, D-031);
    // the scale keeps the tables' half floats away from both their ends.
    // Lights of fixed brightness assume it (10 lux a unit): the airglow
    // (frame.slang) and models' emission (models.cpp, kNitsPerUnit).
    std::array<double, 3> sun_illuminance{10000.0, 10000.0, 10000.0};
    double sun_angular_radius_deg = 0.267;
};

// The atmosphere with the aerosols (Mie) set for a visibility (D-030):
// Koschmieder's relation, extinction 3.912 / V for the meteorological
// visibility V (where a black object's contrast falls to 2%), taken in green
// light at sea level, the Rayleigh part included. The aerosols keep
// Hillaire's ratio of scattering to extinction (0.9). Clearer air than
// Rayleigh alone allows (about 290 km) leaves no aerosols.
inline Atmosphere with_visibility(Atmosphere a, double visibility_m) {
    const double extinction = 3.912 / (std::max(visibility_m, 1.0) / 1000.0);  // per km
    const double mie = std::max(extinction - a.rayleigh_scattering[1], 0.0);
    a.mie_extinction = mie;
    a.mie_scattering = mie * (3.996 / 4.440);
    return a;
}

// The tables' sizes (the shaders' kTransmittanceSize and so on).
inline constexpr std::uint32_t kTransmittanceWidth = 256;
inline constexpr std::uint32_t kTransmittanceHeight = 64;
inline constexpr std::uint32_t kMultiScatteringSize = 32;
inline constexpr std::uint32_t kSkyViewWidth = 192;
inline constexpr std::uint32_t kSkyViewHeight = 108;
inline constexpr std::uint32_t kAerialWidth = 32;  // the aerial-perspective volume, per view
inline constexpr std::uint32_t kAerialHeight = 32;
inline constexpr std::uint32_t kAerialDepth = 64;
inline constexpr double kAerialDistanceKm = 300.0;  // its far slice's far edge

// What the sky's compute passes made, read back for tests: RGBA rows, top
// first; the view's sky-view table; and its light (pre-exposure).
struct SkyReadback {
    std::vector<float> transmittance;     // kTransmittanceWidth x kTransmittanceHeight
    std::vector<float> multi_scattering;  // kMultiScatteringSize squared
    std::vector<float> sky_view;          // kSkyViewWidth x kSkyViewHeight
    std::vector<float> aerial;            // the view's volume: kAerialDepth slices, nearest first
    std::array<float, 3> sun_irradiance{};
    std::array<float, 3> sky_irradiance{};
    std::array<float, 3> moon_irradiance{};
    float exposure = 0.0F;
};

}  // namespace fovea
