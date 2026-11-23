#pragma once

// The puff stream's adapter (D-039): receives a host's particle volumes on a
// UDP port of its own, keeps them by (source, id), and publishes them as
// ParticleFrames. It runs on its own thread (PollThread), apart from CIGI, so
// a few thousand datagrams a second never delay a host's entity updates or
// terrain answers. Nothing allocates after the constructor.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fovea/core/particles.hpp"
#include "fovea/core/time.hpp"
#include "fovea/net/puff_stream.hpp"
#include "fovea/net/udp.hpp"

namespace fovea {

struct PuffAdapterOptions {
    std::uint16_t port = 0;     // 0: any free port (tests)
    std::uint32_t address = 0;  // the local address to listen on; 0: every interface
    // A multicast group to join (D-055; 0: none): then `address` is the
    // interface it's joined on, and the port is bound on every interface.
    std::uint32_t group = 0;
    double geoid_height_m = 0.0;  // the geoid above WGS-84, added to origins' MSL heights (D-035)
    std::chrono::duration<float> fade{0.25F};      // a particle's fade in and out
    std::chrono::milliseconds publish_period{16};  // at most this often (about 60 Hz)
    int wait_ms = 5;                               // poll()'s wait for a datagram
};

// Counts, written by the adapter's thread; read them with relaxed loads (a
// live display) or after the thread stops.
struct PuffAdapterStats {
    std::atomic<std::uint64_t> datagrams{0};  // received
    // Datagrams malformed or truncated, from a source over kMaxSources, or
    // from a source's previous session (a stray after a restart).
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> rejected_records{0};  // non-finite, or a negative radius or density
    std::atomic<std::uint64_t> skipped{0};           // a newer minor's datagram type
    std::atomic<std::uint64_t> origins{0};           // origin datagrams applied
    std::atomic<std::uint64_t> unknown_origin{0};  // puff datagrams naming an origin not yet heard
    std::atomic<std::uint64_t> origins_full{0};    // origins over kMaxParticleOrigins, dropped
    std::atomic<std::uint64_t> records{0};         // puff records taken
    std::atomic<std::uint64_t> late{0};            // records older than the one held
    std::atomic<std::uint64_t> table_full{0};      // new particles over the capacity, dropped
    std::atomic<std::uint64_t> lost{0};            // datagrams never received, by sequence spans
    std::atomic<std::uint64_t> sessions{0};        // sources (re)started: a new session
    std::atomic<std::uint64_t> alive{0};           // particles in the last published frame
    std::atomic<std::uint64_t> publishes{0};
};

class PuffAdapter {
public:
    // Senders at once. A sender silent for kForget with no particles left is
    // forgotten, freeing its place; so is an origin no particle names.
    static constexpr std::size_t kMaxSources = 8;
    static constexpr std::chrono::seconds kForget{10};
    static constexpr std::chrono::seconds kForgetOrigin{2};

    // Binds the socket (throws std::runtime_error if it can't); the state's
    // capacity is the most particles kept.
    PuffAdapter(const PuffAdapterOptions& options, ParticleState& state);
    PuffAdapter(const PuffAdapter&) = delete;
    PuffAdapter& operator=(const PuffAdapter&) = delete;

    // Waits for datagrams, applies them, and publishes when due. Always true
    // (a PollThread runs it until stopped).
    bool poll();

    // One datagram, as if received at `t` (poll() uses it; tests call it).
    void apply(std::span<const std::uint8_t> datagram, LocalTime t) noexcept;
    // Drops what has faded out, closes loss windows and publishes (poll()
    // calls it when due; tests call it).
    void publish(LocalTime t) noexcept;

    std::uint16_t port() const { return socket_.local_port(); }
    std::size_t receive_buffer_bytes() const { return socket_.receive_buffer_bytes(); }
    const PuffAdapterStats& stats() const { return stats_; }

private:
    struct Held {
        Particle p;
        std::uint32_t source = 0;  // index into sources_
        std::uint32_t id = 0;
        std::uint64_t sim_time_ns = 0;
    };
    struct OriginSlot {
        bool used = false;
        std::uint32_t source = 0;  // index into sources_
        std::uint32_t id = 0;
        LocalTime last_used;   // announced or named
        std::size_t refs = 0;  // particles placed from it
        ParticleOrigin frame;
    };
    struct Source {
        bool used = false;
        std::uint32_t id = 0;
        std::uint32_t session = 0;
        bool has_previous = false;
        std::uint32_t previous_session = 0;
        LocalTime last_heard;
        std::size_t alive = 0;  // its particles held
        // Losses: sequences from `base`, the window's first expected; a
        // window closes each second and the next starts after its highest.
        bool counting = false;
        std::uint32_t base = 0;
        std::int64_t highest = -1;
        std::uint64_t received = 0;
    };

    Source* source_for(const puff::Header& h, LocalTime t) noexcept;
    void reset_source(std::uint32_t index) noexcept;
    void close_window(Source& s) noexcept;
    static void count_sequence(Source& s, std::uint32_t sequence) noexcept;
    int find_origin(std::uint32_t source, std::uint32_t id) const noexcept;
    void apply_origin(std::uint32_t source, const puff::Header& h, const puff::Origin& o,
                      LocalTime t) noexcept;
    void apply_puffs(std::uint32_t source, std::span<const std::uint8_t> d, const puff::Header& h,
                     LocalTime t) noexcept;
    std::size_t find_slot(std::uint32_t source, std::uint32_t id) const noexcept;  // npos if absent
    std::size_t find(std::uint32_t source, std::uint32_t id) const noexcept;       // npos if absent
    void index_insert(std::size_t at) noexcept;
    void index_erase(std::size_t slot) noexcept;
    void remove(std::size_t at) noexcept;
    void compact(LocalTime t) noexcept;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    PuffAdapterOptions options_;
    ParticleState& state_;
    UdpSocket socket_;
    std::array<std::uint8_t, 2048> buffer_{};
    std::array<Source, kMaxSources> sources_{};
    std::array<OriginSlot, kMaxParticleOrigins> origins_{};
    std::vector<Held> held_;  // the first count_ are live; sized at construction
    std::size_t count_ = 0;
    std::vector<std::uint32_t> index_;  // open addressing: position + 1, 0 empty; a power of two
    LocalTime last_publish_;
    LocalTime window_start_;
    PuffAdapterStats stats_;
};

}  // namespace fovea
