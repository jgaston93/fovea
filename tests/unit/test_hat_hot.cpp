// HAT/HOT (Integration 1 step 3, D-036): the terrain's normal, the query
// thread, and the CIGI adapter answering HAT/HOT Requests from a database,
// end to end over loopback.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/core/height_queries.hpp"
#include "fovea/net/cigi.hpp"
#include "fovea/net/cigi_adapter.hpp"
#include "fovea/net/udp.hpp"
#include "fovea/terrain/database.hpp"
#include "fovea/terrain/height_query_thread.hpp"
#include "fovea/terrain/terrain_query.hpp"
#include "support/alloc_counter.hpp"
#include "support/terrain_db.hpp"

namespace fovea {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kLoopback = 0x7F000001U;
constexpr double kGeoid = -29.55;  // KHSV's (D-035)

// A tilted plane, exact under bilinear sampling: rising north, falling east.
double plane(double lat, double lon) {
    return 200.0 + 3000.0 * (lat - 34.6) - 1500.0 * (lon + 86.8);
}

const GeoBounds kArea{34.60, 34.67, -86.80, -86.74};

const test::TestDb& plane_db() {
    static const test::TestDb db =
        test::make_db("hat_hot_plane", kArea, {10, 13}, 33,
                      [](std::uint8_t, double a, double o) { return plane(a, o); });
    return db;
}

// Metres a degree north and east here, measured on the ellipsoid (not from
// the radii normal_at uses).
std::pair<double, double> metres_per_degree(double lat, double lon) {
    const double d = 1e-4;
    const DVec3 o = geodetic_to_ecef({lat, lon, 0.0});
    return {glm::length(geodetic_to_ecef({lat + d, lon, 0.0}) - o) / d,
            glm::length(geodetic_to_ecef({lat, lon + d, 0.0}) - o) / d};
}

TEST(TerrainNormal, FlatGroundFacesUp) {
    const ConstantHeightTerrain flat(190.0);
    const auto n = flat.normal_at(34.64, -86.77);
    ASSERT_TRUE(n.has_value());
    EXPECT_NEAR(n->x, 0.0, 1e-12);
    EXPECT_NEAR(n->y, 0.0, 1e-12);
    EXPECT_NEAR(n->z, 1.0, 1e-12);
    const HeightAnswer a = answer_height(flat, {7, 34.64, -86.77, 300.0});
    EXPECT_TRUE(a.valid);
    EXPECT_EQ(a.request.tag, 7U);
    EXPECT_DOUBLE_EQ(a.terrain_m, 190.0);
    EXPECT_FLOAT_EQ(a.normal_elevation_deg, 90.0F);
}

TEST(TerrainNormal, APlanesNormalFromItsSlope) {
    auto db = test::open_db(plane_db());
    const DatabaseTerrain terrain(*db);
    for (const auto& [lat, lon] : {std::pair{34.63, -86.77}, {34.6005, -86.7995}}) {
        // The second point is within a step of the database's south-west
        // corner: its slope is taken one way.
        SCOPED_TRACE(testing::Message() << lat << ", " << lon);
        const auto [north_m, east_m] = metres_per_degree(lat, lon);
        const DVec3 expected = glm::normalize(DVec3{1500.0 / east_m, -3000.0 / north_m, 1.0});
        const auto n = terrain.normal_at(lat, lon);
        ASSERT_TRUE(n.has_value());
        EXPECT_NEAR(n->x, expected.x, 1e-5);
        EXPECT_NEAR(n->y, expected.y, 1e-5);
        EXPECT_NEAR(n->z, expected.z, 1e-6);
        // Facing south-east (down the slope) and tilted 1.8 degrees from up.
        const HeightAnswer a = answer_height(terrain, {0, lat, lon, 0.0});
        ASSERT_TRUE(a.valid);
        EXPECT_NEAR(a.normal_azimuth_deg, glm::degrees(std::atan2(expected.x, expected.y)), 1e-3);
        EXPECT_NEAR(a.normal_elevation_deg, glm::degrees(std::asin(expected.z)), 1e-3);
        EXPECT_GT(a.normal_azimuth_deg, 90.0F);
        EXPECT_LT(a.normal_azimuth_deg, 180.0F);
    }
    EXPECT_FALSE(terrain.normal_at(40.0, -86.77).has_value());  // outside the tiles
    EXPECT_FALSE(answer_height(terrain, {0, 40.0, -86.77, 0.0}).valid);
}

TEST(HeightQueryThread, AnswersEveryRequestThenStops) {
    auto db = test::open_db(plane_db());
    const DatabaseTerrain terrain(*db);
    auto queries = std::make_unique<HeightQueries>();
    HeightQueryThread thread(terrain, *queries);
    thread.start();
    // More than the rings hold: the thread keeps answering as they drain.
    constexpr std::uint64_t kCount = 3000;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t invalid = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received < kCount && std::chrono::steady_clock::now() < deadline) {
        while (sent < kCount) {
            // Every tenth point is outside the database.
            const double lat =
                sent % 10 == 9 ? 40.0 : 34.61 + 0.05 * static_cast<double>(sent) / kCount;
            if (!queries->requests.try_push({sent, lat, -86.77, 0.0})) {
                break;
            }
            ++sent;
        }
        queries->notify();
        while (const auto a = queries->answers.try_pop()) {
            ASSERT_EQ(a->request.tag, received);  // in order
            if (a->valid) {
                EXPECT_NEAR(a->terrain_m, plane(a->request.lat_deg, -86.77), 1e-3);
            } else {
                ++invalid;
            }
            ++received;
        }
    }
    thread.stop();
    EXPECT_EQ(received, kCount);
    EXPECT_EQ(invalid, kCount / 10);
    EXPECT_FALSE(thread.failed());
}

