#include "comm/message_channel.h"
#include "comm/tcp_channel.h"
#include "net/tcp.h"
#include "worker/worker.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// The worker-facing transport name is the common communication contract.
static_assert(
    std::is_same<WorkerTransport, comm::MessageChannel>::value,
    "WorkerTransport must be the shared MessageChannel contract"
);

void test_tcp_channel_round_trip() {
    net::TcpListener listener =
        net::TcpListener::bind("127.0.0.1", 0, 1);

    std::string received;
    bool saw_eof = false;
    std::exception_ptr server_error;

    std::thread server([&] {
        try {
            comm::TcpChannel channel(listener.accept());

            const auto request = channel.receive();
            if (!request) {
                throw std::runtime_error("expected one channel message");
            }
            received = *request;

            channel.send("world");
            saw_eof = !channel.receive().has_value();
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    std::unique_ptr<comm::MessageChannel> client =
        std::make_unique<comm::TcpClientChannel>(
            "127.0.0.1",
            listener.port()
        );

    client->connect();
    client->send("hello");

    const auto reply = client->receive();
    expect(
        reply.has_value() && *reply == "world",
        "client must receive the server reply through the channel"
    );

    client->close();
    server.join();

    if (server_error) {
        std::rethrow_exception(server_error);
    }

    expect(received == "hello", "server must receive the client message");
    expect(saw_eof, "a clean close must surface as nullopt");
}

}  // namespace

int main() {
    try {
        test_tcp_channel_round_trip();
        std::cout << "communication_channel_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "communication_channel_tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
