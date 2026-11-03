#include "comm/tcp_channel.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace comm {

TcpChannel::TcpChannel(net::TcpSocket socket)
    : socket_(std::move(socket)) {}

bool TcpChannel::connected() const noexcept {
    return socket_.valid();
}

std::optional<std::string> TcpChannel::receive() {
    if (!socket_.valid()) {
        throw std::runtime_error(
            "TcpChannel::receive() called before connect()"
        );
    }

    return socket_.receive_frame();
}

void TcpChannel::send(const std::string& message) {
    if (!socket_.valid()) {
        throw std::runtime_error(
            "TcpChannel::send() called before connect()"
        );
    }

    socket_.send_frame(message);
}

void TcpChannel::close() {
    socket_.close();
}

TcpClientChannel::TcpClientChannel(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {}

void TcpClientChannel::connect() {
    if (channel_.connected()) {
        return;
    }

    channel_ = TcpChannel(net::connect_to(host_, port_));
}

std::optional<std::string> TcpClientChannel::receive() {
    return channel_.receive();
}

void TcpClientChannel::send(const std::string& message) {
    channel_.send(message);
}

void TcpClientChannel::close() {
    channel_.close();
}

}  // namespace comm
