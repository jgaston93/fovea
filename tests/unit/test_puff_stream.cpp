// The puff stream (D-039): the codec, and the adapter's rules for keeping
// particles (docs/integration-guide.md, "Particle volumes").

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/core/particles.hpp"
#include "fovea/net/puff_adapter.hpp"
#include "fovea/net/puff_stream.hpp"
#include "fovea/net/udp.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

using namespace std::chrono_literals;
constexpr double kDeg = std::numbers::pi / 180.0;

puff::Header header(std::uint32_t source = 7, std::uint32_t session = 1) {
    puff::Header h;
    h.version_minor = puff::kVersionMinor;
    h.source_id = source;
    h.session = session;
    h.origin_id = 1;
    h.hold_us = 200000;  // 0.2 s
    return h;
}

std::vector<std::uint8_t> origin_datagram(puff::Header h, double lat_deg = 34.6,
                                          double lon_deg = -86.7, double height_msl = 190.0) {
    std::vector<std::uint8_t> b(puff::kMaxDatagram);
    b.resize(puff::write_origin(b, h, {lat_deg * kDeg, lon_deg * kDeg, height_msl}));
    return b;
}

std::vector<std::uint8_t> puff_datagram(puff::Header h, std::span<const puff::Puff> puffs) {
    std::vector<std::uint8_t> b(puff::kMaxDatagram);
    b.resize(puff::write_puffs(b, h, puffs));
    return b;
}

puff::Puff puff_at(std::uint32_t id, float east, float v_east = 0.0F) {
    puff::Puff p;
    p.id = id;
    p.east = east;
    p.north = 2.0F;
    p.up = 3.0F;
    p.v_east = v_east;
    p.radius = 1.5F;
    p.bulk_density = 0.01F;
    return p;
}

TEST(PuffStream, RoundTripsTheHeaderOriginAndPuffs) {
    puff::Header h = header();
    h.flags = puff::kFlagFrozen;
    h.sim_time_ns = 123456789012ULL;
    h.sequence = 42;
    const std::array<puff::Puff, 2> in{puff_at(1, 10.0F, 3.0F), puff_at(2, -4.0F)};
    const auto d = puff_datagram(h, in);
    ASSERT_EQ(d.size(), puff::kHeaderBytes + 2 * puff::kPuffBytes);
    puff::Header out;
    ASSERT_EQ(puff::read_header(d, out), puff::HeaderResult::ok);
    EXPECT_EQ(out.type, static_cast<std::uint8_t>(puff::Type::puffs));
    EXPECT_EQ(out.flags, puff::kFlagFrozen);
    EXPECT_EQ(out.source_id, 7U);
    EXPECT_EQ(out.session, 1U);
    EXPECT_EQ(out.sim_time_ns, 123456789012ULL);
    EXPECT_EQ(out.sequence, 42U);
    EXPECT_EQ(out.hold_us, 200000U);
    EXPECT_EQ(out.count, 2);
    const puff::Puff p = puff::read_puff(d, out, 0);
    EXPECT_EQ(p.id, 1U);
    EXPECT_EQ(p.east, 10.0F);
    EXPECT_EQ(p.v_east, 3.0F);
    EXPECT_EQ(p.radius, 1.5F);
    EXPECT_EQ(p.bulk_density, 0.01F);
    EXPECT_EQ(puff::read_puff(d, out, 1).east, -4.0F);

    const auto o = origin_datagram(header(), 34.6, -86.7, 190.0);
    ASSERT_EQ(puff::read_header(o, out), puff::HeaderResult::ok);
    EXPECT_EQ(out.type, static_cast<std::uint8_t>(puff::Type::origin));
    EXPECT_DOUBLE_EQ(puff::read_origin(o, out).longitude_rad, -86.7 * kDeg);
    EXPECT_DOUBLE_EQ(puff::read_origin(o, out).height_msl_m, 190.0);
}

// Little-endian on the wire, field by field: the magic's bytes are "PUFF".
TEST(PuffStream, TheWireIsLittleEndian) {
    const auto d = origin_datagram(header());
    EXPECT_EQ(d[0], 'P');
    EXPECT_EQ(d[1], 'U');
    EXPECT_EQ(d[2], 'F');
    EXPECT_EQ(d[3], 'F');
    EXPECT_EQ(d[12], 7);  // source_id's low byte
    EXPECT_EQ(d[10], puff::kHeaderBytes);
}

