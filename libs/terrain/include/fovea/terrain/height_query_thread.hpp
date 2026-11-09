#pragma once

// The query thread (Integration 1 step 3, D-036): answers HeightQueries'
// requests from a TerrainQuery (the database's own reader, not the
// renderer's), off the network and render threads, so a tile read from
// disk delays only the answers waiting on it. It sleeps until requests are
// posted. Like HostThread, an exception ends the thread, and stop()
// rethrows it.

#include <atomic>
#include <exception>
#include <thread>

#include "fovea/core/height_queries.hpp"
#include "fovea/terrain/terrain_query.hpp"

namespace fovea {

// One request's answer: the terrain's height and normal under the point.
HeightAnswer answer_height(const TerrainQuery& terrain, const HeightRequest& request);

class HeightQueryThread {
public:
    // The terrain and the queries must outlive the thread.
    HeightQueryThread(const TerrainQuery& terrain, HeightQueries& queries);
    ~HeightQueryThread();
    HeightQueryThread(const HeightQueryThread&) = delete;
    HeightQueryThread& operator=(const HeightQueryThread&) = delete;

    void start();
    void stop();
    bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    void run();

    const TerrainQuery& terrain_;
    HeightQueries& queries_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::exception_ptr error_;
    std::thread thread_;
};

}  // namespace fovea
