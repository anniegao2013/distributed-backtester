#include "net/tcp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace net {

namespace {

[[noreturn]] void throw_system_error(
    const std::string& action,
    int error
) {
    throw std::runtime_error(action + ": " + std::strerror(error));
}


void send_all(int descriptor, const char* data, std::size_t size) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t written = ::send(
            descriptor,
            data + sent,
            size - sent,
            flags
        );

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written < 0) {
            throw_system_error("TCP send failed", errno);
        }

        if (written == 0) {
            throw std::runtime_error(
                "TCP send failed: connection closed by peer"
            );
        }

        sent += static_cast<std::size_t>(written);
    }
}

void set_no_sigpipe(int descriptor) {
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    (void)::setsockopt(
        descriptor,
        SOL_SOCKET,
        SO_NOSIGPIPE,
        &enabled,
        sizeof(enabled)
    );
#else
    (void)descriptor;
#endif
}

std::string resolve_error(const std::string& host, std::uint16_t port, int code) {
    return "Could not resolve " +
           format_tcp_endpoint({host, port}) + ": " + gai_strerror(code);
}

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

// Reads at least one byte, waiting until `deadline` when set. The frame loops
// below repeat this, so the deadline covers the whole frame.
std::size_t read_some_until(
    int descriptor,
    char* data,
    std::size_t size,
    const Deadline& deadline
) {
    for (;;) {
        if (deadline) {
            const auto now = std::chrono::steady_clock::now();

            if (now >= *deadline) {
                throw TimeoutError("TCP receive timed out");
            }

            // Round up to avoid cutting the final sub-millisecond short.
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    *deadline - now
                );
            const auto rounded_ms = (remaining.count() + 999) / 1000;
            const int wait_ms = rounded_ms > std::numeric_limits<int>::max()
                ? std::numeric_limits<int>::max()
                : static_cast<int>(rounded_ms);

            pollfd descriptor_info{};
            descriptor_info.fd = descriptor;
            descriptor_info.events = POLLIN;

            int ready = 0;

            do {
                ready = ::poll(&descriptor_info, 1, wait_ms);
            } while (ready < 0 && errno == EINTR);

            if (ready < 0) {
                throw_system_error("TCP poll failed", errno);
            }

            if (ready == 0) {
                throw TimeoutError("TCP receive timed out");
            }
        }

        const ssize_t received = ::recv(descriptor, data, size, 0);

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received < 0) {
            throw_system_error("TCP receive failed", errno);
        }

        return static_cast<std::size_t>(received);
    }
}

std::optional<std::string> receive_frame_until(
    int descriptor,
    const Deadline& deadline
) {
    unsigned char header[4] = {0, 0, 0, 0};
    std::size_t header_bytes = 0;

    while (header_bytes < sizeof(header)) {
        const std::size_t received = read_some_until(
            descriptor,
            reinterpret_cast<char*>(header) + header_bytes,
            sizeof(header) - header_bytes,
            deadline
        );

        if (received == 0) {
            if (header_bytes == 0) {
                return std::nullopt;
            }

            throw std::runtime_error(
                "TCP connection closed in the middle of a frame header"
            );
        }

        header_bytes += received;
    }

    const std::uint32_t length =
        (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) |
        static_cast<std::uint32_t>(header[3]);

    if (length == 0) {
        throw std::runtime_error("Received an empty TCP frame");
    }

    if (length > kMaxFrameBytes) {
        throw std::runtime_error(
            "Received TCP frame of " + std::to_string(length) +
            " bytes, exceeding the " +
            std::to_string(kMaxFrameBytes) + "-byte limit"
        );
    }

    std::string payload(length, '\0');
    std::size_t payload_bytes = 0;

    while (payload_bytes < payload.size()) {
        const std::size_t received = read_some_until(
            descriptor,
            payload.data() + payload_bytes,
            payload.size() - payload_bytes,
            deadline
        );

        if (received == 0) {
            throw std::runtime_error(
                "TCP connection closed in the middle of a frame"
            );
        }

        payload_bytes += received;
    }

    return payload;
}

}  // namespace

