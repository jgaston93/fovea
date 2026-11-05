#pragma once

// Logging that real-time threads can use: write() formats into a fixed-size
// record and pushes it onto a lock-free queue, with no allocation, locking or
// I/O. A background thread drains the queue into a sink (console, file).
// When the queue is full the record is dropped and counted; the writer never
// waits.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>

#include "fovea/core/containers.hpp"
#include "fovea/core/time.hpp"

#if defined(_MSC_VER)
#include <sal.h>
#define FOVEA_PRINTF_FORMAT_STRING _Printf_format_string_
#define FOVEA_PRINTF_ATTRIBUTE(fmt_index, args_index)
#else
#define FOVEA_PRINTF_FORMAT_STRING
#define FOVEA_PRINTF_ATTRIBUTE(fmt_index, args_index) \
    __attribute__((format(printf, fmt_index, args_index)))
#endif

namespace fovea {

enum class LogLevel : std::uint8_t { debug, info, warn, error };

const char* to_string(LogLevel level);

struct LogRecord {
    std::int64_t time_ns = 0;  // since the logger was created
    LogLevel level = LogLevel::info;
    std::uint32_t thread = 0;      // small per-thread number, in order of first log
    std::array<char, 240> text{};  // NUL-terminated, truncated to fit
};

class Logger {
public:
    // Called on the drain thread only, one record at a time.
    using Sink = std::function<void(const LogRecord&)>;

    // capacity: records held before writes are dropped (a power of two).
    explicit Logger(std::size_t capacity = 4096);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Starts the drain thread. Records written before start() are kept.
    void start(Sink sink);
    // Stops the drain thread after it has written everything queued.
    void stop();

    // printf-style. Returns false if the record was dropped.
    // NOLINTNEXTLINE(cert-dcl50-cpp): C variadics give compile-time format checks
    bool write(LogLevel level, FOVEA_PRINTF_FORMAT_STRING const char* format, ...) noexcept
        FOVEA_PRINTF_ATTRIBUTE(3, 4);

    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    void drain();

    LocalTime origin_;
    MpmcQueue<LogRecord> queue_;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> running_{false};
    Sink sink_;
    std::thread thread_;
};

// A sink that writes "  12.345678 info  t1 text" lines to a C stream (stderr
// or a file the caller opened), flushing after each one.
Logger::Sink make_stream_sink(std::FILE* stream);

}  // namespace fovea
