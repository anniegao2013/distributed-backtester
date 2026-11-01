#include "worker/stdio_transport.h"
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
    "Runs a backtest worker. Reads one compact JSON protocol message per\n"
    "line from stdin, runs up to one backtest per message, and writes one\n"
    "JSON reply per line to stdout. Diagnostics go to stderr.\n"
    "\n"
    "This is the local development transport. Coordinator/worker TCP is not\n"
    "implemented yet.\n"
    "\n"
    "Options:\n"
    "  -w, --worker-id ID  Identity echoed in every result.\n"
    "                      Default: worker-local.\n"
    "  -h, --help          Show this help.\n"
    "\n"
    "The process exits cleanly at end of input.\n";

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

        StdioTransport transport(std::cin, std::cout);
        (void)run_worker(transport, config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Worker error: " << error.what() << '\n';
        return 1;
    }
}
