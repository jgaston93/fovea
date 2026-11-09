#include "fovea/net/cigi.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <type_traits>

namespace fovea::cigi {

namespace {

// Multi-byte fields are copied byte for byte (no alignment assumed) and, when
// the sender's byte order differs, reversed.
template <typename T>
void put(std::span<std::uint8_t> out, std::size_t at, T value, bool swap) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    if (swap) {
        std::reverse(bytes.begin(), bytes.end());
    }
    std::memcpy(out.data() + at, bytes.data(), sizeof(T));
}

template <typename T>
T get(std::span<const std::uint8_t> in, std::size_t at, bool swap) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), in.data() + at, sizeof(T));
    if (swap) {
        std::reverse(bytes.begin(), bytes.end());
    }
    return std::bit_cast<T>(bytes);
}

std::uint8_t bits(std::uint8_t byte, unsigned shift, unsigned width) {
    return static_cast<std::uint8_t>((unsigned{byte} >> shift) & ((1U << width) - 1U));
}

std::uint8_t field(unsigned value, unsigned shift, unsigned width) {
    return static_cast<std::uint8_t>((value & ((1U << width) - 1U)) << shift);
}

unsigned flag(bool b) {
    return b ? 1U : 0U;
}

// The packet header check every decoder starts with: the right ID, and at
// least the ICD's size both in the size byte and in the bytes given.
bool has_packet(std::span<const std::uint8_t> in, std::uint8_t id, std::uint8_t size) {
    return in.size() >= size && in[0] == id && in[1] >= size;
}

// Reserved bits and bytes are sent as zero (ICD 2.11, rule 2).
void clear(std::span<std::uint8_t> out, std::size_t size) {
    std::fill_n(out.begin(), size, std::uint8_t{0});
}

}  // namespace

// ICD 4.1.1, Figure 39 (p. 42):
//   0 Packet ID = 1 | 1 Packet Size = 24 | 2 Major Version = 3 | 3 Database Number
//   4 Minor Version (7-4), Extrapolation/Interpolation Enable (3),
//     Timestamp Valid (2), IG Mode (1-0) | 5 Reserved | 6-7 Byte Swap Magic Number
//   8-11 Host Frame Number | 12-15 Timestamp | 16-19 Last IG Frame Number
//   20-23 Reserved
std::size_t encode(const IgControl& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kIgControlSize) {
        return 0;
    }
    clear(out, kIgControlSize);
    out[0] = kIgControlId;
    out[1] = kIgControlSize;
    out[2] = kMajorVersion;
    put(out, 3, p.database_number, swap);
    out[4] = static_cast<std::uint8_t>(
        field(p.minor_version, 4, 4) | field(flag(p.extrapolation_enable), 3, 1) |
        field(flag(p.timestamp_valid), 2, 1) | field(static_cast<unsigned>(p.ig_mode), 0, 2));
    put(out, 6, kMagicNoSwap, swap);
    put(out, 8, p.host_frame_number, swap);
    put(out, 12, p.timestamp, swap);
    put(out, 16, p.last_ig_frame_number, swap);
    return kIgControlSize;
}

std::optional<IgControl> decode_ig_control(std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kIgControlId, kIgControlSize) || bits(in[4], 0, 2) == 3) {
        return std::nullopt;  // IG Mode 3 (Offline Maintenance) is the IG's alone
    }
    IgControl p;
    p.database_number = get<std::int8_t>(in, 3, swap);
    p.minor_version = bits(in[4], 4, 4);
    p.extrapolation_enable = bits(in[4], 3, 1) != 0;
    p.timestamp_valid = bits(in[4], 2, 1) != 0;
    p.ig_mode = static_cast<IgMode>(bits(in[4], 0, 2));
    p.host_frame_number = get<std::uint32_t>(in, 8, swap);
    p.timestamp = get<std::uint32_t>(in, 12, swap);
    p.last_ig_frame_number = get<std::uint32_t>(in, 16, swap);
    return p;
}

