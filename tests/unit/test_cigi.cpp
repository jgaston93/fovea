#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/net/cigi.hpp"
#include "support/alloc_counter.hpp"

// The expected bytes here are written by hand from the ICD's packet figures
// (CIGI 3.3 ICD: Figure 39, p. 42; Figure 42, p. 48; Figure 53, p. 82;
// Figure 54, p. 85; Figure 65, p. 101; Figure 86, p. 152; Figure 109,
// p. 212; Figure 110, p. 217; Figure 112, p. 220), not
// produced by the encoder, as a big-endian or a little-endian sender would
// send them.

namespace fovea::cigi {
namespace {

using Bytes = std::vector<std::uint8_t>;

// A value's bytes, most significant first (big-endian), or reversed.
template <typename T>
Bytes be(T value) {
    auto raw = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    if constexpr (std::endian::native == std::endian::little) {
        std::reverse(raw.begin(), raw.end());
    }
    return {raw.begin(), raw.end()};
}

template <typename T>
Bytes le(T value) {
    Bytes b = be(value);
    std::reverse(b.begin(), b.end());
    return b;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// Figure 39 as a big-endian host sends it: database 5; minor version 2,
// extrapolation disabled, timestamp valid, IG mode operate (neighbouring bits
// differ, so a field read from the wrong bit shows); host frame
// 0x01020304, timestamp 0x0A0B0C0D, last IG frame 0x11223344.
const Bytes kIgControlBigEndian = {
    0x01, 0x18, 0x03, 0x05,  // Packet ID 1, Packet Size 24, Major Version 3, Database 5
    0x25, 0x00, 0x80, 0x00,  // 0010 0 1 01, Reserved, Byte Swap Magic Number 8000h
    0x01, 0x02, 0x03, 0x04,  // Host Frame Number
    0x0A, 0x0B, 0x0C, 0x0D,  // Timestamp
    0x11, 0x22, 0x33, 0x44,  // Last IG Frame Number
    0x00, 0x00, 0x00, 0x00,  // Reserved
};

// The same packet from a little-endian host.
const Bytes kIgControlLittleEndian = {
    0x01, 0x18, 0x03, 0x05, 0x25, 0x00, 0x00, 0x80, 0x04, 0x03, 0x02, 0x01,
    0x0D, 0x0C, 0x0B, 0x0A, 0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x00, 0x00,
};

void expect_ig_control(const IgControl& p) {
    EXPECT_EQ(p.database_number, 5);
    EXPECT_EQ(p.minor_version, 2);
    EXPECT_FALSE(p.extrapolation_enable);
    EXPECT_TRUE(p.timestamp_valid);
    EXPECT_EQ(p.ig_mode, IgMode::operate);
    EXPECT_EQ(p.host_frame_number, 0x01020304U);
    EXPECT_EQ(p.timestamp, 0x0A0B0C0DU);
    EXPECT_EQ(p.last_ig_frame_number, 0x11223344U);
}

// Figure 42 in a given byte order: entity 0x0102; ground clamp conformal (2),
// inherit alpha, attach, entity state active; extrapolation enable, animation
// state continue (3), direction backward; alpha 0x80; type 0x0304, parent
// 0x0506; roll -12.5, pitch 3.25, yaw 270; 34.6372, -86.7751, 190.5.
template <typename Order>
Bytes entity_control(Order order) {
    return cat({
        {0x02, 0x30},  // Packet ID 2, Packet Size 48
        order(std::uint16_t{0x0102}),
        {0x55},  // 0 10 1 0 1 01: Reserved, Clamp 2, Inherit Alpha, no Collision, Attach, Active
        {0x1D},  // 000 1 11 0 1: Reserved, Extrapolation, Continue, One-Shot, Backward
        {0x80, 0x00},  // Alpha, Reserved
        order(std::uint16_t{0x0304}),
        order(std::uint16_t{0x0506}),
        order(-12.5F),
        order(3.25F),
        order(270.0F),
        order(34.6372),
        order(-86.7751),
        order(190.5),
    });
}

void expect_entity_control(const EntityControl& p) {
    EXPECT_EQ(p.entity_id, 0x0102);
    EXPECT_EQ(p.ground_ocean_clamp, GroundClamp::conformal);
    EXPECT_TRUE(p.inherit_alpha);
    EXPECT_FALSE(p.collision_detection);
    EXPECT_TRUE(p.attach);
    EXPECT_EQ(p.entity_state, EntityState::active);
    EXPECT_TRUE(p.extrapolation_enable);
    EXPECT_EQ(p.animation_state, AnimationState::continue_);
    EXPECT_FALSE(p.animation_loop);
    EXPECT_TRUE(p.animation_backward);
    EXPECT_EQ(p.alpha, 0x80);
    EXPECT_EQ(p.entity_type, 0x0304);
    EXPECT_EQ(p.parent_id, 0x0506);
    EXPECT_EQ(p.roll_deg, -12.5F);
    EXPECT_EQ(p.pitch_deg, 3.25F);
    EXPECT_EQ(p.yaw_deg, 270.0F);
    EXPECT_EQ(p.lat_or_x, 34.6372);
    EXPECT_EQ(p.lon_or_y, -86.7751);
    EXPECT_EQ(p.alt_or_z, 190.5);
}

// Figure 53 in a given byte order: 14:35; Date/Time Valid, Moon Enable and
// Ephemeris Model Enable (Star Field and Sun not: neighbouring bits
// differ); 3 November 2026; star field 62.5 percent.
template <typename Order>
Bytes celestial_sphere_control(Order order) {
    return cat({
        {0x09, 0x10, 0x0E, 0x23},  // Packet ID 9, Packet Size 16, Hour 14, Minute 35
        {0x15},                    // 000 1 0 1 0 1: Reserved, Valid, Stars, Moon, Sun, Ephemeris
        {0x00, 0x00, 0x00},        // Reserved
        order(std::uint32_t{11032026}),
        order(62.5F),
    });
}

void expect_celestial_sphere_control(const CelestialSphereControl& p) {
    EXPECT_EQ(p.hour, 14);
    EXPECT_EQ(p.minute, 35);
    EXPECT_TRUE(p.date_time_valid);
    EXPECT_FALSE(p.star_field_enable);
    EXPECT_TRUE(p.moon_enable);
    EXPECT_FALSE(p.sun_enable);
    EXPECT_TRUE(p.ephemeris_enable);
    EXPECT_EQ(p.date, 11032026U);
    EXPECT_EQ(p.star_field_intensity, 62.5F);
}

// Figure 54 in a given byte order: the atmospheric model enabled, humidity
// 55 percent, 21.5 C, visibility 8000 m, wind 6.5 m/s horizontal and -1.25
// vertical from 270 degrees, 1013.25 mb.
template <typename Order>
Bytes atmosphere_control(Order order) {
    return cat({
        {0x0A, 0x20, 0x01, 0x37},  // Packet ID 10, Packet Size 32, 0000000 1, Humidity 55
        order(21.5F),
        order(8000.0F),
        order(6.5F),
        order(-1.25F),
        order(270.0F),
        order(1013.25F),
        {0x00, 0x00, 0x00, 0x00},  // Reserved
    });
}

void expect_atmosphere_control(const AtmosphereControl& p) {
    EXPECT_TRUE(p.atmospheric_model_enable);
    EXPECT_EQ(p.humidity, 55);
    EXPECT_EQ(p.air_temperature_c, 21.5F);
    EXPECT_EQ(p.visibility_range_m, 8000.0F);
    EXPECT_EQ(p.horizontal_wind_mps, 6.5F);
    EXPECT_EQ(p.vertical_wind_mps, -1.25F);
    EXPECT_EQ(p.wind_direction_deg, 270.0F);
    EXPECT_EQ(p.barometric_pressure_mb, 1013.25F);
}

// Figure 65 in a given byte order: region 0x0102, Layer ID 2 (cloud layer
// 2), humidity 90; cloud type 10 (stratus), random lightning and scud on,
// random winds off, weather enabled; severity 5, scope regional; then each
// float its own value.
template <typename Order>
Bytes weather_control(Order order) {
    return cat({
        {0x0C, 0x38},  // Packet ID 12, Packet Size 56
        order(std::uint16_t{0x0102}),
        {0x02, 0x5A},    // Layer ID 2, Humidity 90
        {0xAB},          // 1010 1 0 1 1: Cloud Type 10, Lightning, no Winds, Scud, Enable
        {0x15},          // 000 101 01: Reserved, Severity 5, Scope Regional
        order(-5.5F),    // Air Temperature
        order(3000.0F),  // Visibility Range
        order(12.5F),    // Scud Frequency
        order(75.0F),    // Coverage
        order(1500.0F),  // Base Elevation
        order(400.0F),   // Thickness
        order(50.0F),    // Transition Band
        order(9.0F),     // Horizontal Wind Speed
        order(0.5F),     // Vertical Wind Speed
        order(180.0F),   // Wind Direction
        order(850.0F),   // Barometric Pressure
        order(0.25F),    // Aerosol Concentration
    });
}

void expect_weather_control(const WeatherControl& p) {
    EXPECT_EQ(p.entity_or_region_id, 0x0102);
    EXPECT_EQ(p.layer_id, 2);
    EXPECT_EQ(p.humidity, 90);
    EXPECT_EQ(p.cloud_type, 10);
    EXPECT_TRUE(p.random_lightning_enable);
    EXPECT_FALSE(p.random_winds_enable);
    EXPECT_TRUE(p.scud_enable);
    EXPECT_TRUE(p.weather_enable);
    EXPECT_EQ(p.severity, 5);
    EXPECT_EQ(p.scope, WeatherScope::regional);
    EXPECT_EQ(p.air_temperature_c, -5.5F);
    EXPECT_EQ(p.visibility_range_m, 3000.0F);
    EXPECT_EQ(p.scud_frequency, 12.5F);
    EXPECT_EQ(p.coverage, 75.0F);
    EXPECT_EQ(p.base_elevation_m, 1500.0F);
    EXPECT_EQ(p.thickness_m, 400.0F);
    EXPECT_EQ(p.transition_band_m, 50.0F);
    EXPECT_EQ(p.horizontal_wind_mps, 9.0F);
    EXPECT_EQ(p.vertical_wind_mps, 0.5F);
    EXPECT_EQ(p.wind_direction_deg, 180.0F);
    EXPECT_EQ(p.barometric_pressure_mb, 850.0F);
    EXPECT_EQ(p.aerosol_concentration, 0.25F);
}

// Figure 109 as a big-endian IG sends it: database -1 (loading 1), status 7;
// minor version 2, host-defined earth model, timestamp not valid, IG mode
// debug.
const Bytes kStartOfFrameBigEndian = {
    0x65, 0x18, 0x03, 0xFF,  // Packet ID 101, Packet Size 24, Major Version 3, Database -1
    0x07, 0x2A, 0x80, 0x00,  // IG Status 7, 0010 1 0 10, Byte Swap Magic Number 8000h
    0x00, 0x00, 0x01, 0x00,  // IG Frame Number 256
    0x00, 0x01, 0x86, 0xA0,  // Timestamp 100000 (one second)
    0xFF, 0xFF, 0xFF, 0xFE,  // Last Host Frame Number
    0x00, 0x00, 0x00, 0x00,  // Reserved
};

// The native-order bytes a message decodes as needing a swap, or not.
bool needs_swap_from_big_endian() {
    return std::endian::native == std::endian::little;
}

MessageReader::Packet first_packet(const Bytes& message, std::uint8_t first_id) {
    MessageReader reader(message, first_id);
    MessageReader::Packet p;
    EXPECT_TRUE(reader.next(p)) << static_cast<int>(reader.error());
    return p;
}

TEST(Cigi, IgControlFromABigEndianHost) {
    MessageReader reader(kIgControlBigEndian, kIgControlId);
    EXPECT_EQ(reader.error(), MessageError::none);
    EXPECT_EQ(reader.swapped(), needs_swap_from_big_endian());
    MessageReader::Packet p;
    ASSERT_TRUE(reader.next(p));
    const auto ig = decode_ig_control(p.bytes, reader.swapped());
    ASSERT_TRUE(ig.has_value());
    expect_ig_control(*ig);
}

TEST(Cigi, IgControlFromALittleEndianHost) {
    MessageReader reader(kIgControlLittleEndian, kIgControlId);
    EXPECT_EQ(reader.swapped(), !needs_swap_from_big_endian());
    const auto ig = decode_ig_control(first_packet(kIgControlLittleEndian, kIgControlId).bytes,
                                      reader.swapped());
    ASSERT_TRUE(ig.has_value());
    expect_ig_control(*ig);
}

TEST(Cigi, IgControlEncodesTheIcdBytesInNativeOrder) {
    IgControl ig;
    ig.database_number = 5;
    ig.timestamp_valid = true;
    ig.ig_mode = IgMode::operate;
    ig.host_frame_number = 0x01020304;
    ig.timestamp = 0x0A0B0C0D;
    ig.last_ig_frame_number = 0x11223344;
    std::array<std::uint8_t, 32> out{};
    out.fill(0xEE);
    ASSERT_EQ(encode(ig, out), 24U);
    const Bytes& expected =
        std::endian::native == std::endian::little ? kIgControlLittleEndian : kIgControlBigEndian;
    EXPECT_EQ(Bytes(out.begin(), out.begin() + 24), expected);
    EXPECT_EQ(out[24], 0xEE);  // nothing past the packet
}

TEST(Cigi, EntityControlInBothByteOrders) {
    for (const bool big : {true, false}) {
        const Bytes message = cat({big ? kIgControlBigEndian : kIgControlLittleEndian,
                                   big ? entity_control([](auto v) { return be(v); })
                                       : entity_control([](auto v) { return le(v); })});
        MessageReader reader(message, kIgControlId);
        MessageReader::Packet p;
        ASSERT_TRUE(reader.next(p));
        ASSERT_TRUE(reader.next(p));
        EXPECT_EQ(p.id, kEntityControlId);
        const auto e = decode_entity_control(p.bytes, reader.swapped());
        ASSERT_TRUE(e.has_value());
        SCOPED_TRACE(big ? "big-endian host" : "little-endian host");
        expect_entity_control(*e);
        EXPECT_FALSE(reader.next(p));
        EXPECT_EQ(reader.error(), MessageError::none);
    }
}

TEST(Cigi, EntityControlEncodesTheIcdBytes) {
    EntityControl e;
    e.entity_id = 0x0102;
    e.ground_ocean_clamp = GroundClamp::conformal;
    e.inherit_alpha = true;
    e.attach = true;
    e.entity_state = EntityState::active;
    e.extrapolation_enable = true;
    e.animation_state = AnimationState::continue_;
    e.animation_backward = true;
    e.alpha = 0x80;
    e.entity_type = 0x0304;
    e.parent_id = 0x0506;
    e.roll_deg = -12.5F;
    e.pitch_deg = 3.25F;
    e.yaw_deg = 270.0F;
    e.lat_or_x = 34.6372;
    e.lon_or_y = -86.7751;
    e.alt_or_z = 190.5;
    std::array<std::uint8_t, 48> out{};
    ASSERT_EQ(encode(e, out), 48U);
    const Bytes expected = std::endian::native == std::endian::little
                               ? entity_control([](auto v) { return le(v); })
                               : entity_control([](auto v) { return be(v); });
    EXPECT_EQ(Bytes(out.begin(), out.end()), expected);
}

// The environment's packets (milestone 6 step 7), from either byte order,
// and encoded as the ICD lays them out.
TEST(Cigi, EnvironmentPacketsInBothByteOrders) {
    for (const bool big : {true, false}) {
        SCOPED_TRACE(big ? "big-endian host" : "little-endian host");
        const auto order = [big](auto v) { return big ? be(v) : le(v); };
        const Bytes message = cat({big ? kIgControlBigEndian : kIgControlLittleEndian,
                                   celestial_sphere_control(order), atmosphere_control(order),
                                   weather_control(order)});
        MessageReader reader(message, kIgControlId);
        MessageReader::Packet p;
        ASSERT_TRUE(reader.next(p));
        ASSERT_TRUE(reader.next(p));
        EXPECT_EQ(p.id, kCelestialSphereControlId);
        const auto c = decode_celestial_sphere_control(p.bytes, reader.swapped());
        ASSERT_TRUE(c.has_value());
        expect_celestial_sphere_control(*c);
        ASSERT_TRUE(reader.next(p));
        EXPECT_EQ(p.id, kAtmosphereControlId);
        const auto a = decode_atmosphere_control(p.bytes, reader.swapped());
        ASSERT_TRUE(a.has_value());
        expect_atmosphere_control(*a);
        ASSERT_TRUE(reader.next(p));
        EXPECT_EQ(p.id, kWeatherControlId);
        const auto w = decode_weather_control(p.bytes, reader.swapped());
        ASSERT_TRUE(w.has_value());
        expect_weather_control(*w);
        EXPECT_FALSE(reader.next(p));
        EXPECT_EQ(reader.error(), MessageError::none);
    }
}

TEST(Cigi, EnvironmentPacketsEncodeTheIcdBytes) {
    const auto native = [](auto v) {
        return std::endian::native == std::endian::little ? le(v) : be(v);
    };
    const auto decoded = [](const Bytes& b, auto decode) {
        auto p = decode(b, false);
        EXPECT_TRUE(p.has_value());
        return p.value_or(typename decltype(p)::value_type{});
    };
    std::array<std::uint8_t, 56> out{};
    const Bytes c = celestial_sphere_control(native);
    ASSERT_EQ(encode(decoded(c, decode_celestial_sphere_control), out), 16U);
    EXPECT_EQ(Bytes(out.begin(), out.begin() + 16), c);
    const Bytes a = atmosphere_control(native);
    ASSERT_EQ(encode(decoded(a, decode_atmosphere_control), out), 32U);
    EXPECT_EQ(Bytes(out.begin(), out.begin() + 32), a);
    const Bytes w = weather_control(native);
    ASSERT_EQ(encode(decoded(w, decode_weather_control), out), 56U);
    EXPECT_EQ(Bytes(out.begin(), out.end()), w);
}

TEST(Cigi, StartOfFrameFromABigEndianIg) {
    MessageReader reader(kStartOfFrameBigEndian, kStartOfFrameId);
    EXPECT_EQ(reader.error(), MessageError::none);
    const auto sof = decode_start_of_frame(
        first_packet(kStartOfFrameBigEndian, kStartOfFrameId).bytes, reader.swapped());
    ASSERT_TRUE(sof.has_value());
    EXPECT_EQ(sof->database_number, -1);
    EXPECT_EQ(sof->ig_status, 7);
    EXPECT_EQ(sof->minor_version, 2);
    EXPECT_TRUE(sof->host_defined_earth_model);
    EXPECT_FALSE(sof->timestamp_valid);
    EXPECT_EQ(sof->ig_mode, IgMode::debug);
    EXPECT_EQ(sof->ig_frame_number, 256U);
    EXPECT_EQ(sof->timestamp, 100000U);
    EXPECT_EQ(sof->last_host_frame_number, 0xFFFFFFFEU);
}

TEST(Cigi, StartOfFrameRoundTrips) {
    StartOfFrame sof;
    sof.database_number = -128;
    sof.ig_status = 255;
    sof.ig_mode = IgMode::offline_maintenance;
    sof.timestamp_valid = true;
    sof.ig_frame_number = 0xDEADBEEF;
    sof.timestamp = 4294967295U;
    sof.last_host_frame_number = 1;
    std::array<std::uint8_t, 24> out{};
    ASSERT_EQ(encode(sof, out), 24U);
    MessageReader reader(out, kStartOfFrameId);
    EXPECT_FALSE(reader.swapped());
    const auto back = decode_start_of_frame(out, false);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->database_number, -128);
    EXPECT_EQ(back->ig_status, 255);
    EXPECT_EQ(back->minor_version, kMinorVersionSent);
    EXPECT_EQ(back->ig_mode, IgMode::offline_maintenance);
    EXPECT_FALSE(back->host_defined_earth_model);
    EXPECT_EQ(back->ig_frame_number, 0xDEADBEEFU);
    EXPECT_EQ(back->timestamp, 4294967295U);
    EXPECT_EQ(back->last_host_frame_number, 1U);
}

TEST(Cigi, ReservedBitsAreSentAsZero) {
    // Every field at its widest: anything else set would be a reserved bit.
    IgControl ig;
    ig.minor_version = 15;
    ig.ig_mode = IgMode::offline_maintenance;
    ig.timestamp_valid = ig.extrapolation_enable = true;
    std::array<std::uint8_t, 24> a{};
    a.fill(0xFF);
    ASSERT_EQ(encode(ig, a), 24U);
    EXPECT_EQ(a[5], 0);
    for (std::size_t i = 20; i < 24; ++i) {
        EXPECT_EQ(a[i], 0) << i;
    }
    EntityControl e;
    e.ground_ocean_clamp = static_cast<GroundClamp>(3);
    e.animation_state = AnimationState::continue_;
    e.inherit_alpha = e.collision_detection = e.attach = e.extrapolation_enable = e.animation_loop =
        e.animation_backward = true;
    e.entity_state = static_cast<EntityState>(3);
    std::array<std::uint8_t, 48> b{};
    b.fill(0xFF);
    ASSERT_EQ(encode(e, b), 48U);
    EXPECT_EQ(b[4] & 0x80, 0);  // byte 4 bit 7
    EXPECT_EQ(b[5] & 0xE0, 0);  // byte 5 bits 7-5
    EXPECT_EQ(b[7], 0);
    CelestialSphereControl c;
    c.date_time_valid = c.star_field_enable = c.moon_enable = c.sun_enable = c.ephemeris_enable =
        true;
    std::array<std::uint8_t, 56> d{};
    d.fill(0xFF);
    ASSERT_EQ(encode(c, d), 16U);
    EXPECT_EQ(d[4] & 0xE0, 0);  // byte 4 bits 7-5
    EXPECT_EQ(d[5] | d[6] | d[7], 0);
    AtmosphereControl at;
    at.atmospheric_model_enable = true;
    d.fill(0xFF);
    ASSERT_EQ(encode(at, d), 32U);
    EXPECT_EQ(d[2], 0x01);  // byte 2 bits 7-1
    EXPECT_EQ(d[28] | d[29] | d[30] | d[31], 0);
    WeatherControl w;
    w.severity = 7;
    w.scope = static_cast<WeatherScope>(3);
    d.fill(0xFF);
    ASSERT_EQ(encode(w, d), 56U);
    EXPECT_EQ(d[7] & 0xE0, 0);  // byte 7 bits 7-5
}

TEST(Cigi, UndefinedValuesRejectThePacket) {
    // Entity State 3 and Ground/Ocean Clamp 3 are undefined (p. 49, 51).
    Bytes state = entity_control([](auto v) { return be(v); });
    state[4] = static_cast<std::uint8_t>(state[4] | 0x03);
    EXPECT_FALSE(decode_entity_control(state, needs_swap_from_big_endian()).has_value());
    Bytes clamp = entity_control([](auto v) { return be(v); });
    clamp[4] = static_cast<std::uint8_t>(clamp[4] | 0x60);
    EXPECT_FALSE(decode_entity_control(clamp, needs_swap_from_big_endian()).has_value());
    // Weather Control Scope 3 and Severity 6 (p. 104). (Hour and Minute out
    // of range are the adapter's to refuse: without Date/Time Valid they
    // mean nothing.)
    Bytes scope = weather_control([](auto v) { return be(v); });
    scope[7] = static_cast<std::uint8_t>(scope[7] | 0x03);
    EXPECT_FALSE(decode_weather_control(scope, needs_swap_from_big_endian()).has_value());
    Bytes severity = weather_control([](auto v) { return be(v); });
    severity[7] = static_cast<std::uint8_t>((severity[7] & 0xE3) | (6 << 2));
    EXPECT_FALSE(decode_weather_control(severity, needs_swap_from_big_endian()).has_value());
    // IG Mode 3 is only reported by the IG (p. 214): refused from a host...
    Bytes host = kIgControlBigEndian;
    host[4] = static_cast<std::uint8_t>(host[4] | 0x03);
    EXPECT_FALSE(decode_ig_control(host, needs_swap_from_big_endian()).has_value());
    // ...and accepted from an IG.
    Bytes ig = kStartOfFrameBigEndian;
    ig[5] = static_cast<std::uint8_t>(ig[5] | 0x03);
    const auto sof = decode_start_of_frame(ig, needs_swap_from_big_endian());
    ASSERT_TRUE(sof.has_value());
    EXPECT_EQ(sof->ig_mode, IgMode::offline_maintenance);
}

TEST(Cigi, EncodeNeedsRoomForThePacket) {
    std::array<std::uint8_t, 47> small{};
    EXPECT_EQ(encode(IgControl{}, std::span(small).first(23)), 0U);
    EXPECT_EQ(encode(EntityControl{}, small), 0U);
    EXPECT_EQ(encode(StartOfFrame{}, std::span(small).first(23)), 0U);
    EXPECT_EQ(encode(CelestialSphereControl{}, std::span(small).first(15)), 0U);
    EXPECT_EQ(encode(AtmosphereControl{}, std::span(small).first(31)), 0U);
    EXPECT_EQ(encode(WeatherControl{}, small), 0U);
}

TEST(Cigi, MessageChecks) {
    EXPECT_EQ(MessageReader(Bytes{}, kIgControlId).error(), MessageError::too_short);
    EXPECT_EQ(MessageReader(Bytes(kIgControlBigEndian.begin(), kIgControlBigEndian.begin() + 7),
                            kIgControlId)
                  .error(),
              MessageError::too_short);
    // An IG-to-host message where a host-to-IG one is expected.
    EXPECT_EQ(MessageReader(kStartOfFrameBigEndian, kIgControlId).error(),
              MessageError::wrong_first_packet);
    Bytes v2 = kIgControlBigEndian;
    v2[2] = 2;
    EXPECT_EQ(MessageReader(v2, kIgControlId).error(), MessageError::wrong_major_version);
    Bytes magic = kIgControlBigEndian;
    magic[6] = 0x12;
    EXPECT_EQ(MessageReader(magic, kIgControlId).error(), MessageError::bad_magic);
    // Any minor version is accepted.
    Bytes minor = kIgControlBigEndian;
    minor[4] = static_cast<std::uint8_t>((minor[4] & 0x0F) | 0x30);
    EXPECT_EQ(MessageReader(minor, kIgControlId).error(), MessageError::none);
    const auto decoded = decode_ig_control(minor, needs_swap_from_big_endian());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->minor_version, 3);
    // The first packet's own size byte must cover it.
    Bytes small = kIgControlBigEndian;
    small[1] = 8;
    EXPECT_EQ(MessageReader(small, kIgControlId).error(), MessageError::bad_packet_size);
}

TEST(Cigi, UnknownPacketsAreSkippedBySize) {
    // ICD 2.11 rule 3: a user-defined packet (opcodes 201-255, ICD 2.10)
    // between two known ones.
    const Bytes user = {0xC9, 0x10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    const Bytes message =
        cat({kIgControlBigEndian, user, entity_control([](auto v) { return be(v); })});
    MessageReader reader(message, kIgControlId);
    std::vector<std::uint8_t> ids;
    MessageReader::Packet p;
    while (reader.next(p)) {
        ids.push_back(p.id);
    }
    EXPECT_EQ(reader.error(), MessageError::none);
    EXPECT_EQ(ids, (std::vector<std::uint8_t>{1, 0xC9, 2}));
    EXPECT_FALSE(decode_entity_control(user, true).has_value());  // wrong ID
}

TEST(Cigi, LongerPacketsAreReadShorterOnesRejected) {
    // ICD 2.11 rule 4: bytes past the expected size are ignored.
    Bytes longer = entity_control([](auto v) { return be(v); });
    longer[1] = 56;
    longer.insert(longer.end(), 8, 0xAB);
    const Bytes message = cat({kIgControlBigEndian, longer});
    MessageReader reader(message, kIgControlId);
    MessageReader::Packet p;
    ASSERT_TRUE(reader.next(p));
    ASSERT_TRUE(reader.next(p));
    EXPECT_EQ(p.bytes.size(), 56U);
    const auto e = decode_entity_control(p.bytes, reader.swapped());
    ASSERT_TRUE(e.has_value());
    expect_entity_control(*e);

    Bytes shorter = entity_control([](auto v) { return be(v); });
    shorter[1] = 40;
    shorter.resize(40);
    EXPECT_FALSE(decode_entity_control(shorter, needs_swap_from_big_endian()).has_value());
}

TEST(Cigi, BadPacketSizesStopTheMessage) {
    for (const std::uint8_t size : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{200}}) {
        Bytes message = cat({kIgControlBigEndian, {0x02, size, 0, 0, 0, 0, 0, 0}});
        MessageReader reader(message, kIgControlId);
        MessageReader::Packet p;
        ASSERT_TRUE(reader.next(p));
        EXPECT_FALSE(reader.next(p)) << int{size};
        EXPECT_EQ(reader.error(), MessageError::bad_packet_size) << int{size};
        EXPECT_FALSE(reader.next(p));
    }
    // A lone trailing byte can't hold a packet header.
    Bytes odd = cat({kIgControlBigEndian, {0x02}});
    MessageReader reader(odd, kIgControlId);
    MessageReader::Packet p;
    ASSERT_TRUE(reader.next(p));
    EXPECT_FALSE(reader.next(p));
    EXPECT_EQ(reader.error(), MessageError::bad_packet_size);
}

TEST(Cigi, EveryTruncationAndSizeByteStaysInBounds) {
    // Every prefix of a message, and every value of every packet's size byte:
    // the reader and decoders never read past the bytes they're given (ASan
    // checks this in the sanitizer builds), and every packet they yield lies
    // inside the message.
    const Bytes full = cat({kIgControlBigEndian,
                            entity_control([](auto v) { return be(v); }),
                            {0xC9, 0x08, 0, 0, 0, 0, 0, 0}});
    const std::array<std::size_t, 3> size_bytes = {1, 25, 73};
    for (std::size_t s = 0; s <= size_bytes.size(); ++s) {
        for (int value = 0; value < (s < size_bytes.size() ? 256 : 1); ++value) {
            Bytes mutated = full;
            if (s < size_bytes.size()) {
                mutated[size_bytes[s]] = static_cast<std::uint8_t>(value);
            }
            for (std::size_t n = 0; n <= mutated.size(); ++n) {
                // A heap copy of exactly n bytes, so ASan sees any overread.
                const Bytes prefix(mutated.begin(), mutated.begin() + static_cast<long>(n));
                MessageReader reader(prefix, kIgControlId);
                MessageReader::Packet p;
                while (reader.next(p)) {
                    ASSERT_GE(p.bytes.data(), prefix.data());
                    ASSERT_LE(p.bytes.data() + p.bytes.size(), prefix.data() + prefix.size());
                    (void)decode_ig_control(p.bytes, reader.swapped());
                    (void)decode_entity_control(p.bytes, reader.swapped());
                    (void)decode_start_of_frame(p.bytes, reader.swapped());
                }
            }
        }
    }
}

TEST(Cigi, ReadingAndWritingAllocateNothing) {
    // The network thread decodes every message: no heap use per packet.
    const Bytes message = cat({kIgControlBigEndian, entity_control([](auto v) { return be(v); })});
    std::array<std::uint8_t, 48> out{};
    const test::AllocationCounter counter;
    for (int i = 0; i < 100; ++i) {
        MessageReader reader(message, kIgControlId);
        MessageReader::Packet p;
        while (reader.next(p)) {
            if (const auto e = decode_entity_control(p.bytes, reader.swapped())) {
                (void)encode(*e, out);
            }
            if (const auto ig = decode_ig_control(p.bytes, reader.swapped())) {
                (void)encode(*ig, out);
            }
        }
    }
    EXPECT_EQ(counter.count(), 0U);
}

// HAT/HOT (Integration 1 step 2). Figure 86 in a given byte order: ID
// 0x0102; entity coordinates, Extended (2); update period 3; entity 0x0405.
template <typename Order>
Bytes hat_hot_request(Order order) {
    return cat({{0x18, 0x20},  // Packet ID 24, Packet Size 32
                order(std::uint16_t{0x0102}),
                {0x06, 0x03},  // 00000 1 10, Update Period 3
                order(std::uint16_t{0x0405}),
                order(34.6372),
                order(-86.7751),
                order(190.5)});
}

void expect_hat_hot_request(const HatHotRequest& p) {
    EXPECT_EQ(p.hat_hot_id, 0x0102);
    EXPECT_TRUE(p.entity_coordinates);
    EXPECT_EQ(p.request_type, HatHotRequestType::extended);
    EXPECT_EQ(p.update_period, 3);
    EXPECT_EQ(p.entity_id, 0x0405);
    EXPECT_DOUBLE_EQ(p.lat_or_x, 34.6372);
    EXPECT_DOUBLE_EQ(p.lon_or_y, -86.7751);
    EXPECT_DOUBLE_EQ(p.alt_or_z, 190.5);
}

// Figure 110: ID 0x0102; Host Frame Number LSN 0xA, HOT, valid; 190.5 m.
template <typename Order>
Bytes hat_hot_response(Order order) {
    return cat({{0x66, 0x10},  // Packet ID 102, Packet Size 16
                order(std::uint16_t{0x0102}),
                {0xA3, 0x00, 0x00, 0x00},  // 1010 00 1 1, Reserved
                order(190.5)});
}

// Figure 112: ID 0x0102; LSN 0x5, valid; HAT 12.25, HOT 190.5, material
// 0x01020304, normal at azimuth -45 and elevation 80 degrees.
template <typename Order>
Bytes hat_hot_extended_response(Order order) {
    return cat({{0x67, 0x28},  // Packet ID 103, Packet Size 40
                order(std::uint16_t{0x0102}),
                {0x51, 0x00, 0x00, 0x00},  // 0101 000 1, Reserved
                order(12.25),
                order(190.5),
                order(std::uint32_t{0x01020304}),
                order(-45.0F),
                order(80.0F),
                {0x00, 0x00, 0x00, 0x00}});
}

TEST(Cigi, HatHotRequestInBothByteOrders) {
    for (const bool big : {true, false}) {
        SCOPED_TRACE(big ? "big-endian host" : "little-endian host");
        const auto order = [big](auto v) { return big ? be(v) : le(v); };
        const Bytes message =
            cat({big ? kIgControlBigEndian : kIgControlLittleEndian, hat_hot_request(order)});
        MessageReader reader(message, kIgControlId);
        MessageReader::Packet p;
        ASSERT_TRUE(reader.next(p));
        ASSERT_TRUE(reader.next(p));
        EXPECT_EQ(p.id, kHatHotRequestId);
        const auto r = decode_hat_hot_request(p.bytes, reader.swapped());
        ASSERT_TRUE(r.has_value());
        expect_hat_hot_request(*r);
        EXPECT_FALSE(reader.next(p));
        EXPECT_EQ(reader.error(), MessageError::none);
    }
}

// Each order's bytes, by the ICD's figures: network order is big-endian
// whatever this computer is.
TEST(Cigi, HatHotPacketsEncodeTheIcdBytesInEitherOrder) {
    const auto big = [](auto v) { return be(v); };
    const auto native = [](auto v) {
        return std::endian::native == std::endian::little ? le(v) : be(v);
    };
    HatHotRequest q;
    q.hat_hot_id = 0x0102;
    q.entity_coordinates = true;
    q.request_type = HatHotRequestType::extended;
    q.update_period = 3;
    q.entity_id = 0x0405;
    q.lat_or_x = 34.6372;
    q.lon_or_y = -86.7751;
    q.alt_or_z = 190.5;
    HatHotResponse r;
    r.hat_hot_id = 0x0102;
    r.host_frame_number_lsn = 0xA;
    r.hot = true;
    r.valid = true;
    r.height_m = 190.5;
    HatHotExtendedResponse x;
    x.hat_hot_id = 0x0102;
    x.host_frame_number_lsn = 0x5;
    x.valid = true;
    x.hat_m = 12.25;
    x.hot_m = 190.5;
    x.material_code = 0x01020304;
    x.normal_azimuth_deg = -45.0F;
    x.normal_elevation_deg = 80.0F;
    for (const ByteOrder o : {ByteOrder::network, ByteOrder::native}) {
        SCOPED_TRACE(o == ByteOrder::network ? "network" : "native");
        const bool swap = swaps(o);
        const auto order = [&](auto v) { return o == ByteOrder::network ? big(v) : native(v); };
        std::array<std::uint8_t, 48> out{};
        out.fill(0xEE);
        ASSERT_EQ(encode(q, out, swap), 32U);
        EXPECT_EQ(Bytes(out.begin(), out.begin() + 32), hat_hot_request(order));
        EXPECT_EQ(out[32], 0xEE);  // nothing past the packet
        ASSERT_EQ(encode(r, out, swap), 16U);
        EXPECT_EQ(Bytes(out.begin(), out.begin() + 16), hat_hot_response(order));
        ASSERT_EQ(encode(x, out, swap), 40U);
        EXPECT_EQ(Bytes(out.begin(), out.begin() + 40), hat_hot_extended_response(order));
    }
}

TEST(Cigi, HatHotResponsesFromEitherOrder) {
    for (const bool big : {true, false}) {
        SCOPED_TRACE(big ? "big-endian IG" : "little-endian IG");
        const auto order = [big](auto v) { return big ? be(v) : le(v); };
        const bool swap = big == needs_swap_from_big_endian();
        const auto r = decode_hat_hot_response(hat_hot_response(order), swap);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->hat_hot_id, 0x0102);
        EXPECT_EQ(r->host_frame_number_lsn, 0xA);
        EXPECT_TRUE(r->hot);
        EXPECT_TRUE(r->valid);
        EXPECT_DOUBLE_EQ(r->height_m, 190.5);
        const auto x = decode_hat_hot_extended_response(hat_hot_extended_response(order), swap);
        ASSERT_TRUE(x.has_value());
        EXPECT_EQ(x->hat_hot_id, 0x0102);
        EXPECT_EQ(x->host_frame_number_lsn, 0x5);
        EXPECT_TRUE(x->valid);
        EXPECT_DOUBLE_EQ(x->hat_m, 12.25);
        EXPECT_DOUBLE_EQ(x->hot_m, 190.5);
        EXPECT_EQ(x->material_code, 0x01020304U);
        EXPECT_FLOAT_EQ(x->normal_azimuth_deg, -45.0F);
        EXPECT_FLOAT_EQ(x->normal_elevation_deg, 80.0F);
    }
}

