#include "fovea/terrain/height_query_thread.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace fovea {

HeightAnswer answer_height(const TerrainQuery& terrain, const HeightRequest& request) {
    HeightAnswer a;
    a.request = request;
    const std::optional<double> h = terrain.height_at(request.lat_deg, request.lon_deg);
    const std::optional<DVec3> n = terrain.normal_at(request.lat_deg, request.lon_deg);
    if (!h || !n) {
        return a;
    }
    a.valid = true;
    a.terrain_m = *h;
    // East, north, up: azimuth from north towards east, elevation from level.
    a.normal_azimuth_deg = static_cast<float>(glm::degrees(std::atan2(n->x, n->y)));
    a.normal_elevation_deg =
        static_cast<float>(glm::degrees(std::asin(std::clamp(n->z, -1.0, 1.0))));
    return a;
}

HeightQueryThread::HeightQueryThread(const TerrainQuery& terrain, HeightQueries& queries)
    : terrain_(terrain), queries_(queries) {}

HeightQueryThread::~HeightQueryThread() {
    // An exception the thread stored is dropped here (call stop() to get it).
    running_.store(false, std::memory_order_release);
    queries_.notify();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HeightQueryThread::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] {
        try {
            run();
        } catch (...) {
            error_ = std::current_exception();
            failed_.store(true, std::memory_order_release);
        }
    });
}

void HeightQueryThread::run() {
    while (running_.load(std::memory_order_acquire)) {
        // Read before draining: a request posted after the drain changes it,
        // so the wait below returns at once rather than missing it.
        const std::uint32_t seen = queries_.posted.load(std::memory_order_acquire);
        // stop() clears running_ before posting: a stop already counted in
        // `seen` is seen here, or the wait below would miss it.
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        while (const auto r = queries_.requests.try_pop()) {
            const HeightAnswer a = answer_height(terrain_, *r);
            // The asker drains answers every frame: a full ring waits for it.
            while (!queries_.answers.try_push(a)) {
                if (!running_.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        queries_.posted.wait(seen, std::memory_order_acquire);
    }
}

void HeightQueryThread::stop() {
    running_.store(false, std::memory_order_release);
    queries_.notify();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (error_) {
        std::rethrow_exception(std::exchange(error_, nullptr));
    }
}

}  // namespace fovea
