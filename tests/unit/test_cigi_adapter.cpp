#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/attitude.hpp"
#include "fovea/core/celestial.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/net/cigi.hpp"
#include "fovea/net/cigi_adapter.hpp"
#include "fovea/net/host_thread.hpp"
#include "fovea/net/udp.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

using Bytes = std::vector<std::uint8_t>;

// A CIGI host message: IG Control, then any Entity Controls.
Bytes host_message(const cigi::IgControl& ig, const std::vector<cigi::EntityControl>& entities) {
    Bytes out(cigi::kIgControlSize + entities.size() * cigi::kEntityControlSize);
    std::size_t at = cigi::encode(ig, out);
    for (const auto& e : entities) {
        at += cigi::encode(e, std::span(out).subspan(at));
    }
    return out;
}

cigi::IgControl operate(std::uint32_t host_frame = 1, std::uint32_t timestamp = 0) {
    cigi::IgControl ig;
    ig.ig_mode = cigi::IgMode::operate;
    ig.host_frame_number = host_frame;
    ig.timestamp_valid = true;
    ig.timestamp = timestamp;
    return ig;
}

cigi::EntityControl entity(std::uint16_t id, double lat, double lon, double alt) {
    cigi::EntityControl e;
    e.entity_id = id;
    e.entity_state = cigi::EntityState::active;
    e.entity_type = 7;
    e.lat_or_x = lat;
    e.lon_or_y = lon;
    e.alt_or_z = alt;
    e.yaw_deg = 90.0F;
    e.pitch_deg = 10.0F;
    e.roll_deg = -5.0F;
    return e;
}

// A host: its own socket on loopback, sending to the adapter's port.
struct Host {
    UdpSocket socket{0, kLoopback};
    UdpEndpoint ig;

    explicit Host(std::uint16_t ig_port) : ig{kLoopback, ig_port} {}
    void send(const Bytes& message) { ASSERT_TRUE(socket.send_to(message, ig)); }
    UdpEndpoint endpoint() const { return {kLoopback, socket.local_port()}; }
};

// Polls until `done` or two seconds pass (loopback delivery is quick but not
// instant).
bool poll_until(CigiAdapter& adapter, SceneState& scene, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)adapter.poll(scene);
        if (done()) {
            return true;
        }
    }
    return false;
}

// The ICD's startup (2.8, p. 13): the host asks for Operate in an IG Control
// on its own; the IG ignores anything else until it has switched.
void start_operating(CigiAdapter& adapter, SceneState& scene, Host& host,
                     std::uint32_t timestamp = 0) {
    host.send(host_message(operate(0, timestamp), {}));
    ASSERT_TRUE(
        poll_until(adapter, scene, [&] { return adapter.ig_mode() == cigi::IgMode::operate; }));
}

TEST(Udp, ParsesAndFormatsAddresses) {
    EXPECT_EQ(parse_ipv4("127.0.0.1"), kLoopback);
    EXPECT_EQ(parse_ipv4("192.168.1.20"), 0xC0A80114U);
    for (const char* bad :
         {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.x", "1..2.3", " 1.2.3.4", "0001.2.3.4"}) {
        EXPECT_FALSE(parse_ipv4(bad).has_value()) << bad;
    }
    EXPECT_EQ(format_endpoint({0xC0A80114U, 8004}), "192.168.1.20:8004");
}

TEST(Udp, LoopbackSendAndReceive) {
    UdpSocket a(0, kLoopback);
    UdpSocket b(0, kLoopback);
    EXPECT_NE(a.local_port(), 0);
    EXPECT_FALSE(b.wait_readable(0));  // nothing yet
    const Bytes sent = {1, 2, 3, 4, 5};
    ASSERT_TRUE(a.send_to(sent, {kLoopback, b.local_port()}));
    ASSERT_TRUE(b.wait_readable(1000));
    std::array<std::uint8_t, 16> buffer{};
    const auto r = b.receive(buffer);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size, 5U);
    EXPECT_FALSE(r->truncated);
    EXPECT_EQ(r->from, (UdpEndpoint{kLoopback, a.local_port()}));
    EXPECT_EQ(Bytes(buffer.begin(), buffer.begin() + 5), sent);
    EXPECT_FALSE(b.receive(buffer).has_value());  // nothing else waiting

    // A datagram larger than the buffer is reported as cut.
    ASSERT_TRUE(a.send_to(Bytes(40, 9), {kLoopback, b.local_port()}));
    ASSERT_TRUE(b.wait_readable(1000));
    const auto cut = b.receive(buffer);
    ASSERT_TRUE(cut.has_value());
    EXPECT_TRUE(cut->truncated);
    EXPECT_EQ(cut->size, buffer.size());
}

