#pragma once

// CIGI 3.3 packets (D-013), encoded and decoded without allocation (D-014).
//
// Written from the CIGI 3.3 ICD (Boeing TST08I016, 3 Nov 2008) and its
// errata (Appendix C, 4 May 2009), which tools/fetch-cigi-icd.sh puts in
// docs/reference/. Section and page references below are the ICD's printed
// page numbers.
//
// Byte order (ICD 2.5, p. 7-8): the sender writes in its native order and
// sets the IG Control or Start of Frame Byte Swap Magic Number to 8000h; the
// receiver swaps every multi-byte field when it reads 80h instead. Encoding
// here writes native order.
//
// Message structure (ICD 2.6, p. 8): a message is one or more packets, each
// starting with an opcode byte and a size byte. A host-to-IG message starts
// with IG Control, an IG-to-host message with Start of Frame.
//
// Compatibility (ICD 2.11, p. 14): bit fields fill from the least significant
// bit, reserved bits are sent as zero, packets with unknown IDs are ignored,
// and bytes beyond a known packet's expected size are ignored.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fovea::cigi {

inline constexpr std::uint8_t kMajorVersion = 3;
// The 3.3 ICD gives Minor Version = 2 for both IG Control (p. 44) and Start
// of Frame (p. 215), and the errata don't change it (D-014). Sent as written;
// any minor version is accepted on receive.
inline constexpr std::uint8_t kMinorVersionSent = 2;
inline constexpr std::uint16_t kMagicNoSwap = 0x8000;  // ICD 2.5, p. 8
inline constexpr std::uint16_t kMagicSwap = 0x0080;

inline constexpr std::uint8_t kIgControlId = 1;  // ICD 4.1.1, p. 42
inline constexpr std::uint8_t kIgControlSize = 24;
inline constexpr std::uint8_t kEntityControlId = 2;  // ICD 4.1.2, p. 48
inline constexpr std::uint8_t kEntityControlSize = 48;
inline constexpr std::uint8_t kCelestialSphereControlId = 9;  // ICD 4.1.9, p. 82
inline constexpr std::uint8_t kCelestialSphereControlSize = 16;
inline constexpr std::uint8_t kAtmosphereControlId = 10;  // ICD 4.1.10, p. 85
inline constexpr std::uint8_t kAtmosphereControlSize = 32;
inline constexpr std::uint8_t kEnvironmentalRegionControlId = 11;  // ICD 4.1.11, p. 88
inline constexpr std::uint8_t kWeatherControlId = 12;              // ICD 4.1.12, p. 100
inline constexpr std::uint8_t kWeatherControlSize = 56;
inline constexpr std::uint8_t kStartOfFrameId = 101;  // ICD 4.2.1, p. 212
inline constexpr std::uint8_t kStartOfFrameSize = 24;

// IG Mode (IG Control, p. 43; Start of Frame, p. 214). Offline Maintenance is
// only reported by the IG.
enum class IgMode : std::uint8_t {
    reset_standby = 0,
    operate = 1,
    debug = 2,
    offline_maintenance = 3,
};

// ICD 4.1.1 IG Control (p. 41-44): host to IG, first packet of each message.
struct IgControl {
    std::uint8_t minor_version = kMinorVersionSent;  // 4 bits; decoded as received
    std::int8_t database_number = 0;                 // 0: no load requested
    IgMode ig_mode = IgMode::reset_standby;
    bool timestamp_valid = false;  // required (true) in asynchronous mode
    bool extrapolation_enable = false;
    std::uint32_t host_frame_number = 0;
    std::uint32_t timestamp = 0;  // 10 us ticks from an arbitrary reference
    std::uint32_t last_ig_frame_number = 0;
};

// Entity State (p. 49), Ground/Ocean Clamp (p. 51) and Animation State
// (p. 52). The 2-bit value 3 is undefined for the first two: a packet holding
// it is rejected.
enum class EntityState : std::uint8_t { inactive_standby = 0, active = 1, destroyed = 2 };
enum class GroundClamp : std::uint8_t { none = 0, non_conformal = 1, conformal = 2 };
enum class AnimationState : std::uint8_t { stop = 0, pause = 1, play = 2, continue_ = 3 };

