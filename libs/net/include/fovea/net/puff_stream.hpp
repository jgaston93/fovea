#pragma once

// The puff stream's codec (D-039, docs/integration-guide.md, "Particle
// volumes"): a host's particle volumes over UDP, version 0.2. Every datagram
// stands alone: a header, then an origin, whole puffs or whole clusters (D-053). Little-endian,
// read field by field; decoding never allocates and never reads past the buffer.

#include <cstddef>
#include <cstdint>
#include <span>

namespace fovea::puff {

inline constexpr std::uint32_t kMagic = 0x46465550U;  // "PUFF" in memory
inline constexpr std::uint16_t kVersionMajor = 0;
inline constexpr std::uint16_t kVersionMinor = 2;
inline constexpr std::size_t kHeaderBytes = 48;  // this version's header
inline constexpr std::size_t kOriginBytes = 24;
inline constexpr std::size_t kPuffBytes = 36;     // this version's puff record
inline constexpr std::size_t kClusterBytes = 64;  // and cluster record (0.2)
inline constexpr std::size_t kMaxDatagram = 1472;

enum class Type : std::uint8_t { origin = 1, puffs = 2, clusters = 3 };

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

// A cluster (0.2): an anisotropic Gaussian, its covariance in east-north-up
// (symmetric, positive definite), spreading by its diffusivity, its total
// extinction cross-section.
struct Cluster {
    std::uint32_t id = 0;
    float east = 0.0F, north = 0.0F, up = 0.0F;        // [m] the centre, from the origin
    float v_east = 0.0F, v_north = 0.0F, v_up = 0.0F;  // [m/s]
    float c_ee = 0.0F, c_en = 0.0F, c_eu = 0.0F;       // [m^2] the covariance
    float c_nn = 0.0F, c_nu = 0.0F, c_uu = 0.0F;
    float diffusivity = 0.0F;  // [m^2/s] the covariance grows by 2 K I a second
    float extinction = 0.0F;   // [m^2] the total extinction cross-section
};

enum class HeaderResult : std::uint8_t {
    ok,            // an origin or puffs, its body complete
    unknown_type,  // a newer minor's type: skip it, it isn't an error
    rejected,      // a wrong magic, another major version, or shorter than it says
};

// Reads a datagram's header. For an origin, the body holds one; for puffs or
// clusters, `count` records of `record_bytes` (at least kPuffBytes or
// kClusterBytes) fit in it.
HeaderResult read_header(std::span<const std::uint8_t> datagram, Header& out) noexcept;

// The origin of a datagram whose header read ok as an origin.
Origin read_origin(std::span<const std::uint8_t> datagram, const Header& h) noexcept;

// Puff `i` (< h.count) of a datagram whose header read ok as puffs. A newer
// minor's longer record is read for the fields this version knows.
Puff read_puff(std::span<const std::uint8_t> datagram, const Header& h, std::size_t i) noexcept;
// Cluster `i` (< h.count) of a datagram whose header read ok as clusters.
Cluster read_cluster(std::span<const std::uint8_t> datagram, const Header& h,
                     std::size_t i) noexcept;

// Writing, for tests and test senders. Each returns the bytes written, or 0
// if `out` is too small. `h.header_bytes`, `record_bytes` and `count` are set
// from what is written.
std::size_t write_origin(std::span<std::uint8_t> out, Header h, const Origin& o) noexcept;
std::size_t write_puffs(std::span<std::uint8_t> out, Header h,
                        std::span<const Puff> puffs) noexcept;
std::size_t write_clusters(std::span<std::uint8_t> out, Header h,
                           std::span<const Cluster> clusters) noexcept;

}  // namespace fovea::puff
