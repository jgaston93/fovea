#pragma once

// A host protocol adapter (CIGI first, DIS later) turns received traffic
// into scene state (foundational decision 5). It runs on the network thread
// and is the only writer of SceneState; nothing downstream sees packets.

#include "fovea/core/scene.hpp"

namespace fovea {

class HostAdapter {
public:
    virtual ~HostAdapter() = default;
    HostAdapter(const HostAdapter&) = delete;
    HostAdapter& operator=(const HostAdapter&) = delete;

    // Handles whatever traffic has arrived, updates the working snapshot and
    // publishes it if anything changed. Returns false once the adapter has
    // stopped (socket closed, end of a replay).
    virtual bool poll(SceneState& scene) = 0;

protected:
    HostAdapter() = default;
};

}  // namespace fovea