// ICD 4.1.2, Figure 42 (p. 48):
//   0 Packet ID = 2 | 1 Packet Size = 48 | 2-3 Entity ID
//   4 Reserved (7), Ground/Ocean Clamp (6-5), Inherit Alpha (4),
//     Collision Detection Enable (3), Attach State (2), Entity State (1-0)
//   5 Reserved (7-5), Linear Extrapolation/Interpolation Enable (4),
//     Animation State (3-2), Animation Loop Mode (1), Animation Direction (0)
//   6 Alpha | 7 Reserved | 8-9 Entity Type | 10-11 Parent ID
//   12-15 Roll | 16-19 Pitch | 20-23 Yaw (single floats)
//   24-31 Latitude/X Offset | 32-39 Longitude/Y Offset | 40-47 Altitude/Z Offset
std::size_t encode(const EntityControl& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kEntityControlSize) {
        return 0;
    }
    clear(out, kEntityControlSize);
    out[0] = kEntityControlId;
    out[1] = kEntityControlSize;
    put(out, 2, p.entity_id, swap);
    out[4] = static_cast<std::uint8_t>(
        field(static_cast<unsigned>(p.ground_ocean_clamp), 5, 2) |
        field(flag(p.inherit_alpha), 4, 1) | field(flag(p.collision_detection), 3, 1) |
        field(flag(p.attach), 2, 1) | field(static_cast<unsigned>(p.entity_state), 0, 2));
    out[5] = static_cast<std::uint8_t>(field(flag(p.extrapolation_enable), 4, 1) |
                                       field(static_cast<unsigned>(p.animation_state), 2, 2) |
                                       field(flag(p.animation_loop), 1, 1) |
                                       field(flag(p.animation_backward), 0, 1));
    out[6] = p.alpha;
    put(out, 8, p.entity_type, swap);
    put(out, 10, p.parent_id, swap);
    put(out, 12, p.roll_deg, swap);
    put(out, 16, p.pitch_deg, swap);
    put(out, 20, p.yaw_deg, swap);
    put(out, 24, p.lat_or_x, swap);
    put(out, 32, p.lon_or_y, swap);
    put(out, 40, p.alt_or_z, swap);
    return kEntityControlSize;
}

std::optional<EntityControl> decode_entity_control(std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kEntityControlId, kEntityControlSize) || bits(in[4], 0, 2) == 3 ||
        bits(in[4], 5, 2) == 3) {
        return std::nullopt;  // Entity State or Ground/Ocean Clamp 3: undefined
    }
    EntityControl p;
    p.entity_id = get<std::uint16_t>(in, 2, swap);
    p.ground_ocean_clamp = static_cast<GroundClamp>(bits(in[4], 5, 2));
    p.inherit_alpha = bits(in[4], 4, 1) != 0;
    p.collision_detection = bits(in[4], 3, 1) != 0;
    p.attach = bits(in[4], 2, 1) != 0;
    p.entity_state = static_cast<EntityState>(bits(in[4], 0, 2));
    p.extrapolation_enable = bits(in[5], 4, 1) != 0;
    p.animation_state = static_cast<AnimationState>(bits(in[5], 2, 2));
    p.animation_loop = bits(in[5], 1, 1) != 0;
    p.animation_backward = bits(in[5], 0, 1) != 0;
    p.alpha = in[6];
    p.entity_type = get<std::uint16_t>(in, 8, swap);
    p.parent_id = get<std::uint16_t>(in, 10, swap);
    p.roll_deg = get<float>(in, 12, swap);
    p.pitch_deg = get<float>(in, 16, swap);
    p.yaw_deg = get<float>(in, 20, swap);
    p.lat_or_x = get<double>(in, 24, swap);
    p.lon_or_y = get<double>(in, 32, swap);
    p.alt_or_z = get<double>(in, 40, swap);
    return p;
}

// ICD 4.1.9, Figure 53 (p. 82):
//   0 Packet ID = 9 | 1 Packet Size = 16 | 2 Hour | 3 Minute
//   4 Reserved (7-5), Date/Time Valid (4), Star Field Enable (3),
//     Moon Enable (2), Sun Enable (1), Ephemeris Model Enable (0)
//   5-7 Reserved | 8-11 Date | 12-15 Star Field Intensity
std::size_t encode(const CelestialSphereControl& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kCelestialSphereControlSize) {
        return 0;
    }
    clear(out, kCelestialSphereControlSize);
    out[0] = kCelestialSphereControlId;
    out[1] = kCelestialSphereControlSize;
    out[2] = p.hour;
    out[3] = p.minute;
    out[4] = static_cast<std::uint8_t>(
        field(flag(p.date_time_valid), 4, 1) | field(flag(p.star_field_enable), 3, 1) |
        field(flag(p.moon_enable), 2, 1) | field(flag(p.sun_enable), 1, 1) |
        field(flag(p.ephemeris_enable), 0, 1));
    put(out, 8, p.date, swap);
    put(out, 12, p.star_field_intensity, swap);
    return kCelestialSphereControlSize;
}