TEST(Udp, ABusyPortIsAnError) {
    UdpSocket a(0, kLoopback);
    EXPECT_THROW(UdpSocket(a.local_port(), kLoopback), std::runtime_error);
}

TEST(CigiAdapter, EntityControlPlacesTheEntity) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapterOptions options;
    options.geoid_height_m = -30.0;
    CigiAdapter adapter(options);
    Host host(adapter.port());
    start_operating(adapter, *scene, host, 1000);
    host.send(host_message(operate(1, 1000), {entity(0, 34.6372, -86.7751, 600.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 2; }));

    const SceneSnapshot& s = scene->latest();
    ASSERT_EQ(s.entities.size(), 1U);
    const EntityState& e = s.entities[0];
    EXPECT_EQ(e.id, 0U);
    EXPECT_EQ(e.model, 7U);
    EXPECT_TRUE(e.active);
    // Altitude above sea level, plus the geoid height, is the ellipsoid
    // height; the attitude is yaw, pitch, roll from north at that point.
    const Geodetic g{34.6372, -86.7751, 570.0};
    const DVec3 expected = geodetic_to_ecef(g);
    EXPECT_LT(glm::length(e.cur.ecef - expected), 1e-6);
    const DQuat q = body_to_ecef(g, Euler{90.0, 10.0, -5.0});
    EXPECT_GT(std::abs(glm::dot(e.cur.orientation, q)), 1.0 - 1e-9);
    EXPECT_DOUBLE_EQ(e.cur.host_t.seconds, 0.0);  // the first timestamp is the origin
    EXPECT_TRUE(e.cur.host_t.valid);
    EXPECT_EQ(adapter.ig_mode(), cigi::IgMode::operate);
}

// A host message of IG Control and any packets, each encoded in turn.
template <typename... Packets>
Bytes message_of(const cigi::IgControl& ig, const Packets&... packets) {
    Bytes out(512);
    std::size_t at = cigi::encode(ig, out);
    ((at += cigi::encode(packets, std::span(out).subspan(at))), ...);
    out.resize(at);
    return out;
}

cigi::WeatherControl weather(std::uint8_t layer_id, float base, float thickness, float visibility,
                             float coverage = 100.0F) {
    cigi::WeatherControl w;
    w.layer_id = layer_id;
    w.weather_enable = true;
    w.base_elevation_m = base;
    w.thickness_m = thickness;
    w.visibility_range_m = visibility;
    w.coverage = coverage;
    return w;
}

// Celestial Sphere Control (milestone 6 step 7): the date and time only when
// Date/Time Valid says so, whether time runs always; a date the sky isn't
// computed for is refused.
TEST(CigiAdapter, CelestialSphereControlSetsTheTimeWhenValid) {
    auto scene = std::make_unique<SceneState>();
    const double config_jd = julian_date(2026, 3, 20, 15.5 * 3600.0);
    scene->edit().environment.julian_date = config_jd;
    scene->edit().environment.time_runs = false;
    CigiAdapter adapter(CigiAdapterOptions{});
    Host host(adapter.port());
    start_operating(adapter, *scene, host);

    cigi::CelestialSphereControl c;
    c.hour = 14;
    c.minute = 35;
    c.date = 11032026;
    c.ephemeris_enable = true;  // Date/Time Valid off: the time carries on, now running
    host.send(message_of(operate(1), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 1; }));
    const EnvironmentState first = scene->latest().environment;
    EXPECT_DOUBLE_EQ(first.julian_date, config_jd);
    EXPECT_TRUE(first.time_runs);

    c.date_time_valid = true;
    c.ephemeris_enable = false;
    host.send(message_of(operate(2), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 2; }));
    const EnvironmentState second = scene->latest().environment;
    EXPECT_DOUBLE_EQ(second.julian_date, julian_date(2026, 11, 3, 14 * 3600.0 + 35 * 60.0));
    EXPECT_FALSE(second.time_runs);

    for (const std::uint32_t bad : {2302026U, 13012026U, 1011949U, 1012051U, 0U}) {
        c.date = bad;  // 30 February, month 13, 1949, 2051, none
        host.send(message_of(operate(3), c));
    }
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().rejected_packets == 5; }));
    EXPECT_EQ(adapter.stats().environment_updates, 2U);
    EXPECT_DOUBLE_EQ(scene->latest().environment.julian_date, second.julian_date);
    // 29 February in a leap year is a date.
    c.date = 2292028;
    host.send(message_of(operate(4), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 3; }));
    // Hour 24 is refused with Date/Time Valid, and means nothing without:
    // the clock is still started.
    c.hour = 24;
    host.send(message_of(operate(5), c));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().rejected_packets == 6; }));
    c.date_time_valid = false;
    c.ephemeris_enable = true;
    host.send(message_of(operate(6), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 4; }));
    EXPECT_TRUE(scene->latest().environment.time_runs);
}

