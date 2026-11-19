// libFuzzer target for the puff stream's codec (D-039): any bytes, read as a
// datagram, must never read out of bounds or crash (ASan and UBSan watch),
// and an accepted datagram's records must all be readable; any bytes applied
// to the adapter likewise. Built by the `fuzz` preset (Clang only):
//   cmake --preset fuzz && cmake --build --preset fuzz --target fovea_fuzz_puff_stream
//   ./build/fuzz/tests/fovea_fuzz_puff_stream -max_total_time=60

#include <cstdint>
#include <span>

#include "fovea/core/particles.hpp"
#include "fovea/net/puff_adapter.hpp"
#include "fovea/net/puff_stream.hpp"
#include "fovea/net/udp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> d(data, size);
    fovea::puff::Header h;
    if (fovea::puff::read_header(d, h) == fovea::puff::HeaderResult::ok) {
        if (h.type == static_cast<std::uint8_t>(fovea::puff::Type::origin)) {
            (void)fovea::puff::read_origin(d, h);
        } else {
            for (std::size_t i = 0; i < h.count; ++i) {
                (void)fovea::puff::read_puff(d, h, i);
            }
        }
    }
    static fovea::ParticleState state(64);
    static fovea::PuffAdapter adapter(
        [] {
            fovea::PuffAdapterOptions o;
            o.address = fovea::kLoopback;
            return o;
        }(),
        state);
    adapter.apply(d, fovea::now());
    adapter.publish(fovea::now());
    return 0;
}