TcpEndpoint parse_tcp_endpoint(const std::string& text) {
    if (text.empty()) {
        throw std::invalid_argument("TCP endpoint must not be empty");
    }

    std::string host;
    std::string port_text;

    if (text.front() == '[') {
        const std::size_t close = text.find(']');

        if (close == std::string::npos ||
            close + 1 >= text.size() ||
            text[close + 1] != ':') {
            throw std::invalid_argument(
                "Expected '[IPv6]:port' in TCP endpoint '" + text + "'"
            );
        }

        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const std::size_t colon = text.rfind(':');

        if (colon == std::string::npos) {
            throw std::invalid_argument(
                "Expected 'host:port' in TCP endpoint '" + text + "'"
            );
        }

        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);

        if (host.find(':') != std::string::npos) {
            throw std::invalid_argument(
                "Bracket IPv6 hosts, for example '[::1]:9000'"
            );
        }
    }

    if (port_text.empty()) {
        throw std::invalid_argument(
            "TCP endpoint '" + text + "' is missing a port"
        );
    }

    unsigned long port = 0;

    for (char c : port_text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            throw std::invalid_argument(
                "TCP port must be numeric in endpoint '" + text + "'"
            );
        }
    }

    try {
        port = std::stoul(port_text);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            "TCP port is out of range in endpoint '" + text + "'"
        );
    }

    if (port > 65535UL) {
        throw std::invalid_argument(
            "TCP port is out of range in endpoint '" + text + "'"
        );
    }

    return {host, static_cast<std::uint16_t>(port)};
}

std::string format_tcp_endpoint(const TcpEndpoint& endpoint) {
    const std::string port = std::to_string(endpoint.port);

    if (endpoint.host.find(':') != std::string::npos) {
        return "[" + endpoint.host + "]:" + port;
    }

    return endpoint.host + ":" + port;
}

TcpSocket::TcpSocket(int descriptor) : descriptor_(descriptor) {
    if (descriptor_ >= 0) {
        set_no_sigpipe(descriptor_);
    }
}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : descriptor_(other.descriptor_) {
    other.descriptor_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
    }

    return *this;
}

bool TcpSocket::valid() const noexcept {
    return descriptor_ >= 0;
}

int TcpSocket::descriptor() const noexcept {
    return descriptor_;
}

void TcpSocket::close() noexcept {
    if (descriptor_ >= 0) {
        (void)::close(descriptor_);
        descriptor_ = -1;
    }
}

void TcpSocket::send_frame(const std::string& payload) {
    if (!valid()) {
        throw std::runtime_error("TCP socket is not connected");
    }

    if (payload.empty()) {
        throw std::invalid_argument("TCP frames must not be empty");
    }

    if (payload.size() > kMaxFrameBytes) {
        throw std::length_error(
            "TCP frame exceeds " + std::to_string(kMaxFrameBytes) +
            " bytes"
        );
    }

    const std::uint32_t length =
        static_cast<std::uint32_t>(payload.size());

    std::string frame;
    frame.resize(4 + payload.size());
    frame[0] = static_cast<char>((length >> 24) & 0xFFU);
    frame[1] = static_cast<char>((length >> 16) & 0xFFU);
    frame[2] = static_cast<char>((length >> 8) & 0xFFU);
    frame[3] = static_cast<char>(length & 0xFFU);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());

    send_all(descriptor_, frame.data(), frame.size());
}

std::optional<std::string> TcpSocket::receive_frame() {
    if (!valid()) {
        throw std::runtime_error("TCP socket is not connected");
    }

    return receive_frame_until(descriptor_, std::nullopt);
}

std::optional<std::string> TcpSocket::receive_frame_for(
    std::chrono::milliseconds timeout
) {
    if (!valid()) {
        throw std::runtime_error("TCP socket is not connected");
    }

    return receive_frame_until(
        descriptor_,
        std::chrono::steady_clock::now() + timeout
    );
}

bool TcpSocket::peer_closed() const noexcept {
    if (!valid()) {
        return true;
    }

    pollfd descriptor{};
    descriptor.fd = descriptor_;
    descriptor.events = POLLIN;

    int ready = 0;

    do {
        ready = ::poll(&descriptor, 1, 0);
    } while (ready < 0 && errno == EINTR);

    if (ready < 0) {
        // Without a usable descriptor there is no way to receive a reply.
        return true;
    }

    if (ready == 0) {
        return false;
    }

    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        return (descriptor.revents & POLLNVAL) != 0;
    }

    // Peek so a readable socket is not mistaken for a close. A return value of
    // zero is an orderly shutdown; a pending byte means data is waiting.
    char byte = 0;
    ssize_t peeked = 0;

    do {
        peeked = ::recv(descriptor_, &byte, 1, MSG_PEEK);
    } while (peeked < 0 && errno == EINTR);

    if (peeked == 0) {
        return true;
    }

    if (peeked > 0) {
        return false;
    }

    // EAGAIN means the peer is still connected even though poll was readable.
    // Any other error (for example ECONNRESET) makes the connection unusable.
    return errno != EAGAIN && errno != EWOULDBLOCK;
}

