#include "fovea/core/log.hpp"

#include <chrono>
#include <cstdarg>
#include <utility>

namespace fovea {

namespace {

std::atomic<std::uint32_t> g_next_thread{1};

std::uint32_t this_thread_number() {
    thread_local const std::uint32_t number = g_next_thread.fetch_add(1, std::memory_order_relaxed);
    return number;
}

}  // namespace

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::debug:
            return "debug";
        case LogLevel::info:
            return "info";
        case LogLevel::warn:
            return "warn";
        case LogLevel::error:
            return "error";
    }
    return "?";
}

Logger::Logger(std::size_t capacity) : origin_(now()), queue_(capacity) {}

Logger::~Logger() {
    stop();
}

void Logger::start(Sink sink) {
    if (running_.exchange(true)) {
        return;
    }
    sink_ = std::move(sink);
    thread_ = std::thread([this] { drain(); });
}

void Logger::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    thread_.join();
}

// NOLINTNEXTLINE(cert-dcl50-cpp): see log.hpp
bool Logger::write(LogLevel level, const char* format, ...) noexcept {
    LogRecord record;
    record.time_ns = (now() - origin_).count();
    record.level = level;
    record.thread = this_thread_number();

    std::va_list args;
    va_start(args, format);
    // Truncation is fine; the record always ends in NUL. The analyzer
    // sometimes misses the va_start above when run over several files.
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    (void)std::vsnprintf(record.text.data(), record.text.size(), format, args);
    va_end(args);

    if (!queue_.try_push(record)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void Logger::drain() {
    LogRecord record;
    for (;;) {
        // Read the flag before draining, so everything pushed before stop()
        // is written by the last pass.
        const bool keep_running = running_.load(std::memory_order_acquire);
        while (queue_.try_pop(record)) {
            sink_(record);
        }
        if (!keep_running) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

Logger::Sink make_stream_sink(std::FILE* stream) {
    return [stream](const LogRecord& r) {
        (void)std::fprintf(stream, "%12.6f %-5s t%u %s\n", static_cast<double>(r.time_ns) * 1e-9,
                           to_string(r.level), r.thread, r.text.data());
        (void)std::fflush(stream);
    };
}

}  // namespace fovea