// --- The adapter, end to end ---

struct Answers {
    std::vector<cigi::HatHotResponse> responses;
    std::vector<cigi::HatHotExtendedResponse> extended;
    std::size_t count() const { return responses.size() + extended.size(); }
};

// A host speaking network order to an adapter with a query thread over the
// plane database (or none), and a render loop's frame starts.
struct Rig {
    std::unique_ptr<TerrainDatabase> db;
    std::unique_ptr<DatabaseTerrain> terrain;
    std::unique_ptr<HeightQueries> queries;
    std::unique_ptr<HeightQueryThread> thread;
    std::unique_ptr<SceneState> scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    UdpSocket host{0, kLoopback};
    std::unique_ptr<CigiAdapter> adapter;
    std::uint64_t frame = 0;
    std::uint32_t host_frame = 0;
    Answers answers;
    std::uint64_t allocations = 0;  // made inside the adapter's poll()

    explicit Rig(bool with_terrain = true) {
        CigiAdapterOptions o;
        o.address = kLoopback;
        o.geoid_height_m = kGeoid;
        if (with_terrain) {
            db = test::open_db(plane_db());
            terrain = std::make_unique<DatabaseTerrain>(*db);
            queries = std::make_unique<HeightQueries>();
            thread = std::make_unique<HeightQueryThread>(*terrain, *queries);
            thread->start();
            o.heights = queries.get();
        }
        adapter = std::make_unique<CigiAdapter>(o, &frames);
        send({});  // the ICD's startup: Operate asked for alone (2.8)
        poll_for(std::chrono::milliseconds(20));
    }
    ~Rig() {
        adapter.reset();
        if (thread) {
            thread->stop();
        }
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // A host message in network order: IG Control (Operate), then packets.
    void send(const std::vector<cigi::HatHotRequest>& requests,
              const std::vector<cigi::EntityControl>& entities = {}) {
        const bool swap = cigi::swaps(cigi::ByteOrder::network);
        Bytes out(cigi::kIgControlSize + entities.size() * cigi::kEntityControlSize +
                  requests.size() * cigi::kHatHotRequestSize);
        cigi::IgControl ig;
        ig.ig_mode = cigi::IgMode::operate;
        ig.host_frame_number = ++host_frame;
        std::size_t n = cigi::encode(ig, out, swap);
        for (const auto& e : entities) {
            n += cigi::encode(e, std::span(out).subspan(n), swap);
        }
        for (const auto& r : requests) {
            n += cigi::encode(r, std::span(out).subspan(n), swap);
        }
        ASSERT_TRUE(host.send_to(out, {kLoopback, adapter->port()}));
    }

    void poll_for(std::chrono::milliseconds d) {
        const auto end = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < end) {
            poll();
            receive();
        }
    }

