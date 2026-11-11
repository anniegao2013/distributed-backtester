#pragma once

#include "comm/message_channel.h"
#include "net/tcp.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace comm {

// A MessageChannel over an already-connected TCP socket. The coordinator uses
// this after accept(); tests can use it with any connected net::TcpSocket.
class TcpChannel : public MessageChannel {
public:
    TcpChannel() = default;
    explicit TcpChannel(net::TcpSocket socket);

    TcpChannel(TcpChannel&&) noexcept = default;
    TcpChannel& operator=(TcpChannel&&) noexcept = default;

    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;

    bool connected() const noexcept;

    // Non-blocking liveness check. Returns true when the peer has closed or
    // reset the connection, or when the socket is no longer usable.
    bool peer_closed() const noexcept;

    std::optional<std::string> receive() override;

    // Like receive(), but the document must arrive within `timeout`.
    std::optional<std::string> receive_for(
        std::chrono::milliseconds timeout
    );

    void send(const std::string& message) override;
    void close() override;

private:
    net::TcpSocket socket_;
};

// The worker side of the TCP communication layer. connect() resolves and dials
// the coordinator; the remaining operations delegate to an owned TcpChannel.
class TcpClientChannel : public MessageChannel {
public:
    TcpClientChannel(std::string host, std::uint16_t port);

    // Dials the coordinator. Throws if the connection cannot be established.
    void connect() override;

    std::optional<std::string> receive() override;
    void send(const std::string& message) override;
    void close() override;

private:
    std::string host_;
    std::uint16_t port_;
    TcpChannel channel_;
};

}  // namespace comm