// A host may send its time every frame: within the minute the IG's clock is
// already in, the clock keeps its seconds and runs on; another minute sets
// it.
TEST(CigiAdapter, TheSameMinuteAgainLeavesTheClockRunning) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter(CigiAdapterOptions{});
    Host host(adapter.port());
    start_operating(adapter, *scene, host);
    cigi::CelestialSphereControl c;
    c.date_time_valid = true;
    c.ephemeris_enable = true;
    c.date = 3202026;
    c.hour = 12;
    c.minute = 0;
    host.send(message_of(operate(1), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 1; }));
    const EnvironmentState first = scene->latest().environment;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    host.send(message_of(operate(2), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 2; }));
    const EnvironmentState again = scene->latest().environment;
    EXPECT_EQ(again.julian_date, first.julian_date);  // not set back to 12:00:00
    EXPECT_EQ(again.set_at, first.set_at);
    EXPECT_TRUE(again.time_runs);
    c.minute = 30;
    host.send(message_of(operate(3), c));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 3; }));
    EXPECT_DOUBLE_EQ(scene->latest().environment.julian_date,
                     julian_date(2026, 3, 20, 12.5 * 3600.0));
}

// Reset/Standby puts the environment back as config set it (ICD 2.7.3).
TEST(CigiAdapter, StandbyPutsTheEnvironmentBack) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapterOptions options;
    options.environment.visibility_m = 20000.0;
    (void)options.environment.layers.push_back({WeatherKind::fog, 100.0, 50.0, 1.0, 200.0, 0});
    scene->edit().environment = options.environment;
    CigiAdapter adapter(options);
    Host host(adapter.port());
    start_operating(adapter, *scene, host);
    cigi::AtmosphereControl a;
    a.visibility_range_m = 3000.0F;
    cigi::WeatherControl no_fog = weather(0, 0.0F, 0.0F, 0.0F);
    no_fog.weather_enable = false;
    host.send(message_of(operate(1), a, no_fog, weather(4, 0.0F, 1500.0F, 5000.0F)));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 3; }));
    EXPECT_EQ(scene->latest().environment.visibility_m, 3000.0);
    cigi::IgControl standby = operate(2);
    standby.ig_mode = cigi::IgMode::reset_standby;
    host.send(message_of(standby));
    ASSERT_TRUE(poll_until(adapter, *scene,
                           [&] { return adapter.ig_mode() == cigi::IgMode::reset_standby; }));
    const EnvironmentState& back = scene->latest().environment;
    EXPECT_EQ(back.visibility_m, 20000.0);
    ASSERT_EQ(back.layers.size(), 1U);
    EXPECT_EQ(back.layers[0].kind, WeatherKind::fog);
}

// Atmosphere Control sets the global visibility; none, or a negative one,
// is refused.
TEST(CigiAdapter, AtmosphereControlSetsTheVisibility) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter(CigiAdapterOptions{});
    Host host(adapter.port());
    start_operating(adapter, *scene, host);
    cigi::AtmosphereControl a;
    a.visibility_range_m = 8000.0F;
    host.send(message_of(operate(1), a));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().environment_updates == 1; }));
    EXPECT_EQ(scene->latest().environment.visibility_m, 8000.0);
    a.visibility_range_m = -1.0F;
    host.send(message_of(operate(2), a));
    a.visibility_range_m = 0.0F;  // none at all can't be drawn
    host.send(message_of(operate(3), a));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().rejected_packets == 2; }));
    EXPECT_EQ(scene->latest().environment.visibility_m, 8000.0);
}

