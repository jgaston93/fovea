#pragma once

// The puff stream's codec (D-039, docs/integration-guide.md, "Particle
// volumes"): a host's particle volumes over UDP, version 0.1. Every datagram
// stands alone: a header, then an origin or whole puffs. Little-endian, read
// field by field; decoding never allocates and never reads past the buffer.

#include <cstddef>
#include <cstdint>
#include <span>

namespace fovea::puff {

inline constexpr std::uint32_t kMagic = 0x46465550U;  // "PUFF" in memory
inline constexpr std::uint16_t kVersionMajor = 0;
inline constexpr std::uint16_t kVersionMinor = 1;
inline constexpr std::size_t kHeaderBytes = 48;  // this version's header
inline constexpr std::size_t kOriginBytes = 24;
inline constexpr std::size_t kPuffBytes = 36;  // this version's record
inline constexpr std::size_t kMaxDatagram = 1472;

enum class Type : std::uint8_t { origin = 1, puffs = 2 };

inline constexpr std::uint8_t kFlagFrozen = 1U << 0;

struct Header {
    std::uint16_t version_minor = 0;
    std::uint8_t type = 0;  // Type, or a newer minor's
    std::uint8_t flags = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t source_id = 0;
    std::uint64_t sim_time_ns = 0;
    std::uint32_t sequence = 0;
    std::uint32_t origin_id = 0;
    std::uint32_t hold_us = 0;
    std::uint16_t record_bytes = 0;
    std::uint16_t count = 0;
    std::uint32_t session = 0;
};

struct Origin {
    double latitude_rad = 0.0;  // WGS-84 geodetic
    double longitude_rad = 0.0;
    double height_msl_m = 0.0;
};

struct Puff {
    std::uint32_t id = 0;
    float east = 0.0F, north = 0.0F, up = 0.0F;        // [m] from the origin
    float v_east = 0.0F, v_north = 0.0F, v_up = 0.0F;  // [m/s]
    float radius = 0.0F;                               // [m]
    float bulk_density = 0.0F;                         // [kg/m^3]
};

enum class HeaderResult : std::uint8_t {
    ok,            // an origin or puffs, its body complete
    unknown_type,  // a newer minor's type: skip it, it isn't an error
    rejected,      // a wrong magic, another major version, or shorter than it says
};

// Reads a datagram's header. For an origin, the body holds one; for puffs,
// `count` records of `record_bytes` (at least kPuffBytes) fit in it.
HeaderResult read_header(std::span<const std::uint8_t> datagram, Header& out) noexcept;

// The origin of a datagram whose header read ok as an origin.
Origin read_origin(std::span<const std::uint8_t> datagram, const Header& h) noexcept;

// Puff `i` (< h.count) of a datagram whose header read ok as puffs. A newer
// minor's longer record is read for the fields this version knows.
Puff read_puff(std::span<const std::uint8_t> datagram, const Header& h, std::size_t i) noexcept;

// Writing, for tests and test senders. Each returns the bytes written, or 0
// if `out` is too small. `h.header_bytes`, `record_bytes` and `count` are set
// from what is written.
std::size_t write_origin(std::span<std::uint8_t> out, Header h, const Origin& o) noexcept;
std::size_t write_puffs(std::span<std::uint8_t> out, Header h,
                        std::span<const Puff> puffs) noexcept;

}  // namespace fovea::puff