// What a host in network order gets: Start of Frame as Figure 109 lays it
// out, big-endian, its magic number 8000h read as unswapped there.
TEST(Cigi, StartOfFrameInNetworkOrder) {
    StartOfFrame sof;
    sof.database_number = -1;
    sof.ig_status = 7;
    sof.host_defined_earth_model = true;
    sof.ig_mode = IgMode::debug;
    sof.ig_frame_number = 256;
    sof.timestamp = 100000;
    sof.last_host_frame_number = 0xFFFFFFFE;
    std::array<std::uint8_t, 24> out{};
    ASSERT_EQ(encode(sof, out, swaps(ByteOrder::network)), 24U);
    EXPECT_EQ(Bytes(out.begin(), out.end()), kStartOfFrameBigEndian);
    EXPECT_EQ(MessageReader(out, kStartOfFrameId).swapped(), needs_swap_from_big_endian());
    EXPECT_FALSE(swaps(ByteOrder::native));
}

TEST(Cigi, HatHotReservedBitsAreSentAsZeroAndIgnored) {
    // Every field at its widest: anything else set would be a reserved bit.
    HatHotRequest q;
    q.entity_coordinates = true;
    q.request_type = static_cast<HatHotRequestType>(3);
    std::array<std::uint8_t, 48> d{};
    d.fill(0xFF);
    ASSERT_EQ(encode(q, d), 32U);
    EXPECT_EQ(d[4] & 0xF8, 0);  // byte 4 bits 7-3
    HatHotResponse r;
    r.host_frame_number_lsn = 0xF;
    r.hot = r.valid = true;
    d.fill(0xFF);
    ASSERT_EQ(encode(r, d), 16U);
    EXPECT_EQ(d[4] & 0x0C, 0);  // byte 4 bits 3-2
    EXPECT_EQ(d[5] | d[6] | d[7], 0);
    HatHotExtendedResponse x;
    x.host_frame_number_lsn = 0xF;
    x.valid = true;
    d.fill(0xFF);
    ASSERT_EQ(encode(x, d), 40U);
    EXPECT_EQ(d[4] & 0x0E, 0);  // byte 4 bits 3-1
    EXPECT_EQ(d[5] | d[6] | d[7] | d[36] | d[37] | d[38] | d[39], 0);
    // Set on receive, they're left alone (ICD 2.11, rule 4: a later version
    // may use them).
    Bytes request = hat_hot_request([](auto v) { return be(v); });
    request[4] = static_cast<std::uint8_t>(request[4] | 0xF8);
    const auto rq = decode_hat_hot_request(request, needs_swap_from_big_endian());
    ASSERT_TRUE(rq.has_value());
    expect_hat_hot_request(*rq);
    Bytes response = hat_hot_response([](auto v) { return be(v); });
    response[4] = static_cast<std::uint8_t>(response[4] | 0x0C);
    response[6] = 0xFF;
    const auto rs = decode_hat_hot_response(response, needs_swap_from_big_endian());
    ASSERT_TRUE(rs.has_value());
    EXPECT_EQ(rs->host_frame_number_lsn, 0xA);
    EXPECT_TRUE(rs->hot && rs->valid);
}

