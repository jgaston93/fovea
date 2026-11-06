#include "fovea/net/host_thread.hpp"

#include <utility>

namespace fovea {

HostThread::HostThread(HostAdapter& adapter, SceneState& scene)
    : adapter_(adapter), scene_(scene) {}

HostThread::~HostThread() {
    // An exception the thread stored is dropped here (call stop() to get it).
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HostThread::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] {
        try {
            while (running_.load(std::memory_order_acquire) && adapter_.poll(scene_)) {
            }
        } catch (...) {
            error_ = std::current_exception();
            failed_.store(true, std::memory_order_release);
        }
    });
}

void HostThread::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (error_) {
        std::rethrow_exception(std::exchange(error_, nullptr));
    }
}

}  // namespace fovea