TcpListener::TcpListener(int descriptor) : descriptor_(descriptor) {}

TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : descriptor_(other.descriptor_) {
    other.descriptor_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
    }

    return *this;
}

TcpListener TcpListener::bind(
    const std::string& host,
    std::uint16_t port,
    int backlog
) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int resolved = ::getaddrinfo(
        host.empty() ? nullptr : host.c_str(),
        service.c_str(),
        &hints,
        &addresses
    );

    if (resolved != 0) {
        throw std::runtime_error(resolve_error(host, port, resolved));
    }

    std::string last_error = "no usable address";

    for (addrinfo* address = addresses;
         address != nullptr;
         address = address->ai_next) {
        const int descriptor = ::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol
        );

        if (descriptor < 0) {
            last_error = std::strerror(errno);
            continue;
        }

        const int enabled = 1;
        (void)::setsockopt(
            descriptor,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            sizeof(enabled)
        );

        if (::bind(descriptor, address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(descriptor, backlog) == 0) {
            ::freeaddrinfo(addresses);
            return TcpListener(descriptor);
        }

        last_error = std::strerror(errno);
        (void)::close(descriptor);
    }

    ::freeaddrinfo(addresses);

    throw std::runtime_error(
        "Could not bind " + format_tcp_endpoint({host, port}) +
        ": " + last_error
    );
}

bool TcpListener::valid() const noexcept {
    return descriptor_ >= 0;
}

int TcpListener::descriptor() const noexcept {
    return descriptor_;
}

std::uint16_t TcpListener::port() const {
    if (!valid()) {
        throw std::runtime_error("TCP listener is not bound");
    }

    sockaddr_storage address{};
    socklen_t length = sizeof(address);

    if (::getsockname(
            descriptor_,
            reinterpret_cast<sockaddr*>(&address),
            &length
        ) != 0) {
        throw_system_error("Could not read the listener port", errno);
    }

    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        return ntohs(ipv4->sin_port);
    }

    if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        return ntohs(ipv6->sin6_port);
    }

    throw std::runtime_error("Listener has an unsupported address family");
}

TcpSocket TcpListener::accept() {
    if (!valid()) {
        throw std::runtime_error("TCP listener is not bound");
    }

    for (;;) {
        const int descriptor = ::accept(descriptor_, nullptr, nullptr);

        if (descriptor >= 0) {
            return TcpSocket(descriptor);
        }

        if (errno == EINTR) {
            continue;
        }

        throw_system_error("TCP accept failed", errno);
    }
}

void TcpListener::close() noexcept {
    if (descriptor_ >= 0) {
        (void)::close(descriptor_);
        descriptor_ = -1;
    }
}

TcpSocket connect_to(const std::string& host, std::uint16_t port) {
    if (host.empty()) {
        throw std::invalid_argument("TCP connect requires a host");
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int resolved = ::getaddrinfo(
        host.c_str(),
        service.c_str(),
        &hints,
        &addresses
    );

    if (resolved != 0) {
        throw std::runtime_error(resolve_error(host, port, resolved));
    }

    std::string last_error = "no usable address";

    for (addrinfo* address = addresses;
         address != nullptr;
         address = address->ai_next) {
        const int descriptor = ::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol
        );

        if (descriptor < 0) {
            last_error = std::strerror(errno);
            continue;
        }

        if (::connect(descriptor, address->ai_addr, address->ai_addrlen) == 0) {
            ::freeaddrinfo(addresses);
            return TcpSocket(descriptor);
        }

        last_error = std::strerror(errno);
        (void)::close(descriptor);
    }

    ::freeaddrinfo(addresses);

    throw std::runtime_error(
        "Could not connect to " + format_tcp_endpoint({host, port}) +
        ": " + last_error
    );
}

}  // namespace net
