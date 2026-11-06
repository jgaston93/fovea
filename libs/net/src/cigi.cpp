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
void put(std::span<std::uint8_t> out, std::size_t at, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(out.data() + at, &value, sizeof(T));
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
std::size_t encode(const IgControl& p, std::span<std::uint8_t> out) {
    if (out.size() < kIgControlSize) {
        return 0;
    }
    clear(out, kIgControlSize);
    out[0] = kIgControlId;
    out[1] = kIgControlSize;
    out[2] = kMajorVersion;
    put(out, 3, p.database_number);
    out[4] = static_cast<std::uint8_t>(
        field(p.minor_version, 4, 4) | field(flag(p.extrapolation_enable), 3, 1) |
        field(flag(p.timestamp_valid), 2, 1) | field(static_cast<unsigned>(p.ig_mode), 0, 2));
    put(out, 6, kMagicNoSwap);
    put(out, 8, p.host_frame_number);
    put(out, 12, p.timestamp);
    put(out, 16, p.last_ig_frame_number);
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
std::size_t encode(const EntityControl& p, std::span<std::uint8_t> out) {
    if (out.size() < kEntityControlSize) {
        return 0;
    }
    clear(out, kEntityControlSize);
    out[0] = kEntityControlId;
    out[1] = kEntityControlSize;
    put(out, 2, p.entity_id);
    out[4] = static_cast<std::uint8_t>(
        field(static_cast<unsigned>(p.ground_ocean_clamp), 5, 2) |
        field(flag(p.inherit_alpha), 4, 1) | field(flag(p.collision_detection), 3, 1) |
        field(flag(p.attach), 2, 1) | field(static_cast<unsigned>(p.entity_state), 0, 2));
    out[5] = static_cast<std::uint8_t>(field(flag(p.extrapolation_enable), 4, 1) |
                                       field(static_cast<unsigned>(p.animation_state), 2, 2) |
                                       field(flag(p.animation_loop), 1, 1) |
                                       field(flag(p.animation_backward), 0, 1));
    out[6] = p.alpha;
    put(out, 8, p.entity_type);
    put(out, 10, p.parent_id);
    put(out, 12, p.roll_deg);
    put(out, 16, p.pitch_deg);
    put(out, 20, p.yaw_deg);
    put(out, 24, p.lat_or_x);
    put(out, 32, p.lon_or_y);
    put(out, 40, p.alt_or_z);
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

// ICD 4.2.1, Figure 109 (p. 212):
//   0 Packet ID = 101 | 1 Packet Size = 24 | 2 Major Version = 3 | 3 Database Number
//   4 IG Status Code | 5 Minor Version (7-4), Earth Reference Model (3),
//     Timestamp Valid (2), IG Mode (1-0) | 6-7 Byte Swap Magic Number
//   8-11 IG Frame Number | 12-15 Timestamp | 16-19 Last Host Frame Number
//   20-23 Reserved
std::size_t encode(const StartOfFrame& p, std::span<std::uint8_t> out) {
    if (out.size() < kStartOfFrameSize) {
        return 0;
    }
    clear(out, kStartOfFrameSize);
    out[0] = kStartOfFrameId;
    out[1] = kStartOfFrameSize;
    out[2] = kMajorVersion;
    put(out, 3, p.database_number);
    out[4] = p.ig_status;
    out[5] = static_cast<std::uint8_t>(
        field(p.minor_version, 4, 4) | field(flag(p.host_defined_earth_model), 3, 1) |
        field(flag(p.timestamp_valid), 2, 1) | field(static_cast<unsigned>(p.ig_mode), 0, 2));
    put(out, 6, kMagicNoSwap);
    put(out, 8, p.ig_frame_number);
    put(out, 12, p.timestamp);
    put(out, 16, p.last_host_frame_number);
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