TEST(PuffStream, RejectsWhatIsntOne) {
    const std::array<puff::Puff, 3> three{puff_at(1, 0), puff_at(2, 0), puff_at(3, 0)};
    const auto good = puff_datagram(header(), three);
    puff::Header h;
    auto bad = good;
    bad[0] = 'X';  // the magic
    EXPECT_EQ(puff::read_header(bad, h), puff::HeaderResult::rejected);
    bad = good;
    bad[4] = 1;  // major 1
    EXPECT_EQ(puff::read_header(bad, h), puff::HeaderResult::rejected);
    // Shorter than the header, or than the records it declares.
    EXPECT_EQ(puff::read_header(std::span(good).first(40), h), puff::HeaderResult::rejected);
    EXPECT_EQ(puff::read_header(std::span(good).first(good.size() - 1), h),
              puff::HeaderResult::rejected);
    // A record shorter than this version's, a header shorter than this
    // version's, or longer than the datagram.
    bad = good;
    bad[36] = 20;
    EXPECT_EQ(puff::read_header(bad, h), puff::HeaderResult::rejected);
    bad = good;
    bad[10] = 40;
    EXPECT_EQ(puff::read_header(bad, h), puff::HeaderResult::rejected);
    bad = good;
    bad[10] = 0xFF;
    bad[11] = 0xFF;
    EXPECT_EQ(puff::read_header(bad, h), puff::HeaderResult::rejected);
    // An origin with no room for its body.
    const auto o = origin_datagram(header());
    EXPECT_EQ(puff::read_header(std::span(o).first(o.size() - 1), h), puff::HeaderResult::rejected);
    // The largest count and record can't overflow into a small need.
    bad = good;
    bad[36] = bad[37] = bad[38] = bad[39] = 0xFF;
    EXPECT_EQ(puff::read_header(bad, h), puff::HeaderResult::rejected);
}

// A newer minor: a longer header and record are read for what's known, and
// a type this version doesn't know is skipped, not rejected.
TEST(PuffStream, ReadsANewerMinor) {
    const std::array<puff::Puff, 2> two{puff_at(1, 5.0F), puff_at(2, 6.0F)};
    const auto v1 = puff_datagram(header(), two);
    // Rebuild with an 8-byte longer header and 4-byte longer records.
    std::vector<std::uint8_t> d(v1.begin(), v1.begin() + 48);
    d.insert(d.end(), 8, 0xAA);
    for (int i = 0; i < 2; ++i) {
        const auto at = v1.begin() + 48 + i * 36;
        d.insert(d.end(), at, at + 36);
        d.insert(d.end(), 4, 0xBB);
    }
    d[6] = 2;    // minor 2
    d[10] = 56;  // header_bytes
    d[36] = 40;  // record_bytes
    puff::Header h;
    ASSERT_EQ(puff::read_header(d, h), puff::HeaderResult::ok);
    EXPECT_EQ(puff::read_puff(d, h, 1).id, 2U);
    EXPECT_EQ(puff::read_puff(d, h, 1).east, 6.0F);
    d[8] = 9;  // a new type
    EXPECT_EQ(puff::read_header(d, h), puff::HeaderResult::unknown_type);
}

TEST(PuffStream, WritingRefusesASmallBuffer) {
    std::array<std::uint8_t, 50> small{};
    EXPECT_EQ(puff::write_origin(small, header(), {}), 0U);
    const std::array<puff::Puff, 1> one{puff_at(1, 0)};
    EXPECT_EQ(puff::write_puffs(small, header(), one), 0U);
}

// The adapter, fed datagrams directly (its socket on any free port, unused).
class PuffAdapterTest : public ::testing::Test {
protected:
    ParticleState state{100};
    PuffAdapterOptions options() {
        PuffAdapterOptions o;
        o.port = 0;
        o.address = kLoopback;
        o.fade = 0.1s;
        return o;
    }
    PuffAdapter adapter{options(), state};
    LocalTime t0 = LocalTime{} + 1000s;

    void send(const std::vector<std::uint8_t>& d, LocalTime t) { adapter.apply(d, t); }
    const ParticleFrame& published(LocalTime t) {
        adapter.publish(t);
        return state.latest();
    }
    std::uint64_t stat(const std::atomic<std::uint64_t>& v) { return v.load(); }
};

