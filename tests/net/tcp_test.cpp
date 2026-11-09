#include "net/tcp.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_endpoint_parsing() {
    const net::TcpEndpoint ipv4 =
        net::parse_tcp_endpoint("127.0.0.1:9000");
    expect(ipv4.host == "127.0.0.1", "IPv4 host must parse");
    expect(ipv4.port == 9000, "IPv4 port must parse");

    const net::TcpEndpoint wildcard =
        net::parse_tcp_endpoint(":0");
    expect(wildcard.host.empty(), "an empty host must parse");
    expect(wildcard.port == 0, "port zero must parse");

    const net::TcpEndpoint ipv6 =
        net::parse_tcp_endpoint("[::1]:8080");
    expect(ipv6.host == "::1", "IPv6 host must parse");
    expect(ipv6.port == 8080, "IPv6 port must parse");
    expect(
        net::format_tcp_endpoint(ipv6) == "[::1]:8080",
        "IPv6 hosts must be bracketed when formatted"
    );

    bool threw = false;
    try {
        (void)net::parse_tcp_endpoint("missing-port");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "an endpoint without a port must be rejected");

    threw = false;
    try {
        (void)net::parse_tcp_endpoint("127.0.0.1:70000");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "an out-of-range port must be rejected");
}

void test_large_frame_round_trip() {
    net::TcpListener listener =
        net::TcpListener::bind("127.0.0.1", 0, 1);
    const std::uint16_t port = listener.port();

    const std::string large(1024 * 1024, 'x');
    std::string received;
    bool saw_eof = false;
    std::exception_ptr server_error;

    std::thread server([&] {
        try {
            net::TcpSocket socket = listener.accept();

            const auto first = socket.receive_frame();
            if (!first) {
                throw std::runtime_error("expected the first frame");
            }
            received = *first;

            socket.send_frame("ack");

            const auto closed = socket.receive_frame();
            saw_eof = !closed.has_value();
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    net::TcpSocket client =
        net::connect_to("127.0.0.1", port);
    client.send_frame(large);

    const auto reply = client.receive_frame();
    expect(
        reply.has_value() && *reply == "ack",
        "client must receive the server's frame"
    );

    client.close();
    server.join();

    if (server_error) {
        std::rethrow_exception(server_error);
    }

    expect(
        received == large,
        "a large frame must survive partial reads and writes"
    );
    expect(saw_eof, "a clean close between frames must be reported as EOF");
}

void test_peer_closed_detection() {
    net::TcpListener listener =
        net::TcpListener::bind("127.0.0.1", 0, 1);
    const std::uint16_t port = listener.port();

    net::TcpSocket client = net::connect_to("127.0.0.1", port);
    net::TcpSocket server = listener.accept();

    expect(
        !server.peer_closed(),
        "a connected peer must not be reported as closed"
    );

    client.send_frame("ping");

    expect(
        !server.peer_closed(),
        "pending data must not be mistaken for a closed peer"
    );

    const auto frame = server.receive_frame();
    expect(frame.has_value() && *frame == "ping", "the frame must arrive");

    client.close();

    bool closed = false;

    for (int attempt = 0; attempt < 100 && !closed; ++attempt) {
        closed = server.peer_closed();

        if (!closed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    expect(
        closed,
        "a peer close must be detected without a blocking read"
    );
}

}  // namespace

int main() {
    try {
        test_endpoint_parsing();
        test_large_frame_round_trip();
        test_peer_closed_detection();
        std::cout << "tcp_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tcp_tests failed: " << error.what() << '\n';
        return 1;
    }
}