    // One IG frame: a frame start, Start of Frame and any answers ready.
    void step_frame() {
        (void)frames.try_push({frame++, now()});
        poll();
        (void)host.wait_readable(5);
        receive();
    }

    // Frames until `n` answers in all have come, or two seconds pass.
    bool frames_until(std::size_t n) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (answers.count() < n && std::chrono::steady_clock::now() < end) {
            step_frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return answers.count() >= n;
    }

    void poll() {
        const test::AllocationCounter counter;
        (void)adapter->poll(*scene);
        allocations += counter.count();
    }

    void receive() {
        std::array<std::uint8_t, 65536> buffer{};
        while (const auto r = host.receive(buffer)) {
            const auto message = std::span(buffer).first(r->size);
            cigi::MessageReader reader(message, cigi::kStartOfFrameId);
            EXPECT_EQ(reader.error(), cigi::MessageError::none);
            cigi::MessageReader::Packet p;
            while (reader.next(p)) {
                if (const auto a = cigi::decode_hat_hot_response(p.bytes, reader.swapped())) {
                    answers.responses.push_back(*a);
                }
                if (const auto a =
                        cigi::decode_hat_hot_extended_response(p.bytes, reader.swapped())) {
                    answers.extended.push_back(*a);
                }
            }
        }
    }
};

cigi::HatHotRequest geodetic(std::uint16_t id, cigi::HatHotRequestType type, double lat, double lon,
                             double msl, std::uint8_t period = 0) {
    cigi::HatHotRequest r;
    r.hat_hot_id = id;
    r.request_type = type;
    r.update_period = period;
    r.lat_or_x = lat;
    r.lon_or_y = lon;
    r.alt_or_z = msl;
    return r;
}

TEST(HatHot, EachKindIsAnsweredFromTheDatabase) {
    Rig rig;
    const double lat = 34.637;
    const double lon = -86.775;
    const double msl = 500.0;
    const double hot = plane(lat, lon) - kGeoid;  // the terrain above mean sea level
    rig.send({geodetic(1, cigi::HatHotRequestType::hat, lat, lon, msl),
              geodetic(2, cigi::HatHotRequestType::hot, lat, lon, msl),
              geodetic(3, cigi::HatHotRequestType::extended, lat, lon, msl),
              geodetic(4, cigi::HatHotRequestType::extended, 40.0, lon, msl)});  // outside
    ASSERT_TRUE(rig.frames_until(4));
    ASSERT_EQ(rig.answers.responses.size(), 2U);
    ASSERT_EQ(rig.answers.extended.size(), 2U);
    const auto& r_hat = rig.answers.responses[0];
    const auto& r_hot = rig.answers.responses[1];
    EXPECT_EQ(r_hat.hat_hot_id, 1);
    EXPECT_TRUE(r_hat.valid);
    EXPECT_FALSE(r_hat.hot);
    EXPECT_NEAR(r_hat.height_m, msl - hot, 1e-3);
    EXPECT_EQ(r_hot.hat_hot_id, 2);
    EXPECT_TRUE(r_hot.valid);
    EXPECT_TRUE(r_hot.hot);
    EXPECT_NEAR(r_hot.height_m, hot, 1e-3);
    const auto& x = rig.answers.extended[0];
    EXPECT_EQ(x.hat_hot_id, 3);
    EXPECT_TRUE(x.valid);
    EXPECT_NEAR(x.hat_m + x.hot_m, msl, 1e-9);  // HAT plus HOT is the altitude
    EXPECT_NEAR(x.hot_m, hot, 1e-3);
    EXPECT_EQ(x.material_code, 0U);
    const HeightAnswer local = answer_height(*rig.terrain, {0, lat, lon, 0.0});
    EXPECT_FLOAT_EQ(x.normal_azimuth_deg, local.normal_azimuth_deg);
    EXPECT_FLOAT_EQ(x.normal_elevation_deg, local.normal_elevation_deg);
    EXPECT_EQ(rig.answers.extended[1].hat_hot_id, 4);
    EXPECT_FALSE(rig.answers.extended[1].valid);
    const CigiAdapterStats& s = rig.adapter->stats();
    EXPECT_EQ(s.hat_hot_requests, 4U);
    EXPECT_EQ(s.hat_hot_answers, 4U);
    EXPECT_EQ(s.hat_hot_invalid, 1U);
    EXPECT_EQ(s.hat_hot_dropped, 0U);
}