TEST(Cigi, HatHotRequestTypeThreeIsRejected) {
    // Request Type 3 is undefined (p. 153).
    Bytes request = hat_hot_request([](auto v) { return be(v); });
    request[4] = static_cast<std::uint8_t>(request[4] | 0x03);
    EXPECT_FALSE(decode_hat_hot_request(request, needs_swap_from_big_endian()).has_value());
    for (const unsigned type : {0U, 1U, 2U}) {
        request[4] = static_cast<std::uint8_t>((request[4] & 0xFCU) | type);
        const auto q = decode_hat_hot_request(request, needs_swap_from_big_endian());
        ASSERT_TRUE(q.has_value());
        EXPECT_EQ(static_cast<unsigned>(q->request_type), type);
    }
}

TEST(Cigi, HatHotPacketsNeedTheirSize) {
    std::array<std::uint8_t, 39> small{};
    EXPECT_EQ(encode(HatHotRequest{}, std::span(small).first(31)), 0U);
    EXPECT_EQ(encode(HatHotResponse{}, std::span(small).first(15)), 0U);
    EXPECT_EQ(encode(HatHotExtendedResponse{}, small), 0U);
    const Bytes request = hat_hot_request([](auto v) { return be(v); });
    EXPECT_FALSE(decode_hat_hot_request(std::span(request).first(31), false).has_value());
    Bytes short_size = request;
    short_size[1] = 31;
    EXPECT_FALSE(decode_hat_hot_request(short_size, false).has_value());
    const Bytes x = hat_hot_extended_response([](auto v) { return be(v); });
    EXPECT_FALSE(decode_hat_hot_extended_response(std::span(x).first(39), false).has_value());
    // Each decoder takes its own packet only.
    EXPECT_FALSE(decode_hat_hot_response(x, false).has_value());
}

}  // namespace
}  // namespace fovea::cigi