std::optional<CelestialSphereControl> decode_celestial_sphere_control(
    std::span<const std::uint8_t> in, bool swap) {
    // Hour and Minute out of range (p. 83) are the adapter's to refuse:
    // without Date/Time Valid they mean nothing.
    if (!has_packet(in, kCelestialSphereControlId, kCelestialSphereControlSize)) {
        return std::nullopt;
    }
    CelestialSphereControl p;
    p.hour = in[2];
    p.minute = in[3];
    p.date_time_valid = bits(in[4], 4, 1) != 0;
    p.star_field_enable = bits(in[4], 3, 1) != 0;
    p.moon_enable = bits(in[4], 2, 1) != 0;
    p.sun_enable = bits(in[4], 1, 1) != 0;
    p.ephemeris_enable = bits(in[4], 0, 1) != 0;
    p.date = get<std::uint32_t>(in, 8, swap);
    p.star_field_intensity = get<float>(in, 12, swap);
    return p;
}

// ICD 4.1.10, Figure 54 (p. 85):
//   0 Packet ID = 10 | 1 Packet Size = 32
//   2 Reserved (7-1), Atmospheric Model Enable (0) | 3 Global Humidity
//   4-7 Global Air Temperature | 8-11 Global Visibility Range
//   12-15 Global Horizontal Wind Speed | 16-19 Global Vertical Wind Speed
//   20-23 Global Wind Direction | 24-27 Global Barometric Pressure
//   28-31 Reserved
std::size_t encode(const AtmosphereControl& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kAtmosphereControlSize) {
        return 0;
    }
    clear(out, kAtmosphereControlSize);
    out[0] = kAtmosphereControlId;
    out[1] = kAtmosphereControlSize;
    out[2] = field(flag(p.atmospheric_model_enable), 0, 1);
    out[3] = p.humidity;
    put(out, 4, p.air_temperature_c, swap);
    put(out, 8, p.visibility_range_m, swap);
    put(out, 12, p.horizontal_wind_mps, swap);
    put(out, 16, p.vertical_wind_mps, swap);
    put(out, 20, p.wind_direction_deg, swap);
    put(out, 24, p.barometric_pressure_mb, swap);
    return kAtmosphereControlSize;
}

std::optional<AtmosphereControl> decode_atmosphere_control(std::span<const std::uint8_t> in,
                                                           bool swap) {
    if (!has_packet(in, kAtmosphereControlId, kAtmosphereControlSize)) {
        return std::nullopt;
    }
    AtmosphereControl p;
    p.atmospheric_model_enable = bits(in[2], 0, 1) != 0;
    p.humidity = in[3];
    p.air_temperature_c = get<float>(in, 4, swap);
    p.visibility_range_m = get<float>(in, 8, swap);
    p.horizontal_wind_mps = get<float>(in, 12, swap);
    p.vertical_wind_mps = get<float>(in, 16, swap);
    p.wind_direction_deg = get<float>(in, 20, swap);
    p.barometric_pressure_mb = get<float>(in, 24, swap);
    return p;
}