TEST(HatHot, WithoutTerrainEveryAnswerIsInvalid) {
    Rig rig(false);
    rig.send({geodetic(9, cigi::HatHotRequestType::hot, 34.63, -86.77, 300.0)});
    ASSERT_TRUE(rig.frames_until(1));
    ASSERT_EQ(rig.answers.responses.size(), 1U);
    EXPECT_EQ(rig.answers.responses[0].hat_hot_id, 9);
    EXPECT_FALSE(rig.answers.responses[0].valid);
    EXPECT_EQ(rig.adapter->stats().hat_hot_invalid, 1U);
}

TEST(HatHot, PeriodicRequestsKeepTheirPeriodAndStopOnZero) {
    Rig rig;
    rig.send({geodetic(5, cigi::HatHotRequestType::hat, 34.63, -86.77, 400.0, 3)});
    ASSERT_TRUE(rig.frames_until(1));  // the first at once
    const std::uint32_t asked = rig.host_frame;
    // Then one every third frame.
    for (int i = 0; i < 12; ++i) {
        rig.step_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    rig.poll_for(std::chrono::milliseconds(20));
    rig.step_frame();
    const std::size_t periodic = rig.answers.count();
    EXPECT_GE(periodic, 4U);
    EXPECT_LE(periodic, 6U);
    for (const auto& r : rig.answers.responses) {
        EXPECT_EQ(r.hat_hot_id, 5);
        EXPECT_TRUE(r.valid);
        EXPECT_EQ(r.host_frame_number_lsn, asked & 0xFU);
    }
    // Period 0 with the same ID: one more answer (after any periodic one
    // still on its way), then none.
    rig.send({geodetic(5, cigi::HatHotRequestType::hat, 34.63, -86.77, 400.0, 0)});
    ASSERT_TRUE(rig.frames_until(periodic + 1));
    for (int i = 0; i < 6; ++i) {
        rig.step_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const std::size_t stopped = rig.answers.count();
    EXPECT_LE(stopped, periodic + 2);
    for (int i = 0; i < 12; ++i) {
        rig.step_frame();
    }
    EXPECT_EQ(rig.answers.count(), stopped);
}

TEST(HatHot, AnEntityPointFollowsItsEntityAndEndsWithIt) {
    Rig rig;
    // Entity 3 at 600 m above sea level heading 90 (east), pitched 10 up;
    // the point 50 m ahead of it and 2 m below, in its body axes.
    cigi::EntityControl e;
    e.entity_id = 3;
    e.entity_state = cigi::EntityState::active;
    e.lat_or_x = 34.63;
    e.lon_or_y = -86.77;
    e.alt_or_z = 600.0;
    e.yaw_deg = 90.0F;
    e.pitch_deg = 10.0F;
    cigi::HatHotRequest r;
    r.hat_hot_id = 8;
    r.request_type = cigi::HatHotRequestType::extended;
    r.entity_coordinates = true;
    r.entity_id = 3;
    r.update_period = 1;
    r.lat_or_x = 50.0;
    r.alt_or_z = 2.0;
    rig.send({r}, {e});
    ASSERT_TRUE(rig.frames_until(1));
    const Geodetic origin{34.63, -86.77, 600.0 + kGeoid};
    const auto expected = [&](const Geodetic& at) {
        const DVec3 p =
            geodetic_to_ecef(at) + body_to_ecef(at, {90.0, 10.0, 0.0}) * DVec3{50.0, 0.0, 2.0};
        const Geodetic g = ecef_to_geodetic(p);
        return g.alt_m - plane(g.lat_deg, g.lon_deg);  // its HAT
    };
    ASSERT_TRUE(rig.answers.extended[0].valid);
    EXPECT_NEAR(rig.answers.extended[0].hat_m, expected(origin), 1e-3);
    // The entity moves north: the next answers follow it.
    e.lat_or_x = 34.64;
    rig.send({}, {e});
    rig.poll_for(std::chrono::milliseconds(20));
    rig.answers = {};
    ASSERT_TRUE(rig.frames_until(2));
    EXPECT_NEAR(rig.answers.extended.back().hat_m, expected({34.64, -86.77, 600.0 + kGeoid}), 1e-3);
    // Destroyed: one invalid answer ends the periodic request (after any
    // asked before, still on their way).
    e.entity_state = cigi::EntityState::destroyed;
    rig.send({}, {e});
    rig.poll_for(std::chrono::milliseconds(20));
    rig.answers = {};
    for (int i = 0; i < 10; ++i) {
        rig.step_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_FALSE(rig.answers.extended.empty());
    EXPECT_FALSE(rig.answers.extended.back().valid);
    EXPECT_EQ(std::count_if(rig.answers.extended.begin(), rig.answers.extended.end(),
                            [](const auto& x) { return !x.valid; }),
              1);
    EXPECT_LE(rig.answers.extended.size(), 2U);
}

// A backlog (no frames while 2,000 requests arrive: both rings full, the
// query thread waiting on the answers') leaves over several frames, none
// lost, however fast the query thread refills the ring as it drains.
TEST(HatHot, ABacklogLeavesOverSeveralFramesWithNoneLost) {
    Rig rig;
    std::vector<cigi::HatHotRequest> requests;
    for (std::uint16_t i = 0; i < 100; ++i) {
        requests.push_back(geodetic(i, cigi::HatHotRequestType::extended, 34.63, -86.77, 500.0));
    }
    for (int m = 0; m < 20; ++m) {
        rig.send(requests);
        rig.poll_for(std::chrono::milliseconds(5));
    }
    rig.poll_for(std::chrono::milliseconds(50));
    ASSERT_EQ(rig.adapter->stats().hat_hot_requests, 2000U);
    ASSERT_EQ(rig.adapter->stats().hat_hot_dropped, 0U);
    ASSERT_TRUE(rig.frames_until(2000));
    EXPECT_EQ(rig.answers.count(), 2000U);
    EXPECT_EQ(rig.adapter->stats().hat_hot_dropped, 0U);
}

// The plan's load: 100 requests in one host message, answered in the IG's
// next message or the one after; and the network thread allocates nothing
// doing it (the render thread never sees a request).
TEST(HatHot, AHundredRequestsAFrameAreAnsweredNextFrameWithoutAllocating) {
    Rig rig;
    std::vector<cigi::HatHotRequest> requests;
    for (std::uint16_t i = 0; i < 100; ++i) {
        requests.push_back(
            geodetic(i, cigi::HatHotRequestType::extended, 34.61 + 0.0005 * i, -86.77, 500.0));
    }
    rig.send(requests);  // warms the query thread's tiles
    ASSERT_TRUE(rig.frames_until(100));
    std::vector<double> latency_ms;
    double worst_frames = 0;
    rig.allocations = 0;
    for (int round = 0; round < 20; ++round) {
        rig.answers = {};
        rig.send(requests);
        const auto sent = std::chrono::steady_clock::now();
        rig.poll_for(std::chrono::milliseconds(1));  // the request arrives
        int frames = 0;
        while (rig.answers.count() < 100 && frames < 10) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            rig.step_frame();
            ++frames;
        }
        latency_ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent)
                .count());
        worst_frames = std::max(worst_frames, static_cast<double>(frames));
        ASSERT_EQ(rig.answers.count(), 100U);
        for (const auto& x : rig.answers.extended) {
            EXPECT_TRUE(x.valid);
        }
    }
    EXPECT_EQ(rig.allocations, 0U);
    EXPECT_LE(worst_frames, 2.0);
    std::sort(latency_ms.begin(), latency_ms.end());
    std::printf(
        "100 HAT/HOT requests answered in %.2f ms (median), %.2f ms (worst), "
        "within %.0f frames\n",
        latency_ms[latency_ms.size() / 2], latency_ms.back(), worst_frames);
}

}  // namespace
}  // namespace fovea
