#pragma once

#include "net/tcp.h"
#include "worker/worker.h"

#include <cstdint>
#include <optional>
#include <string>

// Production worker transport: the worker is the TCP client.
//
// Framing is a 4-byte big-endian payload length followed by one UTF-8 JSON
// document; see net/tcp.h. connect() dials the coordinator, receive() returns
// one complete document, and send() writes exactly one.
class TcpTransport : public WorkerTransport {
public:
    TcpTransport(std::string host, std::uint16_t port);

    // Dials the coordinator. Throws if the connection cannot be established.
    void connect() override;

    // Returns one complete message, or std::nullopt when the coordinator
    // closes the connection cleanly between messages.
    std::optional<std::string> receive() override;

    // Sends one complete message.
    void send(const std::string& message) override;

private:
    std::string host_;
    std::uint16_t port_;
    net::TcpSocket socket_;
};
