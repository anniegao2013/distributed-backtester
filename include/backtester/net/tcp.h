#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Minimal blocking TCP helpers used by the coordinator and workers.
//
// A connection carries length-prefixed frames: a 4-byte unsigned big-endian
// payload length followed by exactly that many bytes of UTF-8 JSON. Framing is
// independent of the message schema, so the protocol layer stays
// transport-neutral. Every frame must be non-empty and no larger than
// kMaxFrameBytes.
namespace net {

inline constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;

struct TcpEndpoint {
    std::string host;
    std::uint16_t port = 0;
};

// Parses "HOST:PORT", ":PORT", or "[IPv6]:PORT". Throws std::invalid_argument
// for malformed endpoints or a port outside [0, 65535].
TcpEndpoint parse_tcp_endpoint(const std::string& text);

// Formats an endpoint, bracketing IPv6 hosts.
std::string format_tcp_endpoint(const TcpEndpoint& endpoint);

// An owned connected socket.
class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(int descriptor);
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool valid() const noexcept;
    int descriptor() const noexcept;

    void close() noexcept;

    // Writes one length-prefixed frame, retrying partial writes.
    void send_frame(const std::string& payload);

    // Blocks until one complete frame arrives. Returns std::nullopt when the
    // peer closes the connection cleanly between frames. Throws if the peer
    // closes mid-frame, the frame is empty, or the frame exceeds the limit.
    std::optional<std::string> receive_frame();

private:
    int descriptor_ = -1;
};

// An owned listening socket.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Resolves HOST and binds every candidate address until one succeeds.
    // An empty host binds all interfaces. Port 0 selects an ephemeral port.
    static TcpListener bind(
        const std::string& host,
        std::uint16_t port,
        int backlog = 64
    );

    bool valid() const noexcept;
    int descriptor() const noexcept;

    // Returns the actual local port, which is useful when binding port 0.
    std::uint16_t port() const;

    // Blocks until a peer connects.
    TcpSocket accept();

    void close() noexcept;

private:
    explicit TcpListener(int descriptor);

    int descriptor_ = -1;
};

// Connects to HOST:PORT, trying each resolved address in turn.
TcpSocket connect_to(const std::string& host, std::uint16_t port);

}  // namespace net