// Weather Control replaces a global layer as a whole, or removes it (Weather
// Enable off); config's layers are replaced the same way, by their ids.
// Regional layers, weather entities and the layers not drawn (hail here)
// are counted and ignored, as is Environmental Region Control.
TEST(CigiAdapter, WeatherControlSetsAndRemovesGlobalLayers) {
    auto scene = std::make_unique<SceneState>();
    auto& layers = scene->edit().environment.layers;
    (void)layers.push_back({WeatherKind::fog, 100.0, 50.0, 1.0, 200.0, 0});
    (void)layers.push_back({WeatherKind::rain, 0.0, 1500.0, 1.0, 5000.0, 4});
    CigiAdapterOptions options;
    options.geoid_height_m = -30.0;
    CigiAdapter adapter(options);
    Host host(adapter.port());
    start_operating(adapter, *scene, host);

    cigi::WeatherControl regional = weather(0, 0.0F, 10.0F, 10.0F);
    regional.scope = cigi::WeatherScope::regional;
    std::array<std::uint8_t, 48> region{};  // Environmental Region Control (4.1.11, p. 88)
    region[0] = cigi::kEnvironmentalRegionControlId;
    region[1] = 48;
    cigi::WeatherControl no_rain = weather(4, 0.0F, 0.0F, 0.0F);
    no_rain.weather_enable = false;
    host.send(message_of(operate(1), weather(0, 180.0F, 60.0F, 300.0F),
                         weather(2, 1500.0F, 400.0F, 1000.0F, 75.0F), no_rain, regional,
                         weather(7, 0.0F, 100.0F, 100.0F)));
    Bytes with_region = message_of(operate(2));
    with_region.insert(with_region.end(), region.begin(), region.end());
    host.send(with_region);
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 3; }));
    EXPECT_EQ(adapter.stats().environment_updates, 3U);
    EXPECT_EQ(adapter.stats().weather_ignored, 2U);
    EXPECT_EQ(adapter.stats().skipped_packets, 1U);

    const auto& now = scene->latest().environment.layers;
    ASSERT_EQ(now.size(), 2U);
    const WeatherLayer* fog = nullptr;
    const WeatherLayer* cloud = nullptr;
    for (const WeatherLayer& l : now) {
        (l.id == 0 ? fog : cloud) = &l;
    }
    ASSERT_NE(fog, nullptr);
    ASSERT_NE(cloud, nullptr);
    EXPECT_EQ(fog->kind, WeatherKind::fog);
    EXPECT_EQ(fog->base_m, 150.0);  // 180 above sea level, the geoid 30 m below it
    EXPECT_EQ(fog->thickness_m, 60.0);
    EXPECT_EQ(fog->visibility_m, 300.0);
    EXPECT_EQ(cloud->id, 2);
    EXPECT_EQ(cloud->kind, WeatherKind::cloud);
    EXPECT_EQ(cloud->coverage, 0.75);
}

TEST(CigiAdapter, SamplesShiftAndTimestampsUnwrap) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host, 0xFFFFFF00U);
    // Across the 32-bit rollover: 0xFFFFFF00 to 0x100 is 0x200 ticks.
    host.send(host_message(operate(1, 0xFFFFFF00U), {entity(3, 10.0, 20.0, 100.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 2; }));
    host.send(host_message(operate(2, 0x100U), {entity(3, 10.0, 20.0, 200.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 3; }));
    const EntityState& e = scene->latest().entities[0];
    EXPECT_NEAR(e.cur.host_t.seconds, 0x200 * 10e-6, 1e-12);
    EXPECT_DOUBLE_EQ(e.prev.host_t.seconds, 0.0);
    EXPECT_NEAR(glm::length(e.cur.ecef - e.prev.ecef), 100.0, 1e-6);  // 100 m higher
    EXPECT_GT(e.cur.recv_t, e.prev.recv_t);
}

TEST(CigiAdapter, StandbyIgnoresTheHostAndDestroysEntities) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    // The IG starts in Reset/Standby: entities sent then are ignored.
    cigi::IgControl standby = operate();
    standby.ig_mode = cigi::IgMode::reset_standby;
    host.send(host_message(standby, {entity(1, 0.0, 0.0, 0.0)}));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().ignored_in_standby == 1; }));
    EXPECT_TRUE(scene->latest().entities.empty());

    // The message switching to Operate was sent in Reset/Standby: its
    // entities are ignored too (ICD 2.8, p. 13).
    host.send(host_message(operate(), {entity(1, 0.0, 0.0, 0.0), entity(2, 1.0, 1.0, 0.0)}));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.ig_mode() == cigi::IgMode::operate; }));
    EXPECT_TRUE(scene->latest().entities.empty());
    host.send(host_message(operate(), {entity(1, 0.0, 0.0, 0.0), entity(2, 1.0, 1.0, 0.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return scene->latest().entities.size() == 2; }));
    // Back to Reset/Standby: every entity is destroyed (ICD p. 214).
    host.send(host_message(standby, {}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return scene->latest().entities.empty(); }));
    EXPECT_EQ(adapter.ig_mode(), cigi::IgMode::reset_standby);
}

