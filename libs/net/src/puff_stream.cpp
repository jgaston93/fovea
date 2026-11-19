#include "fovea/net/puff_stream.hpp"

#include <bit>
#include <cstring>
#include <type_traits>

namespace fovea::puff {

namespace {

// Little-endian reads and writes, byte by byte: the stream's order on any
// machine, never a cast of the buffer.
template <typename T>
T get(std::span<const std::uint8_t> b, std::size_t at) noexcept {
    using U = std::conditional_t<
        sizeof(T) == 8, std::uint64_t,
        std::conditional_t<sizeof(T) == 4, std::uint32_t,
                           std::conditional_t<sizeof(T) == 2, std::uint16_t, std::uint8_t>>>;
    U u = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        u = static_cast<U>(u | (static_cast<U>(b[at + i]) << (8 * i)));
    }
    return std::bit_cast<T>(u);
}

template <typename T>
void put(std::span<std::uint8_t> b, std::size_t at, T v) noexcept {
    using U = std::conditional_t<
        sizeof(T) == 8, std::uint64_t,
        std::conditional_t<sizeof(T) == 4, std::uint32_t,
                           std::conditional_t<sizeof(T) == 2, std::uint16_t, std::uint8_t>>>;
    const U u = std::bit_cast<U>(v);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        b[at + i] = static_cast<std::uint8_t>(u >> (8 * i));
    }
}

void put_header(std::span<std::uint8_t> b, const Header& h) noexcept {
    std::memset(b.data(), 0, kHeaderBytes);
    put(b, 0, kMagic);
    put(b, 4, kVersionMajor);
    put(b, 6, h.version_minor);
    put(b, 8, h.type);
    put(b, 9, h.flags);
    put(b, 10, h.header_bytes);
    put(b, 12, h.source_id);
    put(b, 16, h.sim_time_ns);
    put(b, 24, h.sequence);
    put(b, 28, h.origin_id);
    put(b, 32, h.hold_us);
    put(b, 36, h.record_bytes);
    put(b, 38, h.count);
    put(b, 40, h.session);
    // 44: reserved, 0
}

}  // namespace

HeaderResult read_header(std::span<const std::uint8_t> d, Header& out) noexcept {
    if (d.size() < kHeaderBytes || get<std::uint32_t>(d, 0) != kMagic ||
        get<std::uint16_t>(d, 4) != kVersionMajor) {
        return HeaderResult::rejected;
    }
    Header h;
    h.version_minor = get<std::uint16_t>(d, 6);
    h.type = get<std::uint8_t>(d, 8);
    h.flags = get<std::uint8_t>(d, 9);
    h.header_bytes = get<std::uint16_t>(d, 10);
    h.source_id = get<std::uint32_t>(d, 12);
    h.sim_time_ns = get<std::uint64_t>(d, 16);
    h.sequence = get<std::uint32_t>(d, 24);
    h.origin_id = get<std::uint32_t>(d, 28);
    h.hold_us = get<std::uint32_t>(d, 32);
    h.record_bytes = get<std::uint16_t>(d, 36);
    h.count = get<std::uint16_t>(d, 38);
    h.session = get<std::uint32_t>(d, 40);
    if (h.header_bytes < kHeaderBytes || h.header_bytes > d.size()) {
        return HeaderResult::rejected;
    }
    const std::size_t body = d.size() - h.header_bytes;
    out = h;
    if (h.type == static_cast<std::uint8_t>(Type::origin)) {
        return body >= kOriginBytes ? HeaderResult::ok : HeaderResult::rejected;
    }
    if (h.type == static_cast<std::uint8_t>(Type::puffs)) {
        // At most 65535 x 65535: no overflow in a 64-bit size_t, nor a 32-bit one.
        const std::size_t need = std::size_t{h.record_bytes} * h.count;
        return h.record_bytes >= kPuffBytes && need <= body ? HeaderResult::ok
                                                            : HeaderResult::rejected;
    }
    return HeaderResult::unknown_type;
}

Origin read_origin(std::span<const std::uint8_t> d, const Header& h) noexcept {
    const std::size_t at = h.header_bytes;
    return {get<double>(d, at), get<double>(d, at + 8), get<double>(d, at + 16)};
}

Puff read_puff(std::span<const std::uint8_t> d, const Header& h, std::size_t i) noexcept {
    const std::size_t at = h.header_bytes + i * h.record_bytes;
    Puff p;
    p.id = get<std::uint32_t>(d, at);
    p.east = get<float>(d, at + 4);
    p.north = get<float>(d, at + 8);
    p.up = get<float>(d, at + 12);
    p.v_east = get<float>(d, at + 16);
    p.v_north = get<float>(d, at + 20);
    p.v_up = get<float>(d, at + 24);
    p.radius = get<float>(d, at + 28);
    p.bulk_density = get<float>(d, at + 32);
    return p;
}

std::size_t write_origin(std::span<std::uint8_t> out, Header h, const Origin& o) noexcept {
    if (out.size() < kHeaderBytes + kOriginBytes) {
        return 0;
    }
    h.type = static_cast<std::uint8_t>(Type::origin);
    h.header_bytes = kHeaderBytes;
    h.record_bytes = 0;
    h.count = 0;
    put_header(out, h);
    put(out, kHeaderBytes, o.latitude_rad);
    put(out, kHeaderBytes + 8, o.longitude_rad);
    put(out, kHeaderBytes + 16, o.height_msl_m);
    return kHeaderBytes + kOriginBytes;
}

std::size_t write_puffs(std::span<std::uint8_t> out, Header h,
                        std::span<const Puff> puffs) noexcept {
    const std::size_t n = kHeaderBytes + puffs.size() * kPuffBytes;
    if (puffs.size() > 0xFFFF || out.size() < n) {
        return 0;
    }
    h.type = static_cast<std::uint8_t>(Type::puffs);
    h.header_bytes = kHeaderBytes;
    h.record_bytes = kPuffBytes;
    h.count = static_cast<std::uint16_t>(puffs.size());
    put_header(out, h);
    for (std::size_t i = 0; i < puffs.size(); ++i) {
        const std::size_t at = kHeaderBytes + i * kPuffBytes;
        const Puff& p = puffs[i];
        put(out, at, p.id);
        put(out, at + 4, p.east);
        put(out, at + 8, p.north);
        put(out, at + 12, p.up);
        put(out, at + 16, p.v_east);
        put(out, at + 20, p.v_north);
        put(out, at + 24, p.v_up);
        put(out, at + 28, p.radius);
        put(out, at + 32, p.bulk_density);
    }
    return n;
}

}  // namespace fovea::puff
