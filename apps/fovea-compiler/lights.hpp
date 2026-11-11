#pragma once

// A runway's lights laid out (milestone 6 step 8, D-033): where each light
// of its systems stands and the colour it shows each way, from the FAA's
// standard, AC 150/5340-30J (tools/fetch-faa-ac.sh puts it in
// docs/reference/), cited by paragraph and figure. The systems are the
// airport file's, as the FAA's NASR data codes them (runways.hpp). Lights
// are in the runway's frame (RunwayPoint); the compiler stands them on its
// surface. Plain C++: tested on made-up runways.
//
// Laid out: edge lights (HIRL, MIRL, LIRL), threshold and runway end
// lights, displaced thresholds, centreline lights, touchdown zone lights
// and PAPIs. Approach lighting systems (MALSR, ALSF-2) are FAA systems
// whose layouts are in FAA JO 6850.2, not this AC; they, REILs and any
// code not known here are listed as skipped.

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "runways.hpp"

namespace fovea::compiler {

// LightColour and LightType are the lights layer's (tile_file.hpp).

struct RunwayLight {
    RunwayPoint at;  // on the ground
    LightType type = LightType::edge;
    // The colour seen by an aircraft landing on (or taking off from) each
    // end, travelling from it toward the other: [0] the first end's, [1]
    // the second's. none: dark that way.
    std::array<LightColour, 2> colour{};
    // A PAPI unit: white above this angle (degrees above level), red below.
    double papi_angle_deg = 0.0;
};

struct RunwayLayout {
    std::vector<RunwayLight> lights;
    std::vector<std::string> skipped;  // systems named but not laid out, and why
};

// `length_m`: the runway's, between its ends (RunwaySurface::length_m).
RunwayLayout layout_lights(const Runway& runway, double length_m);

// What baking them did.
struct LightsReport {
    std::size_t lights = 0;
    std::size_t tiles = 0;             // that hold some
    std::vector<std::string> skipped;  // every runway's, as layout_lights gives them
};

// Lays out each runway's lights and bakes them into the lights layer of
// the tiles at `level` holding them, in the database being built at `part`
// (its finest level, `finest`, written already), each on the ground there
// as built: a fixture's own height (centimetres) isn't added. Errors name
// the runway: a light outside the database.
bool bake_lights(std::span<const RunwaySurface> runways, const std::filesystem::path& part,
                 const DatabaseManifest& manifest, std::uint8_t level, std::uint8_t finest,
                 LightsReport& report, std::vector<std::string>& errors);

}  // namespace fovea::compiler
