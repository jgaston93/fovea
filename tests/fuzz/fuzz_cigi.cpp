// libFuzzer target for the CIGI codec (milestone 2 step 1; the environment's
// packets from milestone 6 step 7; HAT/HOT from Integration 1 step 2): any bytes, read as a
// host-to-IG and as an IG-to-host message, must never read out of bounds or crash (ASan and UBSan
// watch), and every decoded packet must survive encode -> decode -> encode unchanged. Built by the
// `fuzz` preset (Clang only); the nightly run fuzzes for five minutes:
//   cmake --preset fuzz && cmake --build --preset fuzz --target fovea_fuzz_cigi
//   ./build/fuzz/tests/fovea_fuzz_cigi -max_total_time=60 tests/fuzz/corpus/cigi

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "fovea/net/cigi.hpp"

namespace {

using namespace fovea::cigi;

// encode(decode(encode(p))) must equal encode(p) in either byte order: no
// field lost or mangled.
template <typename Packet, typename Decode>
void check_round_trip(const Packet& p, Decode decode) {
    for (const bool swap : {false, true}) {
        std::array<std::uint8_t, 64> first{};
        std::array<std::uint8_t, 64> second{};
        const std::size_t n = encode(p, first, swap);
        const auto back = decode(std::span<const std::uint8_t>(first).first(n), swap);
        if (n == 0 || !back || encode(*back, second, swap) != n ||
            std::memcmp(first.data(), second.data(), n) != 0) {
            std::abort();
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> message(data, size);
    for (const std::uint8_t first : {kIgControlId, kStartOfFrameId}) {
        MessageReader reader(message, first);
        MessageReader::Packet p;
        while (reader.next(p)) {
            if (p.bytes.data() < data || p.bytes.data() + p.bytes.size() > data + size) {
                std::abort();
            }
            if (const auto ig = decode_ig_control(p.bytes, reader.swapped())) {
                check_round_trip(*ig, decode_ig_control);
            }
            if (const auto e = decode_entity_control(p.bytes, reader.swapped())) {
                check_round_trip(*e, decode_entity_control);
            }
            if (const auto c = decode_celestial_sphere_control(p.bytes, reader.swapped())) {
                check_round_trip(*c, decode_celestial_sphere_control);
            }
            if (const auto a = decode_atmosphere_control(p.bytes, reader.swapped())) {
                check_round_trip(*a, decode_atmosphere_control);
            }
            if (const auto w = decode_weather_control(p.bytes, reader.swapped())) {
                check_round_trip(*w, decode_weather_control);
            }
            if (const auto q = decode_hat_hot_request(p.bytes, reader.swapped())) {
                check_round_trip(*q, decode_hat_hot_request);
            }
            if (const auto sof = decode_start_of_frame(p.bytes, reader.swapped())) {
                check_round_trip(*sof, decode_start_of_frame);
            }
            if (const auto r = decode_hat_hot_response(p.bytes, reader.swapped())) {
                check_round_trip(*r, decode_hat_hot_response);
            }
            if (const auto x = decode_hat_hot_extended_response(p.bytes, reader.swapped())) {
                check_round_trip(*x, decode_hat_hot_extended_response);
            }
        }
    }
    return 0;
}
