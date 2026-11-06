#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "flight.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/net/cigi_adapter.hpp"
#include "fovea/net/host_thread.hpp"
#include "script.hpp"
#include "session.hpp"

namespace fovea::test_host {
namespace {

FlightState start_at(double heading, double speed) {
    FlightState s;
    s.position = {34.6, -86.8, 500.0};
    s.attitude.yaw_deg = heading;
    s.speed_mps = speed;
    return s;
}

// Ground distance north and east between two points, from their ECEF
// positions in the local level at the first (straight line; fine at a few
// km).
DVec3 ned_between(const Geodetic& a, const Geodetic& b) {
    return glm::transpose(ned_to_ecef(a.lat_deg, a.lon_deg)) *
           (geodetic_to_ecef(b) - geodetic_to_ecef(a));
}

void fly(Flight& f, double seconds, double dt = 1.0 / 60.0) {
    for (double t = 0.0; t < seconds - 1e-9; t += dt) {
        f.step(dt);
    }
}

TEST(TestHostFlight, StraightAndLevelCoversSpeedTimesTime) {
    Segment s;
    s.kind = Segment::Kind::straight;
    s.duration_s = 100.0;
    s.speed_mps = 50.0;
    Flight f(start_at(90.0, 50.0), {s}, false);
    const Geodetic from = f.state().position;
    fly(f, 10.0);
    const DVec3 d = ned_between(from, f.state().position);
    EXPECT_NEAR(d.y, 500.0, 0.05);  // east, 500 m (a hair less along the curve)
    EXPECT_NEAR(d.x, 0.0, 0.05);
    EXPECT_NEAR(f.state().position.alt_m, 500.0, 1e-9);
    EXPECT_DOUBLE_EQ(f.state().attitude.roll_deg, 0.0);
}

TEST(TestHostFlight, ClimbSetsPitchAndAltitude) {
    Segment s;
    s.kind = Segment::Kind::straight;
    s.duration_s = 100.0;
    s.speed_mps = 50.0;
    s.climb_mps = 5.0;
    Flight f(start_at(0.0, 50.0), {s}, false);
    fly(f, 10.0);
    EXPECT_NEAR(f.state().position.alt_m, 550.0, 1e-6);
    // Eased to atan(5/50) = 5.71 degrees, well within 10 s at 30 deg/s.
    EXPECT_NEAR(f.state().attitude.pitch_deg, glm::degrees(std::atan2(5.0, 50.0)), 1e-9);
}

TEST(TestHostFlight, ACoordinatedTurnBanksAndComesRound) {
    Segment s;
    s.kind = Segment::Kind::turn;
    s.duration_s = 30.0;  // 360 degrees at 12 deg/s
    s.speed_mps = 50.0;
    s.turn_rate_dps = 12.0;
    Flight f(start_at(0.0, 50.0), {s}, false);
    const Geodetic from = f.state().position;
    fly(f, 15.0);
    // Half way: heading 180, banked right by atan(v * omega / g).
    EXPECT_NEAR(f.state().attitude.yaw_deg, 180.0, 0.3);
    EXPECT_NEAR(f.state().attitude.roll_deg,
                glm::degrees(std::atan(50.0 * glm::radians(12.0) / kGravity)), 1e-9);
    fly(f, 15.0);
    // A full circle (radius v / omega = 239 m) ends near where it began.
    EXPECT_NEAR(std::fmod(f.state().attitude.yaw_deg + 180.0, 360.0) - 180.0, 0.0, 0.3);
    EXPECT_LT(glm::length(ned_between(from, f.state().position)), 2.0);
}

TEST(TestHostFlight, AFigureEightTurnsBothWaysAndCloses) {
    Segment s;
    s.kind = Segment::Kind::figure_eight;
    s.speed_mps = 40.0;
    s.turn_rate_dps = 15.0;
    s.duration_s = 720.0 / 15.0;
    Flight f(start_at(0.0, 40.0), {s}, false);
    const Geodetic from = f.state().position;
    fly(f, 12.0);
    EXPECT_GT(f.state().attitude.roll_deg, 10.0);  // right first
    fly(f, 24.0);
    EXPECT_LT(f.state().attitude.roll_deg, -10.0);  // then left
    fly(f, 12.05);  // 48 s in all (a little over: the steps' sum rounds)
    EXPECT_TRUE(f.finished());
    // Back near the start (the bank eases in and out, so not exactly).
    EXPECT_LT(glm::length(ned_between(from, f.state().position)), 15.0);
}

TEST(TestHostFlight, RollsGoAllTheWayRoundThenLevel) {
    Segment roll;
    roll.kind = Segment::Kind::roll;
    roll.duration_s = 4.0;
    roll.speed_mps = 50.0;
    roll.roll_rate_dps = 90.0;
    Segment level;
    level.kind = Segment::Kind::straight;
    level.duration_s = 10.0;
    level.speed_mps = 50.0;
    Flight f(start_at(0.0, 50.0), {roll, level}, false);
    fly(f, 1.0);
    EXPECT_NEAR(f.state().attitude.roll_deg, 90.0, 1.6);
    fly(f, 1.0);
    EXPECT_NEAR(std::abs(f.state().attitude.roll_deg), 180.0, 1.6);  // inverted
    fly(f, 2.0);
    EXPECT_NEAR(f.state().attitude.roll_deg, 0.0, 1.6);  // once round
    fly(f, 5.0);
    EXPECT_DOUBLE_EQ(f.state().attitude.roll_deg, 0.0);
}

TEST(TestHostFlight, LoopsOrHolds) {
    Segment s;
    s.kind = Segment::Kind::hold;
    s.duration_s = 1.0;
    Flight once(start_at(0.0, 0.0), {s, s}, false);
    fly(once, 3.0);
    EXPECT_TRUE(once.finished());
    Flight looping(start_at(0.0, 0.0), {s, s}, true);
    fly(looping, 3.5);
    EXPECT_FALSE(looping.finished());
    EXPECT_EQ(looping.segment_index(), 1U);
}

TEST(TestHostScript, ExampleScriptsLoad) {
    for (const char* name : {"circuit.toml", "gentle.toml"}) {
        const auto r =
            load_script(std::string(FOVEA_SOURCE_DIR "/tools/cigi-test-host/scripts/") + name);
        EXPECT_TRUE(r.script.has_value()) << name << ": " << (r.errors.empty() ? "" : r.errors[0]);
    }
}

TEST(TestHostScript, MistakesAreReportedWithTheirLines) {
    const std::string text = R"([ig]
address = "1.2.3"
port = 8004
[start]
lat = 34.6
lon = -86.8
alt = 300.0
[[segment]]
kind = "loop_the_loop"
duration = 5.0
[[segment]]
kind = "figure_eight"
speed = 40.0
[[segment]]
kind = "straight"
duration = 5.0
speed = 10.0
climb = 20.0
)";
    const auto r = parse_script(text, "s.toml");
    ASSERT_FALSE(r.script.has_value());
    ASSERT_EQ(r.errors.size(), 4U);
    EXPECT_NE(r.errors[0].find("s.toml:2: 'address' in [ig] must be an IPv4 address"),
              std::string::npos)
        << r.errors[0];
    EXPECT_NE(r.errors[1].find("s.toml:9:"), std::string::npos) << r.errors[1];
    EXPECT_NE(r.errors[2].find("needs a 'turn_rate'"), std::string::npos) << r.errors[2];
    EXPECT_NE(r.errors[3].find("'climb' can't be faster than 'speed'"), std::string::npos)
        << r.errors[3];
}

TEST(TestHostSession, FliesFoveasAdapterOverLoopback) {
    // The test host against Fovea's CIGI adapter, in one process: the host
    // asks for Operate, waits for it, then sends entity 0; Start of Frame
    // comes back for each IG frame.
    auto scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    CigiAdapterOptions ig_options;
    ig_options.address = kLoopback;
    CigiAdapter adapter(ig_options, &frames);
    HostThread network(adapter, *scene);
    network.start();

    Session session({kLoopback, adapter.port()}, 0, kLoopback);
    Segment s;
    s.kind = Segment::Kind::straight;
    s.duration_s = 100.0;
    s.speed_mps = 60.0;
    Flight flight(start_at(0.0, 60.0), {s}, false);
    const auto begin = std::chrono::steady_clock::now();
    auto last = begin;
    std::uint64_t ig_frame = 0;
    // About 1.5 s at 60 Hz: host frames, and IG frame starts as a render
    // loop would push them.
    while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(1500)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const auto t = std::chrono::steady_clock::now();
        (void)frames.try_push({ig_frame++, t});
        if (session.operating()) {
            flight.step(std::chrono::duration<double>(t - last).count());
        }
        last = t;
        session.send_frame(flight.state(), t);
    }
    network.stop();

    EXPECT_TRUE(session.operating());
    EXPECT_GT(session.stats().start_of_frames, 30U);
    EXPECT_GT(session.stats().round_trips, 10U);
    const EntityState* e = find_entity(scene->latest(), 0);
    ASSERT_NE(e, nullptr);
    // Where the IG has entity 0 is where the host last sent it (within one
    // frame of flight: 60 m/s for 16 ms).
    const DVec3 sent = geodetic_to_ecef(flight.state().position);
    EXPECT_LT(glm::length(e->cur.ecef - sent), 2.0);
    EXPECT_EQ(adapter.stats().rejected_messages, 0U);
}

}  // namespace
}  // namespace fovea::test_host
