#pragma once

// A thread calling a poll function until stopped or it returns false (the
// function waits on its own socket, so this doesn't spin). Like HostThread,
// an exception ends the thread, and stop() rethrows it. For inputs beside the
// host's own (the puff stream, D-039).

#include <atomic>
#include <exception>
#include <functional>
#include <thread>
#include <utility>

namespace fovea {

class PollThread {
public:
    explicit PollThread(std::function<bool()> poll) : poll_(std::move(poll)) {}
    ~PollThread() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    PollThread(const PollThread&) = delete;
    PollThread& operator=(const PollThread&) = delete;

    void start() {
        if (running_.exchange(true)) {
            return;
        }
        thread_ = std::thread([this] {
            try {
                while (running_.load(std::memory_order_acquire) && poll_()) {
                }
            } catch (...) {
                error_ = std::current_exception();
                failed_.store(true, std::memory_order_release);
            }
        });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (error_) {
            std::rethrow_exception(std::exchange(error_, nullptr));
        }
    }

    bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    std::function<bool()> poll_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::exception_ptr error_;
    std::thread thread_;
};

}  // namespace fovea