TEST(CigiAdapter, InactiveDestroyedAndAttachedEntities) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host);
    host.send(host_message(
        operate(), {entity(1, 0.0, 0.0, 0.0), entity(2, 0.0, 0.0, 0.0), entity(3, 0.0, 0.0, 0.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return scene->latest().entities.size() == 3; }));

    cigi::EntityControl inactive = entity(1, 0.0, 0.0, 0.0);
    inactive.entity_state = cigi::EntityState::inactive_standby;
    cigi::EntityControl destroyed = entity(2, 0.0, 0.0, 0.0);
    destroyed.entity_state = cigi::EntityState::destroyed;
    cigi::EntityControl child = entity(4, 1.0, 2.0, 3.0);
    child.attach = true;
    child.parent_id = 3;
    host.send(host_message(operate(2), {inactive, destroyed, child}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 3; }));

    const SceneSnapshot& s = scene->latest();
    ASSERT_EQ(s.entities.size(), 2U);  // 2 destroyed; the child (4) not applied yet
    EXPECT_EQ(find_entity(s, 2), nullptr);
    EXPECT_EQ(find_entity(s, 4), nullptr);
    ASSERT_NE(find_entity(s, 1), nullptr);
    EXPECT_FALSE(find_entity(s, 1)->active);
    EXPECT_TRUE(find_entity(s, 3)->active);
    EXPECT_EQ(adapter.stats().attached_entities, 1U);
}

TEST(CigiAdapter, StartOfFrameGoesToTheConfiguredHost) {
    auto scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    UdpSocket host_socket(0, kLoopback);
    CigiAdapterOptions options;
    options.host = UdpEndpoint{kLoopback, host_socket.local_port()};
    CigiAdapter adapter(options, &frames);
    // Before any host message: a synchronous host waits for this.
    ASSERT_TRUE(frames.try_push({41, now()}));
    (void)adapter.poll(*scene);
    ASSERT_TRUE(host_socket.wait_readable(1000));
    std::array<std::uint8_t, 64> buffer{};
    const auto r = host_socket.receive(buffer);
    ASSERT_TRUE(r.has_value());
    cigi::MessageReader reader{std::span(buffer).first(r->size), cigi::kStartOfFrameId};
    cigi::MessageReader::Packet p;
    ASSERT_TRUE(reader.next(p));
    const auto sof = cigi::decode_start_of_frame(p.bytes, reader.swapped());
    ASSERT_TRUE(sof.has_value());
    EXPECT_EQ(sof->ig_frame_number, 41U);
    EXPECT_EQ(sof->ig_mode, cigi::IgMode::reset_standby);
    EXPECT_TRUE(sof->timestamp_valid);
    EXPECT_EQ(sof->last_host_frame_number, 0U);
    EXPECT_EQ(adapter.stats().frames_reported, 1U);
}