TEST_F(PuffAdapterTest, KeepsPuffsByIdAndPlacesTheirOrigin) {
    send(origin_datagram(header(), 34.6, -86.7, 190.0), t0);
    const std::array<puff::Puff, 2> two{puff_at(1, 10.0F), puff_at(2, 20.0F)};
    send(puff_datagram(header(), two), t0);
    const ParticleFrame& f = published(t0);
    ASSERT_EQ(f.count, 2U);
    EXPECT_EQ(f.particles[1].position.x, 20.0F);
    EXPECT_EQ(f.particles[0].first_seen, t0);
    // The origin: on the ellipsoid at the MSL height (geoid 0 here), with
    // east, north and up as ECEF unit vectors.
    const ParticleOrigin& o = f.origins[f.particles[0].origin];
    const DVec3 expected = geodetic_to_ecef({34.6, -86.7, 190.0});
    EXPECT_NEAR(glm::length(o.ecef - expected), 0.0, 1e-6);
    const DVec3 up = o.enu_to_ecef[2];
    EXPECT_NEAR(glm::dot(up, glm::normalize(expected)), 1.0, 0.01);  // close to radial
    EXPECT_NEAR(glm::dot(o.enu_to_ecef[0], o.enu_to_ecef[1]), 0.0, 1e-12);
    EXPECT_NEAR(glm::dot(glm::cross(o.enu_to_ecef[0], o.enu_to_ecef[1]), up), 1.0, 1e-12);
    EXPECT_EQ(stat(adapter.stats().records), 2U);
}

TEST_F(PuffAdapterTest, PuffsBeforeTheirOriginAreDropped) {
    const std::array<puff::Puff, 1> one{puff_at(1, 0)};
    send(puff_datagram(header(), one), t0);
    EXPECT_EQ(published(t0).count, 0U);
    EXPECT_EQ(stat(adapter.stats().unknown_origin), 1U);
    send(origin_datagram(header()), t0);
    send(puff_datagram(header(), one), t0);
    EXPECT_EQ(published(t0).count, 1U);
}

// A record is taken unless the one held is newer: an equal time (a frozen
// sender's refreshes) is taken, an older one (a late datagram) isn't.
TEST_F(PuffAdapterTest, TheNewestRecordWinsAndAnEqualTimeIsTaken) {
    send(origin_datagram(header()), t0);
    puff::Header h = header();
    h.sim_time_ns = 2000;
    const std::array<puff::Puff, 1> newer{puff_at(1, 5.0F)};
    send(puff_datagram(h, newer), t0);
    h.sim_time_ns = 1000;
    const std::array<puff::Puff, 1> older{puff_at(1, 99.0F)};
    send(puff_datagram(h, older), t0 + 10ms);
    EXPECT_EQ(published(t0 + 10ms).particles[0].position.x, 5.0F);
    EXPECT_EQ(stat(adapter.stats().late), 1U);
    // Frozen: the same time again, refreshing it.
    h.sim_time_ns = 2000;
    h.flags = puff::kFlagFrozen;
    send(puff_datagram(h, newer), t0 + 150ms);
    const ParticleFrame& f = published(t0 + 150ms);
    EXPECT_EQ(f.particles[0].received, t0 + 150ms);
    EXPECT_TRUE(f.particles[0].frozen);
    EXPECT_EQ(stat(adapter.stats().late), 1U);
}

// A particle not refreshed within hold_us fades out over the fade, then goes.
TEST_F(PuffAdapterTest, AParticleNotRefreshedFadesThenGoes) {
    send(origin_datagram(header()), t0);
    const std::array<puff::Puff, 1> one{puff_at(1, 0)};
    send(puff_datagram(header(), one), t0);  // hold 0.2 s, fade 0.1 s
    const Particle p = published(t0).particles[0];
    EXPECT_EQ(p.fade_from, t0 + 200ms);
    EXPECT_FLOAT_EQ(particle_opacity_at(p, t0 + 50ms, 0.1s), 0.5F);  // fading in
    EXPECT_FLOAT_EQ(particle_opacity_at(p, t0 + 150ms, 0.1s), 1.0F);
    EXPECT_FLOAT_EQ(particle_opacity_at(p, t0 + 250ms, 0.1s), 0.5F);  // fading out
    EXPECT_EQ(published(t0 + 290ms).count, 1U);
    EXPECT_EQ(published(t0 + 300ms).count, 0U);
}