// ICD 4.1.12, Figure 65 (p. 101):
//   0 Packet ID = 12 | 1 Packet Size = 56 | 2-3 Entity ID/Region ID
//   4 Layer ID | 5 Humidity
//   6 Cloud Type (7-4), Random Lightning Enable (3), Random Winds Enable (2),
//     Scud Enable (1), Weather Enable (0)
//   7 Reserved (7-5), Severity (4-2), Scope (1-0)
//   8-11 Air Temperature | 12-15 Visibility Range | 16-19 Scud Frequency
//   20-23 Coverage | 24-27 Base Elevation | 28-31 Thickness
//   32-35 Transition Band | 36-39 Horizontal Wind Speed
//   40-43 Vertical Wind Speed | 44-47 Wind Direction
//   48-51 Barometric Pressure | 52-55 Aerosol Concentration
std::size_t encode(const WeatherControl& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kWeatherControlSize) {
        return 0;
    }
    clear(out, kWeatherControlSize);
    out[0] = kWeatherControlId;
    out[1] = kWeatherControlSize;
    put(out, 2, p.entity_or_region_id, swap);
    out[4] = p.layer_id;
    out[5] = p.humidity;
    out[6] = static_cast<std::uint8_t>(
        field(p.cloud_type, 4, 4) | field(flag(p.random_lightning_enable), 3, 1) |
        field(flag(p.random_winds_enable), 2, 1) | field(flag(p.scud_enable), 1, 1) |
        field(flag(p.weather_enable), 0, 1));
    out[7] = static_cast<std::uint8_t>(field(p.severity, 2, 3) |
                                       field(static_cast<unsigned>(p.scope), 0, 2));
    put(out, 8, p.air_temperature_c, swap);
    put(out, 12, p.visibility_range_m, swap);
    put(out, 16, p.scud_frequency, swap);
    put(out, 20, p.coverage, swap);
    put(out, 24, p.base_elevation_m, swap);
    put(out, 28, p.thickness_m, swap);
    put(out, 32, p.transition_band_m, swap);
    put(out, 36, p.horizontal_wind_mps, swap);
    put(out, 40, p.vertical_wind_mps, swap);
    put(out, 44, p.wind_direction_deg, swap);
    put(out, 48, p.barometric_pressure_mb, swap);
    put(out, 52, p.aerosol_concentration, swap);
    return kWeatherControlSize;
}

std::optional<WeatherControl> decode_weather_control(std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kWeatherControlId, kWeatherControlSize) || bits(in[7], 0, 2) == 3 ||
        bits(in[7], 2, 3) > 5) {
        return std::nullopt;  // Scope 3 undefined; Severity 0-5 (p. 104)
    }
    WeatherControl p;
    p.entity_or_region_id = get<std::uint16_t>(in, 2, swap);
    p.layer_id = in[4];
    p.humidity = in[5];
    p.cloud_type = bits(in[6], 4, 4);
    p.random_lightning_enable = bits(in[6], 3, 1) != 0;
    p.random_winds_enable = bits(in[6], 2, 1) != 0;
    p.scud_enable = bits(in[6], 1, 1) != 0;
    p.weather_enable = bits(in[6], 0, 1) != 0;
    p.severity = bits(in[7], 2, 3);
    p.scope = static_cast<WeatherScope>(bits(in[7], 0, 2));
    p.air_temperature_c = get<float>(in, 8, swap);
    p.visibility_range_m = get<float>(in, 12, swap);
    p.scud_frequency = get<float>(in, 16, swap);
    p.coverage = get<float>(in, 20, swap);
    p.base_elevation_m = get<float>(in, 24, swap);
    p.thickness_m = get<float>(in, 28, swap);
    p.transition_band_m = get<float>(in, 32, swap);
    p.horizontal_wind_mps = get<float>(in, 36, swap);
    p.vertical_wind_mps = get<float>(in, 40, swap);
    p.wind_direction_deg = get<float>(in, 44, swap);
    p.barometric_pressure_mb = get<float>(in, 48, swap);
    p.aerosol_concentration = get<float>(in, 52, swap);
    return p;
}

// ICD 4.1.24, Figure 86 (p. 152); the errata leave it as it is:
//   0 Packet ID = 24 | 1 Packet Size = 32 | 2-3 HAT/HOT ID
//   4 Reserved (7-3), Coordinate System (2), Request Type (1-0)
//   5 Update Period | 6-7 Entity ID
//   8-15 Latitude/X Offset | 16-23 Longitude/Y Offset | 24-31 Altitude/Z Offset
std::size_t encode(const HatHotRequest& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kHatHotRequestSize) {
        return 0;
    }
    clear(out, kHatHotRequestSize);
    out[0] = kHatHotRequestId;
    out[1] = kHatHotRequestSize;
    put(out, 2, p.hat_hot_id, swap);
    out[4] = static_cast<std::uint8_t>(field(flag(p.entity_coordinates), 2, 1) |
                                       field(static_cast<unsigned>(p.request_type), 0, 2));
    out[5] = p.update_period;
    put(out, 6, p.entity_id, swap);
    put(out, 8, p.lat_or_x, swap);
    put(out, 16, p.lon_or_y, swap);
    put(out, 24, p.alt_or_z, swap);
    return kHatHotRequestSize;
}