// ICD 4.1.2 Entity Control (p. 45-55): an entity's state. Entity 0 is the
// host's main entity (CIGI's ownship). Top-level entities give geodetic
// latitude, longitude (degrees) and altitude (metres); attached (child)
// entities give X, Y, Z offsets in metres in the parent's body axes, in the
// same three fields.
struct EntityControl {
    std::uint16_t entity_id = 0;
    EntityState entity_state = EntityState::inactive_standby;
    bool attach = false;  // false: top level, position is geodetic
    bool collision_detection = false;
    bool inherit_alpha = false;
    GroundClamp ground_ocean_clamp = GroundClamp::none;
    bool animation_backward = false;  // Animation Direction
    bool animation_loop = false;      // Animation Loop Mode: continuous
    AnimationState animation_state = AnimationState::stop;
    bool extrapolation_enable = false;
    std::uint8_t alpha = 0;
    std::uint16_t entity_type = 0;  // 0: no geometry (a viewpoint, a camera)
    std::uint16_t parent_id = 0;
    float roll_deg = 0.0F;   // -180 to 180
    float pitch_deg = 0.0F;  // -90 to 90
    float yaw_deg = 0.0F;    // 0 to 360, from true north
    double lat_or_x = 0.0;
    double lon_or_y = 0.0;
    double alt_or_z = 0.0;
};

// ICD 4.1.9 Celestial Sphere Control (p. 82-84): the date and time of day
// and the sky's bodies.
struct CelestialSphereControl {
    std::uint8_t hour = 0;         // 0 to 23, UTC
    std::uint8_t minute = 0;       // 0 to 59, UTC
    bool ephemeris_enable = true;  // the IG runs the time of day on
    bool sun_enable = true;
    bool moon_enable = true;
    bool star_field_enable = true;
    bool date_time_valid = false;         // Hour, Minute and Date set the IG's date and time
    std::uint32_t date = 0;               // MMDDYYYY: month x 1000000 + day x 10000 + year
    float star_field_intensity = 100.0F;  // percent
};

// ICD 4.1.10 Atmosphere Control (p. 85-87): the global atmosphere.
struct AtmosphereControl {
    bool atmospheric_model_enable = false;  // for sensors' apparent radiances
    std::uint8_t humidity = 0;              // percent
    float air_temperature_c = 0.0F;
    float visibility_range_m = 0.0F;  // >= 0
    float horizontal_wind_mps = 0.0F;
    float vertical_wind_mps = 0.0F;   // positive up
    float wind_direction_deg = 0.0F;  // from which it blows, from true north
    float barometric_pressure_mb = 0.0F;
};

// Weather Control's Scope (p. 104). 3 is undefined: a packet holding it is
// rejected.
enum class WeatherScope : std::uint8_t { global = 0, regional = 1, entity = 2 };

// Weather Control's standard Layer IDs (p. 102); 10 to 255 are the IG's.
enum class WeatherLayerId : std::uint8_t {
    ground_fog = 0,
    cloud_1 = 1,
    cloud_2 = 2,
    cloud_3 = 3,
    rain = 4,
    snow = 5,
    sleet = 6,
    hail = 7,
    sand = 8,
    dust = 9,
};

// ICD 4.1.12 Weather Control (p. 100-107): a global or regional weather
// layer, or a weather entity.
struct WeatherControl {
    std::uint16_t entity_or_region_id = 0;  // ignored for global layers
    std::uint8_t layer_id = 0;              // WeatherLayerId, or the IG's own
    std::uint8_t humidity = 0;              // percent
    bool weather_enable = false;
    bool scud_enable = false;
    bool random_winds_enable = false;
    bool random_lightning_enable = false;
    std::uint8_t cloud_type = 0;  // 4 bits: 0 none, 1 to 10 the ICD's types
    WeatherScope scope = WeatherScope::global;
    std::uint8_t severity = 0;  // 3 bits, 0 to 5; 6 and 7 are undefined: rejected
    float air_temperature_c = 0.0F;
    float visibility_range_m = 0.0F;
    float scud_frequency = 0.0F;    // percent
    float coverage = 0.0F;          // percent
    float base_elevation_m = 0.0F;  // above mean sea level
    float thickness_m = 0.0F;
    float transition_band_m = 0.0F;
    float horizontal_wind_mps = 0.0F;
    float vertical_wind_mps = 0.0F;
    float wind_direction_deg = 0.0F;
    float barometric_pressure_mb = 0.0F;
    float aerosol_concentration = 0.0F;  // g/m^3
};

