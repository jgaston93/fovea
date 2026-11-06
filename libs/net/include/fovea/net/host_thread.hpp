#pragma once

// The network thread: calls a host adapter's poll() until stopped (the
// adapter waits on its socket inside poll(), so this doesn't spin). Like
// FrameLoop, an exception ends the thread, and stop() rethrows it.

#include <atomic>
#include <exception>
#include <thread>

#include "fovea/core/scene.hpp"
#include "fovea/net/host_adapter.hpp"

namespace fovea {

class HostThread {
public:
    // The adapter and scene must outlive the thread.
    HostThread(HostAdapter& adapter, SceneState& scene);
    ~HostThread();
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;

    void start();
    void stop();
    bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    HostAdapter& adapter_;
    SceneState& scene_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::exception_ptr error_;
    std::thread thread_;
};

}  // namespace fovea
