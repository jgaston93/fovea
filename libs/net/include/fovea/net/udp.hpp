#pragma once

// A thin non-blocking UDP socket (IPv4), the only platform-specific code in
// libs/net (D-014): BSD sockets, or Winsock on Windows. Sending and
// receiving never allocate.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "fovea/core/ipv4.hpp"

namespace fovea {

// An IPv4 address and port, both in host byte order.
struct UdpEndpoint {
    std::uint32_t address = 0;  // 0x7F000001 is 127.0.0.1
    std::uint16_t port = 0;

    friend bool operator==(const UdpEndpoint&, const UdpEndpoint&) = default;
};

inline constexpr std::uint32_t kLoopback = 0x7F000001;

std::string format_endpoint(const UdpEndpoint& e);  // "a.b.c.d:port"

class UdpSocket {
public:
    // Binds to `port` on `address` (0: every interface; port 0: any free
    // port, see local_port()). Throws std::runtime_error if it can't.
    explicit UdpSocket(std::uint16_t port, std::uint32_t address = 0);
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    std::uint16_t local_port() const { return local_port_; }

    // The receive buffer the system granted: datagrams that arrive while it
    // is full are dropped. 4 MB is asked for; Linux caps it at twice
    // net.core.rmem_max (208 KB by default, so about 416 KB).
    std::size_t receive_buffer_bytes() const;

    // Waits up to `timeout_ms` for a datagram to arrive (0: just checks).
    // True if one is waiting.
    // Not const, though no member changes: the socket's state does.
    bool wait_readable(int timeout_ms);  // NOLINT(readability-make-member-function-const)

    // The next waiting datagram, copied into `buffer` (cut to its size), and
    // where it came from. Nothing when no datagram is waiting. Errors from
    // earlier sends that the system reports here (an ICMP "port
    // unreachable") are skipped.
    struct Received {
        std::size_t size = 0;
        bool truncated = false;  // the datagram was larger than the buffer
        UdpEndpoint from;
    };
    std::optional<Received> receive(  // NOLINT(readability-make-member-function-const)
        std::span<std::uint8_t> buffer);

    // Sends one datagram. False if the system refused it (the send buffer is
    // full, or the destination is unreachable).
    bool send_to(  // NOLINT(readability-make-member-function-const)
        std::span<const std::uint8_t> data, const UdpEndpoint& to);

private:
    std::intptr_t handle_ = -1;  // an int on POSIX, a SOCKET on Windows
    std::uint16_t local_port_ = 0;
};

}  // namespace fovea
