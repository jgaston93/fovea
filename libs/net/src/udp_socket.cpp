#include "fovea/net/udp.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace fovea {

namespace {

#ifdef _WIN32
using Native = SOCKET;
constexpr Native kInvalid = INVALID_SOCKET;

// Winsock is started once per process, on first use, and left running.
void start_winsock() {
    static const bool started = [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("cannot start Winsock");
        }
        return true;
    }();
    (void)started;
}

void close_native(Native s) {
    closesocket(s);
}

int last_error() {
    return WSAGetLastError();
}

// A previous send's ICMP "port unreachable" (WSAECONNRESET) or a datagram
// cut short (WSAEMSGSIZE, handled by the caller) surface from recvfrom on
// Windows.
bool skippable_receive_error(int err) {
    return err == WSAECONNRESET;
}
#else
using Native = int;
constexpr Native kInvalid = -1;

void start_winsock() {}

void close_native(Native s) {
    ::close(s);
}

int last_error() {
    return errno;
}

// Linux reports a previous send's ICMP "port unreachable" as ECONNREFUSED on
// the next receive.
bool skippable_receive_error(int err) {
    return err == ECONNREFUSED;
}
#endif

Native native(std::intptr_t h) {
    return static_cast<Native>(h);
}

sockaddr_in to_sockaddr(const UdpEndpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = htonl(e.address);
    return a;
}

UdpEndpoint from_sockaddr(const sockaddr_in& a) {
    return {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
}

[[noreturn]] void fail(const char* what, int err) {
    std::array<char, 96> text{};
    (void)std::snprintf(text.data(), text.size(), "UDP socket: %s failed (error %d)", what, err);
    throw std::runtime_error(text.data());
}

}  // namespace

std::string format_endpoint(const UdpEndpoint& e) {
    std::array<char, 32> text{};
    (void)std::snprintf(text.data(), text.size(), "%u.%u.%u.%u:%u", (e.address >> 24) & 0xFFU,
                        (e.address >> 16) & 0xFFU, (e.address >> 8) & 0xFFU, e.address & 0xFFU,
                        unsigned{e.port});
    return text.data();
}

UdpSocket::UdpSocket(std::uint16_t port, std::uint32_t address) {
    start_winsock();
    const Native s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        fail("socket()", last_error());
    }
    handle_ = static_cast<std::intptr_t>(s);
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool ok = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    const bool ok = flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    // Room for bursts; best effort, the system may grant less.
    const int wanted = 4 * 1024 * 1024;
    (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&wanted),
                       sizeof(wanted));
    const sockaddr_in local = to_sockaddr({address, port});
    if (!ok) {
        const int err = last_error();
        close_native(s);
        fail("setting non-blocking mode", err);
    }
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        const int err = last_error();
        close_native(s);
        fail("bind()", err);
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        const int err = last_error();
        close_native(s);
        fail("getsockname()", err);
    }
    local_port_ = ntohs(bound.sin_port);
}

std::size_t UdpSocket::receive_buffer_bytes() const {
    int size = 0;
    socklen_t length = sizeof(size);
    if (::getsockopt(native(handle_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&size),
                     &length) != 0) {
        return 0;
    }
    return static_cast<std::size_t>(size);
}

UdpSocket::~UdpSocket() {
    close_native(native(handle_));
}

bool UdpSocket::wait_readable(int timeout_ms) {  // NOLINT(readability-make-member-function-const)
#ifdef _WIN32
    WSAPOLLFD fd{};
    fd.fd = native(handle_);
    fd.events = POLLRDNORM;
    return WSAPoll(&fd, 1, timeout_ms) > 0;
#else
    pollfd fd{};
    fd.fd = native(handle_);
    fd.events = POLLIN;
    return ::poll(&fd, 1, timeout_ms) > 0;
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const): the socket's state changes
std::optional<UdpSocket::Received> UdpSocket::receive(std::span<std::uint8_t> buffer) {
    for (;;) {
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        Received r;
#ifdef _WIN32
        const int n = ::recvfrom(native(handle_), reinterpret_cast<char*>(buffer.data()),
                                 static_cast<int>(buffer.size()), 0,
                                 reinterpret_cast<sockaddr*>(&from), &length);
        if (n < 0 && last_error() == WSAEMSGSIZE) {
            // Windows fills the buffer and reports the rest as lost.
            r.size = buffer.size();
            r.truncated = true;
            r.from = from_sockaddr(from);
            return r;
        }
#else
        // MSG_TRUNC: the datagram's full length comes back, so a cut is seen.
        const auto n = ::recvfrom(native(handle_), buffer.data(), buffer.size(), MSG_TRUNC,
                                  reinterpret_cast<sockaddr*>(&from), &length);
#endif
        if (n < 0) {
            const int err = last_error();
            if (skippable_receive_error(err)) {
                continue;
            }
            return std::nullopt;  // nothing waiting (would block), or a failure
        }
        const auto full = static_cast<std::size_t>(n);
        r.truncated = full > buffer.size();
        r.size = r.truncated ? buffer.size() : full;
        r.from = from_sockaddr(from);
        return r;
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const): the socket's state changes
bool UdpSocket::send_to(std::span<const std::uint8_t> data, const UdpEndpoint& to) {
    const sockaddr_in a = to_sockaddr(to);
#ifdef _WIN32
    const int n = ::sendto(native(handle_), reinterpret_cast<const char*>(data.data()),
                           static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&a),
                           sizeof(a));
#else
    const auto n = ::sendto(native(handle_), data.data(), data.size(), 0,
                            reinterpret_cast<const sockaddr*>(&a), sizeof(a));
#endif
    return n >= 0 && static_cast<std::size_t>(n) == data.size();
}

}  // namespace fovea
