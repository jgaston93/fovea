#pragma once

// Compiled-in feature modules, enabled through config (fovea-plan.md, "Core
// services and modules"): rotor downwash, brownout, PAPI, sensor modes. A
// feature that only uses core services is a Module; one that changes the
// main render path or the data format belongs in core.

#include <string_view>

#include "fovea/core/scene.hpp"
#include "fovea/render/frame.hpp"
#include "fovea/render/view.hpp"

namespace fovea {

struct ModuleContext {
    std::string_view name;
};

// GPU time the module may use per frame, summed over all views; the stats
// feed reports the actual cost against it.
struct GpuBudget {
    double ms_per_frame = 0.0;
};

class Module {
public:
    virtual ~Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    virtual void init(const ModuleContext& context) = 0;
    // Once per frame on the render thread: no allocation, no I/O.
    virtual void update(const FrameContext& frame, const SceneSnapshot& scene) = 0;
    // Once per view per frame.
    virtual void record(CommandRecorder& recorder, const ViewDesc& view) = 0;
    virtual void shutdown() = 0;
    virtual GpuBudget budget() const = 0;

protected:
    Module() = default;
};

}  // namespace fovea