TEST_F(PuffAdapterTest, ParticlesMoveOnTheirVelocityUnlessFrozen) {
    send(origin_datagram(header()), t0);
    const std::array<puff::Puff, 1> one{puff_at(1, 10.0F, 4.0F)};
    send(puff_datagram(header(), one), t0);
    Particle p = published(t0).particles[0];
    EXPECT_FLOAT_EQ(particle_position_at(p, t0 + 500ms).x, 12.0F);
    EXPECT_FLOAT_EQ(particle_position_at(p, t0 - 1s).x, 10.0F);  // never before its sample
    p.frozen = true;
    EXPECT_FLOAT_EQ(particle_position_at(p, t0 + 500ms).x, 10.0F);
}

// A restarted sender (a new session) starts its time again: its old
// particles and origins are dropped, so the new ones aren't refused as late.
TEST_F(PuffAdapterTest, ANewSessionResetsItsSource) {
    send(origin_datagram(header(7, 1)), t0);
    puff::Header old = header(7, 1);
    old.sim_time_ns = 1'000'000'000'000ULL;
    const std::array<puff::Puff, 2> two{puff_at(1, 1.0F), puff_at(2, 2.0F)};
    send(puff_datagram(old, two), t0);
    // Another source's particles are untouched.
    send(origin_datagram(header(8, 1)), t0);
    const std::array<puff::Puff, 1> theirs{puff_at(1, 50.0F)};
    send(puff_datagram(header(8, 1), theirs), t0);
    ASSERT_EQ(published(t0).count, 3U);
    // Restarted: the old origin is gone with it, so this waits for the new one.
    puff::Header fresh = header(7, 2);
    fresh.sim_time_ns = 5;
    const std::array<puff::Puff, 1> one{puff_at(1, 9.0F)};
    send(puff_datagram(fresh, one), t0 + 10ms);
    EXPECT_EQ(stat(adapter.stats().unknown_origin), 1U);
    send(origin_datagram(header(7, 2)), t0 + 10ms);
    send(puff_datagram(fresh, one), t0 + 10ms);
    const ParticleFrame& f = published(t0 + 10ms);
    ASSERT_EQ(f.count, 2U);  // source 8's one and source 7's new one
    float east7 = 0.0F;
    float east8 = 0.0F;
    for (std::size_t i = 0; i < f.count; ++i) {
        (f.particles[i].position.x == 50.0F ? east8 : east7) = f.particles[i].position.x;
    }
    EXPECT_EQ(east7, 9.0F);
    EXPECT_EQ(east8, 50.0F);
    EXPECT_EQ(stat(adapter.stats().late), 0U);
    EXPECT_EQ(stat(adapter.stats().sessions), 3U);
}

// Particles beyond the capacity are dropped and counted; a gone one frees
// its place.
TEST_F(PuffAdapterTest, TheCapacityIsAHardLimit) {
    send(origin_datagram(header()), t0);
    std::vector<puff::Puff> many;
    for (std::uint32_t i = 0; i < 39; ++i) {
        many.push_back(puff_at(i, static_cast<float>(i)));
    }
    for (std::uint32_t batch = 0; batch < 3; ++batch) {  // 117 ids into 100 places
        for (auto& p : many) {
            p.id = batch * 39 + (p.id % 39);
        }
        send(puff_datagram(header(), many), t0);
    }
    EXPECT_EQ(published(t0).count, 100U);
    EXPECT_EQ(stat(adapter.stats().table_full), 17U);
    // All faded and gone: room again.
    EXPECT_EQ(published(t0 + 1s).count, 0U);
    send(puff_datagram(header(), many), t0 + 1s);
    EXPECT_EQ(published(t0 + 1s).count, 39U);
}

// Losses are counted over a window from the span of sequences seen, so a
// reordering isn't a loss.
TEST_F(PuffAdapterTest, LossesAreCountedOverSequenceSpans) {
    send(origin_datagram(header()), t0);  // sequence 0
    adapter.publish(t0);                  // closes its window: the next expects 1
    puff::Header h = header();
    const std::array<puff::Puff, 1> one{puff_at(1, 0)};
    for (const std::uint32_t s : {10U, 12U, 11U, 13U, 16U}) {  // 11 late, 14 and 15 lost
        h.sequence = s;
        send(puff_datagram(h, one), t0 + 100ms);
    }
    adapter.publish(t0 + 2s);
    EXPECT_EQ(stat(adapter.stats().lost), 11U);  // 1..9, 14 and 15; 11's lateness isn't a loss
}