std::optional<HatHotRequest> decode_hat_hot_request(std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kHatHotRequestId, kHatHotRequestSize) || bits(in[4], 0, 2) == 3) {
        return std::nullopt;  // Request Type 3: undefined
    }
    HatHotRequest p;
    p.hat_hot_id = get<std::uint16_t>(in, 2, swap);
    p.entity_coordinates = bits(in[4], 2, 1) != 0;
    p.request_type = static_cast<HatHotRequestType>(bits(in[4], 0, 2));
    p.update_period = in[5];
    p.entity_id = get<std::uint16_t>(in, 6, swap);
    p.lat_or_x = get<double>(in, 8, swap);
    p.lon_or_y = get<double>(in, 16, swap);
    p.alt_or_z = get<double>(in, 24, swap);
    return p;
}

// ICD 4.2.1, Figure 109 (p. 212):
//   0 Packet ID = 101 | 1 Packet Size = 24 | 2 Major Version = 3 | 3 Database Number
//   4 IG Status Code | 5 Minor Version (7-4), Earth Reference Model (3),
//     Timestamp Valid (2), IG Mode (1-0) | 6-7 Byte Swap Magic Number
//   8-11 IG Frame Number | 12-15 Timestamp | 16-19 Last Host Frame Number
//   20-23 Reserved
std::size_t encode(const StartOfFrame& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kStartOfFrameSize) {
        return 0;
    }
    clear(out, kStartOfFrameSize);
    out[0] = kStartOfFrameId;
    out[1] = kStartOfFrameSize;
    out[2] = kMajorVersion;
    put(out, 3, p.database_number, swap);
    out[4] = p.ig_status;
    out[5] = static_cast<std::uint8_t>(
        field(p.minor_version, 4, 4) | field(flag(p.host_defined_earth_model), 3, 1) |
        field(flag(p.timestamp_valid), 2, 1) | field(static_cast<unsigned>(p.ig_mode), 0, 2));
    put(out, 6, kMagicNoSwap, swap);
    put(out, 8, p.ig_frame_number, swap);
    put(out, 12, p.timestamp, swap);
    put(out, 16, p.last_host_frame_number, swap);
    return kStartOfFrameSize;
}

std::optional<StartOfFrame> decode_start_of_frame(std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kStartOfFrameId, kStartOfFrameSize)) {
        return std::nullopt;
    }
    StartOfFrame p;
    p.database_number = get<std::int8_t>(in, 3, swap);
    p.ig_status = in[4];
    p.minor_version = bits(in[5], 4, 4);
    p.host_defined_earth_model = bits(in[5], 3, 1) != 0;
    p.timestamp_valid = bits(in[5], 2, 1) != 0;
    p.ig_mode = static_cast<IgMode>(bits(in[5], 0, 2));
    p.ig_frame_number = get<std::uint32_t>(in, 8, swap);
    p.timestamp = get<std::uint32_t>(in, 12, swap);
    p.last_host_frame_number = get<std::uint32_t>(in, 16, swap);
    return p;
}

// ICD 4.2.2, Figure 110 (p. 217); the errata leave it as it is:
//   0 Packet ID = 102 | 1 Packet Size = 16 | 2-3 HAT/HOT ID
//   4 Host Frame Number LSN (7-4), Reserved (3-2), Response Type (1),
//     Valid (0) | 5-7 Reserved | 8-15 Height
std::size_t encode(const HatHotResponse& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kHatHotResponseSize) {
        return 0;
    }
    clear(out, kHatHotResponseSize);
    out[0] = kHatHotResponseId;
    out[1] = kHatHotResponseSize;
    put(out, 2, p.hat_hot_id, swap);
    out[4] = static_cast<std::uint8_t>(field(p.host_frame_number_lsn, 4, 4) |
                                       field(flag(p.hot), 1, 1) | field(flag(p.valid), 0, 1));
    put(out, 8, p.height_m, swap);
    return kHatHotResponseSize;
}

