#pragma once

// Terrain height queries (Integration 1 step 3, D-036): the host adapter asks
// for the height of the terrain under points, and a query thread answers
// from the database's finest data, away from the render thread. Protocol
// neutral, like the scene state: an adapter turns its requests into these
// and the answers back into its own packets (CIGI's HAT/HOT). Lock-free
// rings both ways, so neither side waits for the other.

#include <atomic>
#include <cstdint>

#include "fovea/core/containers.hpp"

namespace fovea {

struct HeightRequest {
    std::uint64_t tag = 0;  // the asker's, echoed in the answer
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_m = 0.0;  // the point's height above the WGS-84 ellipsoid: the asker's, echoed
};

struct HeightAnswer {
    HeightRequest request;
    bool valid = false;                  // false: outside the database, or no tile reads there
    double terrain_m = 0.0;              // the terrain's height above the WGS-84 ellipsoid
    float normal_azimuth_deg = 0.0F;     // the surface normal: -180 to 180 from true north
    float normal_elevation_deg = 90.0F;  // -90 to 90 from the local level; 90: level ground
};

struct HeightQueries {
    // About ten frames' worth at Aerie's two requests a frame, and a burst
    // of 100 a frame (the plan's load) with room to spare.
    static constexpr std::size_t kCapacity = 1024;

    SpscRing<HeightRequest, kCapacity> requests;  // asker to query thread
    SpscRing<HeightAnswer, kCapacity> answers;    // query thread to asker
    // Bumped after pushing requests, or to stop: the query thread waits on it.
    std::atomic<std::uint32_t> posted{0};

    void notify() {
        posted.fetch_add(1, std::memory_order_release);
        posted.notify_one();
    }
};

}  // namespace fovea
