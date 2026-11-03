#include "worker/tcp_transport.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

TcpTransport::TcpTransport(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {}

void TcpTransport::connect() {
    if (socket_.valid()) {
        return;
    }

    socket_ = net::connect_to(host_, port_);
}

std::optional<std::string> TcpTransport::receive() {
    if (!socket_.valid()) {
        throw std::runtime_error(
            "TcpTransport::receive() called before connect()"
        );
    }

    return socket_.receive_frame();
}

void TcpTransport::send(const std::string& message) {
    if (!socket_.valid()) {
        throw std::runtime_error(
            "TcpTransport::send() called before connect()"
        );
    }

    socket_.send_frame(message);
}