std::optional<HatHotResponse> decode_hat_hot_response(std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kHatHotResponseId, kHatHotResponseSize)) {
        return std::nullopt;
    }
    HatHotResponse p;
    p.hat_hot_id = get<std::uint16_t>(in, 2, swap);
    p.host_frame_number_lsn = bits(in[4], 4, 4);
    p.hot = bits(in[4], 1, 1) != 0;
    p.valid = bits(in[4], 0, 1) != 0;
    p.height_m = get<double>(in, 8, swap);
    return p;
}

// ICD 4.2.3, Figure 112 (p. 220); the errata leave it as it is:
//   0 Packet ID = 103 | 1 Packet Size = 40 | 2-3 HAT/HOT ID
//   4 Host Frame Number LSN (7-4), Reserved (3-1), Valid (0) | 5-7 Reserved
//   8-15 HAT | 16-23 HOT | 24-27 Material Code
//   28-31 Normal Vector Azimuth | 32-35 Normal Vector Elevation | 36-39 Reserved
std::size_t encode(const HatHotExtendedResponse& p, std::span<std::uint8_t> out, bool swap) {
    if (out.size() < kHatHotExtendedResponseSize) {
        return 0;
    }
    clear(out, kHatHotExtendedResponseSize);
    out[0] = kHatHotExtendedResponseId;
    out[1] = kHatHotExtendedResponseSize;
    put(out, 2, p.hat_hot_id, swap);
    out[4] = static_cast<std::uint8_t>(field(p.host_frame_number_lsn, 4, 4) |
                                       field(flag(p.valid), 0, 1));
    put(out, 8, p.hat_m, swap);
    put(out, 16, p.hot_m, swap);
    put(out, 24, p.material_code, swap);
    put(out, 28, p.normal_azimuth_deg, swap);
    put(out, 32, p.normal_elevation_deg, swap);
    return kHatHotExtendedResponseSize;
}

std::optional<HatHotExtendedResponse> decode_hat_hot_extended_response(
    std::span<const std::uint8_t> in, bool swap) {
    if (!has_packet(in, kHatHotExtendedResponseId, kHatHotExtendedResponseSize)) {
        return std::nullopt;
    }
    HatHotExtendedResponse p;
    p.hat_hot_id = get<std::uint16_t>(in, 2, swap);
    p.host_frame_number_lsn = bits(in[4], 4, 4);
    p.valid = bits(in[4], 0, 1) != 0;
    p.hat_m = get<double>(in, 8, swap);
    p.hot_m = get<double>(in, 16, swap);
    p.material_code = get<std::uint32_t>(in, 24, swap);
    p.normal_azimuth_deg = get<float>(in, 28, swap);
    p.normal_elevation_deg = get<float>(in, 32, swap);
    return p;
}

MessageReader::MessageReader(std::span<const std::uint8_t> message, std::uint8_t first_packet_id)
    : message_(message) {
    // IG Control and Start of Frame both hold Major Version at byte 2 and the
    // magic number at bytes 6-7 (Figures 39 and 109).
    if (message.size() < 8) {
        error_ = MessageError::too_short;
    } else if (message[0] != first_packet_id) {
        error_ = MessageError::wrong_first_packet;
    } else if (message[2] != kMajorVersion) {
        error_ = MessageError::wrong_major_version;
    } else if (message[1] < kIgControlSize) {
        // Both first packets are 24 bytes: the checks above and below must
        // not read past it.
        static_assert(kIgControlSize == kStartOfFrameSize);
        error_ = MessageError::bad_packet_size;
    } else {
        const auto magic = get<std::uint16_t>(message, 6, false);
        if (magic == kMagicNoSwap) {
            swap_ = false;
        } else if (magic == kMagicSwap) {
            swap_ = true;
        } else {
            error_ = MessageError::bad_magic;
        }
    }
}

bool MessageReader::next(Packet& packet) {
    if (error_ != MessageError::none || offset_ >= message_.size()) {
        return false;
    }
    const std::size_t left = message_.size() - offset_;
    // A size under 2 can't hold its own header (and would never advance).
    const std::size_t size = left >= 2 ? std::size_t{message_[offset_ + 1]} : std::size_t{0};
    if (size < 2 || size > left) {
        error_ = MessageError::bad_packet_size;
        return false;
    }
    packet.id = message_[offset_];
    packet.bytes = message_.subspan(offset_, size);
    offset_ += size;
    return true;
}

}  // namespace fovea::cigi
