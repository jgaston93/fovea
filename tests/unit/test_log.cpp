#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/log.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

// Collects what the drain thread hands the sink.
struct Collected {
    std::mutex mutex;
    std::vector<LogRecord> records;

    Logger::Sink sink() {
        return [this](const LogRecord& r) {
            const std::lock_guard lock(mutex);
            records.push_back(r);
        };
    }
};

TEST(Logger, RecordsWrittenBeforeStartAreDeliveredByStop) {
    Logger log(16);
    EXPECT_TRUE(log.write(LogLevel::warn, "value %d of %s", 42, "test"));
    Collected out;
    log.start(out.sink());
    log.stop();
    ASSERT_EQ(out.records.size(), 1U);
    EXPECT_STREQ(out.records[0].text.data(), "value 42 of test");
    EXPECT_EQ(out.records[0].level, LogLevel::warn);
}

TEST(Logger, FullQueueDropsAndCounts) {
    Logger log(4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(log.write(LogLevel::info, "%d", i));
    }
    EXPECT_FALSE(log.write(LogLevel::info, "dropped"));
    EXPECT_EQ(log.dropped(), 1U);
}

TEST(Logger, LongMessagesAreTruncatedAndTerminated) {
    Logger log(4);
    const std::string long_text(1000, 'x');
    EXPECT_TRUE(log.write(LogLevel::info, "%s", long_text.c_str()));
    Collected out;
    log.start(out.sink());
    log.stop();
    ASSERT_EQ(out.records.size(), 1U);
    EXPECT_EQ(std::strlen(out.records[0].text.data()), out.records[0].text.size() - 1);
}

TEST(Logger, ConcurrentWritersKeepTheirOwnOrder) {
    constexpr int kThreads = 4;
    constexpr int kEach = 2000;
    Logger log(16384);
    Collected out;
    log.start(out.sink());
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&log, t] {
            for (int i = 0; i < kEach; ++i) {
                ASSERT_TRUE(log.write(LogLevel::debug, "%d %d", t, i));
            }
        });
    }
    for (auto& w : writers) {
        w.join();
    }
    log.stop();

    ASSERT_EQ(out.records.size(), static_cast<std::size_t>(kThreads * kEach));
    std::vector<int> next(kThreads, 0);
    for (const auto& r : out.records) {
        int t = -1;
        int i = -1;
        ASSERT_EQ(std::sscanf(r.text.data(), "%d %d", &t, &i), 2);
        ASSERT_EQ(i, next[static_cast<std::size_t>(t)]);
        ++next[static_cast<std::size_t>(t)];
    }
}

TEST(Logger, WriteAllocatesNothing) {
    Logger log(64);
    log.write(LogLevel::info, "warm-up");  // first use sets up the thread number
    const test::AllocationCounter counter;
    for (int i = 0; i < 32; ++i) {
        log.write(LogLevel::info, "frame %d took %.3f ms on %s", i, 16.6, "center");
    }
    EXPECT_EQ(counter.count(), 0U);
}

TEST(Logger, ThrowingSinkDropsTheRecordAndKeepsRunning) {
    Logger log(16);
    log.write(LogLevel::info, "first");
    log.write(LogLevel::info, "second");
    int calls = 0;
    log.start([&calls](const LogRecord&) {
        ++calls;
        if (calls == 1) {
            throw std::runtime_error("sink failed");
        }
    });
    log.stop();
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(log.dropped(), 1U);
}

TEST(Logger, CanBeRestarted) {
    Logger log(16);
    Collected out;
    log.start(out.sink());
    log.stop();
    log.write(LogLevel::info, "between runs");
    log.start(out.sink());
    log.stop();
    ASSERT_EQ(out.records.size(), 1U);
    EXPECT_STREQ(out.records[0].text.data(), "between runs");
}

TEST(Logger, StreamSinkFormatsALine) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    LogRecord r;
    r.time_ns = 1'500'000'000;
    r.level = LogLevel::error;
    r.thread = 3;
    std::snprintf(r.text.data(), r.text.size(), "boom");
    make_stream_sink(f)(r);
    std::rewind(f);
    std::array<char, 128> line{};
    ASSERT_NE(std::fgets(line.data(), static_cast<int>(line.size()), f), nullptr);
    std::fclose(f);
    EXPECT_STREQ(line.data(), "    1.500000 error t3 boom\n");
}

}  // namespace
}  // namespace fovea