TEST(CigiAdapter, StartOfFrameAnswersTheLastSenderOtherwise) {
    auto scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    CigiAdapter adapter({}, &frames);
    Host host(adapter.port());
    // No host known yet: the frame start isn't reported anywhere.
    ASSERT_TRUE(frames.try_push({1, now()}));
    (void)adapter.poll(*scene);
    EXPECT_EQ(adapter.stats().frames_unreported, 1U);

    host.send(host_message(operate(77), {}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 1; }));
    ASSERT_TRUE(frames.try_push({2, now()}));
    (void)adapter.poll(*scene);
    ASSERT_TRUE(host.socket.wait_readable(1000));
    std::array<std::uint8_t, 64> buffer{};
    const auto r = host.socket.receive(buffer);
    ASSERT_TRUE(r.has_value());
    const auto sof = cigi::decode_start_of_frame(std::span(buffer).first(r->size), false);
    ASSERT_TRUE(sof.has_value());
    EXPECT_EQ(sof->ig_frame_number, 2U);
    EXPECT_EQ(sof->ig_mode, cigi::IgMode::operate);
    EXPECT_EQ(sof->last_host_frame_number, 77U);  // acknowledges the host's frame
}

TEST(CigiAdapter, GarbageIsCountedNotFatal) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    // Junk of every length up to 300 bytes, read as it arrives (a burst of
    // hundreds all at once can overflow the system's receive buffer, which
    // drops datagrams before the adapter sees them), then one good message.
    std::uint64_t sent = 0;
    for (std::size_t n = 0; n < 300; ++n) {
        Bytes junk(n);
        for (std::size_t i = 0; i < n; ++i) {
            junk[i] = static_cast<std::uint8_t>(i * 37U + n);
        }
        host.send(junk);
        ++sent;
        if (n % 32 == 31) {
            ASSERT_TRUE(poll_until(adapter, *scene,
                                   [&] { return adapter.stats().rejected_messages == sent; }));
        }
    }
    start_operating(adapter, *scene, host);
    host.send(host_message(operate(), {entity(9, 1.0, 2.0, 3.0)}));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return find_entity(scene->latest(), 9) != nullptr; }));
    EXPECT_EQ(adapter.stats().rejected_messages, 300U);
    EXPECT_EQ(adapter.stats().messages, 2U);
}

TEST(CigiAdapter, AFloodCantHoldUpStartOfFrame) {
    auto scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    UdpSocket host_socket(0, kLoopback);
    CigiAdapterOptions options;
    options.host = UdpEndpoint{kLoopback, host_socket.local_port()};
    CigiAdapter adapter(options, &frames);
    Host host(adapter.port());
    for (std::uint32_t i = 0; i < 150; ++i) {
        host.send(host_message(operate(i), {}));
    }
    ASSERT_TRUE(adapter.poll(*scene));  // waits for the first, reads at most 64
    EXPECT_GE(adapter.stats().messages, 1U);
    EXPECT_LE(adapter.stats().messages, 64U);
    // The next poll sends the frame's Start of Frame before reading more.
    ASSERT_TRUE(frames.try_push({5, now()}));
    ASSERT_TRUE(adapter.poll(*scene));
    EXPECT_EQ(adapter.stats().frames_reported, 1U);
    EXPECT_TRUE(host_socket.wait_readable(1000));
    EXPECT_GE(adapter.stats().messages, 2U);
}

TEST(Udp, ReceiveBufferIsAtLeastTheDefault) {
    UdpSocket s(0, kLoopback);
    EXPECT_GE(s.receive_buffer_bytes(), 64U * 1024U);
}

