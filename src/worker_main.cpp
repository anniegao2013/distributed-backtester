#include "net/tcp.h"
#include "worker/stdio_transport.h"
#include "worker/tcp_transport.h"
#include "worker/worker.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kUsage =
    "Usage: backtester_worker [options]\n"
    "\n"
    "Runs a backtest worker. Announces worker_ready, then reads one protocol\n"
    "message at a time, runs up to one backtest per job, and replies with one\n"
    "JSON message. Diagnostics go to stderr.\n"
    "\n"
    "By default messages are one compact JSON document per line on stdin and\n"
    "stdout. With --connect the worker is a TCP client of a coordinator and\n"
    "uses length-prefixed frames.\n"
    "\n"
    "Options:\n"
    "  -w, --worker-id ID     Identity used in worker_ready, results, and\n"
    "                         errors. Default: worker-local.\n"
    "  -c, --connect HOST:PORT\n"
    "                         Connect to a coordinator over TCP instead of\n"
    "                         using stdio.\n"
    "  -h, --help             Show this help.\n"
    "\n"
    "The process exits cleanly at end of input, after a shutdown, or when the\n"
    "coordinator closes the connection.\n";

bool is_ascii_alphanumeric(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// Matches the protocol's opaque-identifier rules for worker_id.
bool is_valid_worker_id(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];

        if (!is_ascii_alphanumeric(c) &&
            c != '.' && c != '_' && c != '-' && c != ':') {
            return false;
        }

        if (i == 0 && !is_ascii_alphanumeric(c)) {
            return false;
        }
    }

    return true;
}

struct Options {
    std::string worker_id = "worker-local";
    bool connect = false;
    net::TcpEndpoint coordinator;
    bool help = false;
};

Options parse_options(int argc, char* argv[]) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "-w" || argument == "--worker-id") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    argument + " requires an identifier"
                );
            }

            options.worker_id = argv[++i];
        } else if (argument == "-c" || argument == "--connect") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    argument + " requires HOST:PORT"
                );
            }

            options.connect = true;
            options.coordinator =
                net::parse_tcp_endpoint(argv[++i]);
        } else {
            throw std::invalid_argument(
                "Unknown argument '" + argument + "'"
            );
        }
    }

    if (!options.help && !is_valid_worker_id(options.worker_id)) {
        throw std::invalid_argument(
            "Invalid worker id '" + options.worker_id +
            "'; use letters, digits, dots, underscores, hyphens, or colons, "
            "starting with a letter or digit"
        );
    }

    if (!options.help && options.connect) {
        if (options.coordinator.host.empty()) {
            throw std::invalid_argument(
                "--connect requires a host, for example 127.0.0.1:9000"
            );
        }

        if (options.coordinator.port == 0) {
            throw std::invalid_argument(
                "--connect requires a non-zero port"
            );
        }
    }

    return options;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);

        if (options.help) {
            std::cout << kUsage;
            return 0;
        }

        WorkerConfig config;
        config.worker_id = options.worker_id;
        config.log = [](const std::string& message) {
            std::cerr << "[worker] " << message << '\n';
        };

        if (options.connect) {
            TcpTransport transport(
                options.coordinator.host,
                options.coordinator.port
            );
            (void)run_worker(transport, config);
        } else {
            StdioTransport transport(std::cin, std::cout);
            (void)run_worker(transport, config);
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Worker error: " << error.what() << '\n';
        return 1;
    }
}