// A loss across a window's boundary is counted; a datagram of a newer
// minor's type took its sequence number, so it isn't a loss; a wild jump (a
// corrupt sequence) starts counting again rather than adding billions.
TEST_F(PuffAdapterTest, LossesAcrossWindowsSkippedTypesAndJumps) {
    puff::Header h = header();
    const std::array<puff::Puff, 1> one{puff_at(1, 0)};
    send(origin_datagram(h), t0);  // 0
    for (const std::uint32_t s : {1U, 2U, 3U, 4U}) {
        h.sequence = s;
        send(puff_datagram(h, one), t0);
    }
    adapter.publish(t0 + 1s);                 // closes 0..4
    for (const std::uint32_t s : {7U, 8U}) {  // 5 and 6 lost across the boundary
        h.sequence = s;
        send(puff_datagram(h, one), t0 + 1100ms);
    }
    auto skipped = puff_datagram(h, one);
    skipped[8] = 9;   // a type this version doesn't know
    skipped[24] = 9;  // sequence 9
    send(skipped, t0 + 1100ms);
    h.sequence = 10;
    send(puff_datagram(h, one), t0 + 1100ms);
    adapter.publish(t0 + 2s);
    EXPECT_EQ(stat(adapter.stats().lost), 2U);
    EXPECT_EQ(stat(adapter.stats().skipped), 1U);
    h.sequence = 10 + 0x7FFF0000U;  // corrupt
    send(puff_datagram(h, one), t0 + 2100ms);
    h.sequence = 11 + 0x7FFF0000U;
    send(puff_datagram(h, one), t0 + 2100ms);
    adapter.publish(t0 + 3s);
    EXPECT_EQ(stat(adapter.stats().lost), 2U);
}

// A sender silent for PuffAdapter::kForget with no particles left is
// forgotten, so a ninth can come after eight have gone.
TEST_F(PuffAdapterTest, SilentSourcesAreForgotten) {
    for (std::uint32_t s = 1; s <= PuffAdapter::kMaxSources; ++s) {
        send(origin_datagram(header(s)), t0);
    }
    send(origin_datagram(header(99)), t0);
    EXPECT_EQ(stat(adapter.stats().rejected), 1U);  // over the table
    adapter.publish(t0 + 11s);
    send(origin_datagram(header(99)), t0 + 11s);
    EXPECT_EQ(stat(adapter.stats().rejected), 1U);
    EXPECT_EQ(stat(adapter.stats().origins), 9U);
}

// A stray datagram from before a restart is refused: it mustn't reset the
// new session.
TEST_F(PuffAdapterTest, AStrayFromThePreviousSessionIsRefused) {
    send(origin_datagram(header(7, 1)), t0);
    send(origin_datagram(header(7, 2)), t0);  // restarted
    const std::array<puff::Puff, 1> one{puff_at(1, 4.0F)};
    send(puff_datagram(header(7, 2), one), t0);
    send(puff_datagram(header(7, 1), one), t0);  // a stray from session 1
    EXPECT_EQ(published(t0).count, 1U);
    EXPECT_EQ(stat(adapter.stats().rejected), 1U);
    EXPECT_EQ(stat(adapter.stats().sessions), 2U);
}

// A sender re-centring under new ids frees the old origins once no particle
// names them: far more origins over time than the table holds at once.
TEST_F(PuffAdapterTest, OriginsNoParticleNamesAreFreed) {
    const std::array<puff::Puff, 1> one{puff_at(1, 0)};
    for (std::uint32_t k = 0; k < 3 * kMaxParticleOrigins; ++k) {
        puff::Header h = header();
        h.origin_id = k + 1;
        h.sim_time_ns = k;
        const LocalTime t = t0 + std::chrono::milliseconds(500 * k);
        send(origin_datagram(h), t);
        send(puff_datagram(h, one), t);  // the one particle moves to the new origin
        adapter.publish(t);
    }
    EXPECT_EQ(stat(adapter.stats().origins_full), 0U);
    EXPECT_EQ(stat(adapter.stats().unknown_origin), 0U);
    EXPECT_EQ(published(t0 + 100s).count, 0U);
}