TEST(CigiAdapter, LateMessagesAreDroppedAndRestartedClocksFollowed) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host, 1000);
    host.send(host_message(operate(1, 2000), {entity(1, 0.0, 0.0, 100.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 2; }));
    // 100 ticks (1 ms) earlier than the last: arrived out of order. Not
    // applied, and not read as a 12-hour jump forward.
    host.send(host_message(operate(2, 1900), {entity(1, 0.0, 0.0, 999.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().stale_messages == 1; }));
    const EntityState* e = find_entity(scene->latest(), 1);
    ASSERT_NE(e, nullptr);
    EXPECT_NEAR(ecef_to_geodetic(e->cur.ecef).alt_m, 100.0, 1e-6);
    EXPECT_NEAR(e->cur.host_t.seconds, 1000 * 10e-6, 1e-12);
    // Ten seconds back: the host's clock restarted. Applied, and host time
    // carries on from where it was instead of going back.
    host.send(host_message(operate(3, 2000U - 1000000U), {entity(1, 0.0, 0.0, 300.0)}));
    host.send(host_message(operate(4, 2000U - 1000000U + 500U), {entity(1, 0.0, 0.0, 400.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 4; }));
    e = find_entity(scene->latest(), 1);
    ASSERT_NE(e, nullptr);
    EXPECT_NEAR(ecef_to_geodetic(e->cur.ecef).alt_m, 400.0, 1e-6);
    EXPECT_NEAR(e->cur.host_t.seconds, 1500 * 10e-6, 1e-12);
    EXPECT_NEAR(e->prev.host_t.seconds, 1000 * 10e-6, 1e-12);
}

TEST(CigiAdapter, UnstampedSamplesSayTheirHostTimeIsInvalid) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host);
    cigi::IgControl unstamped = operate();
    unstamped.timestamp_valid = false;
    host.send(host_message(unstamped, {entity(1, 0.0, 0.0, 0.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 2; }));
    ASSERT_NE(find_entity(scene->latest(), 1), nullptr);
    EXPECT_FALSE(find_entity(scene->latest(), 1)->cur.host_t.valid);
}

TEST(CigiAdapter, AttachingHidesAnEntityAndDestroyingRemovesIt) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host);
    host.send(host_message(operate(1), {entity(1, 0.0, 0.0, 0.0), entity(2, 0.0, 0.0, 0.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return scene->latest().entities.size() == 2; }));
    // Entity 1 becomes a child: not drawn at its old top-level place.
    cigi::EntityControl attach = entity(1, 1.0, 2.0, 3.0);
    attach.attach = true;
    attach.parent_id = 2;
    host.send(host_message(operate(2), {attach}));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return adapter.stats().attached_entities == 1; }));
    ASSERT_NE(find_entity(scene->latest(), 1), nullptr);
    EXPECT_FALSE(find_entity(scene->latest(), 1)->active);
    // Destroyed removes it, attached or not (ICD p. 49).
    attach.entity_state = cigi::EntityState::destroyed;
    host.send(host_message(operate(3), {attach}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return scene->latest().entities.size() == 1; }));
    EXPECT_EQ(find_entity(scene->latest(), 1), nullptr);
}

TEST(CigiAdapter, AfterAStallOnlyTheNewestFrameIsReported) {
    auto scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    UdpSocket host_socket(0, kLoopback);
    CigiAdapterOptions options;
    options.host = UdpEndpoint{kLoopback, host_socket.local_port()};
    CigiAdapter adapter(options, &frames);
    for (std::uint64_t f = 10; f < 40; ++f) {  // 30 frames while the thread was held up
        ASSERT_TRUE(frames.try_push({f, now()}));
    }
    (void)adapter.poll(*scene);
    EXPECT_EQ(adapter.stats().frames_reported, 1U);
    EXPECT_EQ(adapter.stats().frames_coalesced, 29U);
    ASSERT_TRUE(host_socket.wait_readable(1000));
    std::array<std::uint8_t, 64> buffer{};
    const auto r = host_socket.receive(buffer);
    ASSERT_TRUE(r.has_value());
    const auto sof = cigi::decode_start_of_frame(std::span(buffer).first(r->size), false);
    ASSERT_TRUE(sof.has_value());
    EXPECT_EQ(sof->ig_frame_number, 39U);
    EXPECT_FALSE(host_socket.wait_readable(50));  // just the one
}

TEST(CigiAdapter, SampleTimesFollowTheHostClock) {
    // Two messages stamped 1 ms apart, the second arriving 50 ms late: its
    // sample time is 1 ms after the first's, not 50 ms. Smoothing is
    // allowed only when both CIGI flags say so.
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host, 1000);
    cigi::IgControl ig = operate(1, 1000);
    ig.extrapolation_enable = true;
    cigi::EntityControl e = entity(1, 0.0, 0.0, 0.0);
    e.extrapolation_enable = true;
    host.send(host_message(ig, {e}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 2; }));
    const LocalTime first = find_entity(scene->latest(), 1)->cur.sample_t;
    EXPECT_TRUE(find_entity(scene->latest(), 1)->smooth);
    EXPECT_TRUE(scene->latest().smoothing_allowed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ig = operate(2, 1100);
    ig.extrapolation_enable = false;  // the host withdraws its leave
    host.send(host_message(ig, {e}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 3; }));
    const EntityState* now_e = find_entity(scene->latest(), 1);
    const double gap_ms =
        std::chrono::duration<double, std::milli>(now_e->cur.sample_t - first).count();
    // Plus the zero's creep, 100 ppm of the time between them (5 us for
    // 50 ms; a slow runner may take longer).
    EXPECT_NEAR(gap_ms, 1.0, 0.05);
    const double late_ms =
        std::chrono::duration<double, std::milli>(now_e->cur.recv_t - now_e->cur.sample_t).count();
    EXPECT_GE(late_ms, 45.0);
    // The host's global flag is the scene's; the entity's own stays its own.
    EXPECT_FALSE(scene->latest().smoothing_allowed);
    EXPECT_TRUE(now_e->smooth);
}