// ICD 4.2.1 Start of Frame (p. 212-216): IG to host, first packet of each
// message.
struct StartOfFrame {
    std::uint8_t minor_version = kMinorVersionSent;  // 4 bits; decoded as received
    std::int8_t database_number = 0;                 // 0: the IG controls loading
    std::uint8_t ig_status = 0;                      // 0: normal operation
    IgMode ig_mode = IgMode::reset_standby;
    bool timestamp_valid = false;
    bool host_defined_earth_model = false;  // Earth Reference Model: false is WGS 84
    std::uint32_t ig_frame_number = 0;
    std::uint32_t timestamp = 0;  // 10 us ticks
    std::uint32_t last_host_frame_number = 0;
};

// Encoding: writes the packet at the start of `out` in native byte order and
// returns its size, or 0 if `out` is too small.
std::size_t encode(const IgControl& p, std::span<std::uint8_t> out);
std::size_t encode(const EntityControl& p, std::span<std::uint8_t> out);
std::size_t encode(const CelestialSphereControl& p, std::span<std::uint8_t> out);
std::size_t encode(const AtmosphereControl& p, std::span<std::uint8_t> out);
std::size_t encode(const WeatherControl& p, std::span<std::uint8_t> out);
std::size_t encode(const StartOfFrame& p, std::span<std::uint8_t> out);

// Decoding a whole message.
enum class MessageError : std::uint8_t {
    none,
    too_short,           // not even the first packet's header and magic number
    wrong_first_packet,  // not IG Control (host to IG) or Start of Frame (IG to host)
    wrong_major_version,
    bad_magic,        // neither 8000h nor 80h
    bad_packet_size,  // a packet's size is too small for it, or runs past the message
};

// Walks the packets of one CIGI 3 message (one-byte packet ID and size; CIGI
// 4 frames packets differently and gets its own reader). The constructor checks the first packet
// (its opcode, major version and byte swap magic number); next() then yields
// every packet, the first included, until the end or a malformed size.
class MessageReader {
public:
    // `first_packet_id`: kIgControlId for host-to-IG, kStartOfFrameId for
    // IG-to-host messages. The bytes must outlive the reader.
    MessageReader(std::span<const std::uint8_t> message, std::uint8_t first_packet_id);

    // none until a check or a packet size fails; then next() returns false.
    MessageError error() const { return error_; }
    bool swapped() const { return swap_; }  // the sender's byte order differs from ours

    struct Packet {
        std::uint8_t id = 0;
        std::span<const std::uint8_t> bytes;  // the whole packet, header included
    };
    // The next packet, or false at the end of the message or on an error.
    bool next(Packet& packet);

private:
    std::span<const std::uint8_t> message_;
    std::size_t offset_ = 0;
    bool swap_ = false;
    MessageError error_ = MessageError::none;
};

// Decoding one packet from MessageReader: nothing if the ID is wrong, the
// packet is shorter than the ICD's size, or a field holds a value the ICD
// leaves undefined (Entity State or Ground/Ocean Clamp 3; IG Mode 3 in IG
// Control, which only the IG may report; Hour over 23 or Minute over 59;
// Weather Control Scope 3 or Severity over 5). Longer packets are accepted (ICD
// 2.11, rule 4). `swap`: MessageReader::swapped().
std::optional<IgControl> decode_ig_control(std::span<const std::uint8_t> in, bool swap);
std::optional<EntityControl> decode_entity_control(std::span<const std::uint8_t> in, bool swap);
std::optional<CelestialSphereControl> decode_celestial_sphere_control(
    std::span<const std::uint8_t> in, bool swap);
std::optional<AtmosphereControl> decode_atmosphere_control(std::span<const std::uint8_t> in,
                                                           bool swap);
std::optional<WeatherControl> decode_weather_control(std::span<const std::uint8_t> in, bool swap);
std::optional<StartOfFrame> decode_start_of_frame(std::span<const std::uint8_t> in, bool swap);

}  // namespace fovea::cigi