// The table under churn, against a reference: particles arriving, refreshed
// and expiring at random, the count always what the reference says, and a
// refreshed id never held twice.
TEST(PuffAdapter, TheTableStaysRightUnderChurn) {
    ParticleState state(1000);
    PuffAdapterOptions o;
    o.address = kLoopback;
    o.fade = 0.05s;
    PuffAdapter adapter(o, state);
    const LocalTime t0 = LocalTime{} + 1000s;
    puff::Header h = header();
    std::vector<std::uint8_t> d(puff::kMaxDatagram);
    d.resize(puff::write_origin(d, h, {0.6, -1.5, 190.0}));
    adapter.apply(d, t0);
    std::map<std::uint32_t, LocalTime> gone_at;  // id -> when it goes (fade_from + fade)
    std::uint32_t rng = 12345;
    const auto next = [&rng] {
        rng = rng * 1664525U + 1013904223U;
        return rng >> 8;
    };
    for (int step = 1; step <= 400; ++step) {
        const LocalTime t = t0 + std::chrono::milliseconds(10 * step);
        std::vector<puff::Puff> batch;
        for (int i = 0; i < 39; ++i) {
            batch.push_back(puff_at(next() % 800, 1.0F));
        }
        h.hold_us = 20000 + (next() % 5) * 20000;  // 20 to 100 ms
        h.sim_time_ns = static_cast<std::uint64_t>(step);
        std::vector<std::uint8_t> p(puff::kMaxDatagram);
        p.resize(puff::write_puffs(p, h, batch));
        adapter.apply(p, t);
        for (const auto& b : batch) {
            gone_at[b.id] = t + std::chrono::microseconds(h.hold_us) + 50ms;
        }
        adapter.publish(t);
        std::size_t expected = 0;
        for (const auto& [id, when] : gone_at) {
            expected += t < when ? 1U : 0U;
        }
        const ParticleFrame& f = state.latest();
        ASSERT_EQ(f.count, expected) << "step " << step;
    }
    EXPECT_EQ(adapter.stats().table_full.load(), 0U);
}

TEST_F(PuffAdapterTest, NonFiniteOrNegativePuffsAreRejected) {
    send(origin_datagram(header()), t0);
    std::array<puff::Puff, 3> bad{puff_at(1, 0), puff_at(2, 0), puff_at(3, 0)};
    bad[0].east = std::nanf("");
    bad[1].radius = -1.0F;
    send(puff_datagram(header(), bad), t0);
    EXPECT_EQ(published(t0).count, 1U);
    EXPECT_EQ(stat(adapter.stats().rejected_records), 2U);
    EXPECT_EQ(stat(adapter.stats().rejected), 0U);
}

TEST_F(PuffAdapterTest, ApplyingAndPublishingAllocateNothing) {
    std::vector<std::vector<std::uint8_t>> datagrams;
    datagrams.push_back(origin_datagram(header()));
    std::vector<puff::Puff> many;
    for (std::uint32_t i = 0; i < 39; ++i) {
        many.push_back(puff_at(i, static_cast<float>(i)));
    }
    datagrams.push_back(puff_datagram(header(), many));
    const test::AllocationCounter counter;
    for (int k = 0; k < 50; ++k) {
        for (const auto& d : datagrams) {
            adapter.apply(d, t0 + std::chrono::milliseconds(k));
        }
        adapter.publish(t0 + std::chrono::milliseconds(k));
    }
    adapter.publish(t0 + 10s);  // and dropping them all
    EXPECT_EQ(counter.count(), 0U);
}

// Through the socket: a datagram sent to its port is applied by poll().
TEST(PuffAdapter, ReceivesOverUdp) {
    ParticleState state(10);
    PuffAdapterOptions o;
    o.address = kLoopback;
    o.wait_ms = 50;
    o.publish_period = 0ms;
    PuffAdapter adapter(o, state);
    UdpSocket sender(0, kLoopback);
    const auto origin = origin_datagram(header());
    const std::array<puff::Puff, 1> one{puff_at(3, 1.0F)};
    const auto puffs = puff_datagram(header(), one);
    ASSERT_TRUE(sender.send_to(origin, {kLoopback, adapter.port()}));
    ASSERT_TRUE(sender.send_to(puffs, {kLoopback, adapter.port()}));
    for (int i = 0; i < 40 && state.latest().count == 0; ++i) {
        adapter.poll();
    }
    EXPECT_EQ(state.latest().count, 1U);
}

}  // namespace
}  // namespace fovea