TEST(CigiAdapter, ARestartedHostClockIsMappedAfresh) {
    // The host leaves for 300 ms (Reset/Standby) and comes back with its
    // clock at 0 again: sample times match arrival times again, rather than
    // lagging by the time it was away.
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    Host host(adapter.port());
    start_operating(adapter, *scene, host, 500000);
    host.send(host_message(operate(1, 500000), {entity(1, 0.0, 0.0, 0.0)}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 2; }));
    cigi::IgControl standby = operate(2, 500100);
    standby.ig_mode = cigi::IgMode::reset_standby;
    host.send(host_message(standby, {}));
    ASSERT_TRUE(poll_until(adapter, *scene, [&] { return adapter.stats().messages == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    start_operating(adapter, *scene, host, 0);
    host.send(host_message(operate(3, 100), {entity(1, 0.0, 0.0, 0.0)}));
    ASSERT_TRUE(
        poll_until(adapter, *scene, [&] { return find_entity(scene->latest(), 1) != nullptr; }));
    const EntityState* e = find_entity(scene->latest(), 1);
    const double lag_ms =
        std::chrono::duration<double, std::milli>(e->cur.recv_t - e->cur.sample_t).count();
    EXPECT_LT(lag_ms, 50.0);  // not the 300 ms the host was away
}

TEST(CigiAdapter, SteadyStateAllocatesNothing) {
    auto scene = std::make_unique<SceneState>();
    FrameStartRing frames;
    UdpSocket host_socket(0, kLoopback);
    CigiAdapterOptions options;
    options.host = UdpEndpoint{kLoopback, host_socket.local_port()};
    CigiAdapter adapter(options, &frames);
    Host host(adapter.port());
    const Bytes message = host_message(operate(), {entity(0, 1.0, 2.0, 3.0)});
    std::array<std::uint8_t, 64> drain{};
    auto round = [&](std::uint64_t frame) {
        host.send(message);
        (void)frames.try_push({frame, now()});
        const std::uint64_t before = adapter.stats().messages;
        (void)poll_until(adapter, *scene, [&] { return adapter.stats().messages > before; });
        while (host_socket.receive(drain)) {
        }
    };
    start_operating(adapter, *scene, host);
    round(0);  // warm-up
    const test::AllocationCounter counter;
    for (std::uint64_t f = 1; f <= 50; ++f) {
        round(f);
    }
    // The adapter's own allocations: poll_until's std::function is the
    // test's, made before counting.
    EXPECT_EQ(counter.count(), 0U);
    EXPECT_EQ(adapter.stats().messages, 52U);
}

TEST(HostThread, RunsTheAdapterUntilStopped) {
    auto scene = std::make_unique<SceneState>();
    CigiAdapter adapter({});
    HostThread thread(adapter, *scene);
    thread.start();
    Host host(adapter.port());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::uint64_t seen = 0;
    while (seen == 0 && std::chrono::steady_clock::now() < deadline) {
        // The first message switches to Operate; later ones place entity 5.
        host.send(host_message(operate(), {entity(5, 1.0, 1.0, 1.0)}));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const SceneSnapshot& latest = scene->latest();
        seen = find_entity(latest, 5) != nullptr ? latest.sequence : 0;
    }
    thread.stop();
    EXPECT_GT(seen, 0U);
    EXPECT_NE(find_entity(scene->latest(), 5), nullptr);
}

TEST(HostThread, StopRethrowsTheAdaptersException) {
    struct Throws : HostAdapter {
        bool poll(SceneState& /*scene*/) override { throw std::runtime_error("socket gone"); }
    } adapter;
    auto scene = std::make_unique<SceneState>();
    HostThread thread(adapter, *scene);
    thread.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!thread.failed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(thread.failed());
    EXPECT_THROW(thread.stop(), std::runtime_error);
}

}  // namespace
}  // namespace fovea
