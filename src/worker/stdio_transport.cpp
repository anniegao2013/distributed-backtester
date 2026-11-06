#include "worker/stdio_transport.h"

#include <cstddef>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>

namespace {

// A single line must fit comfortably in memory; oversized input is treated as
// a transport error rather than an allocation.
constexpr std::size_t kMaxMessageBytes = 8 * 1024 * 1024;

}  // namespace

StdioTransport::StdioTransport(
    std::istream& input,
    std::ostream& output
)
    : input_(input), output_(output) {}

void StdioTransport::connect() {
    if (!input_.good() || !output_.good()) {
        throw std::runtime_error(
            "StdioTransport requires readable input and writable output"
        );
    }
}

std::optional<std::string> StdioTransport::receive() {
    std::string line;

    while (std::getline(input_, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            continue;
        }

        if (line.size() > kMaxMessageBytes) {
            throw std::runtime_error(
                "StdioTransport message exceeds " +
                    std::to_string(kMaxMessageBytes) + " bytes"
            );
        }

        return line;
    }

    if (input_.bad()) {
        throw std::runtime_error("StdioTransport failed to read input");
    }

    return std::nullopt;
}

void StdioTransport::send(const std::string& message) {
    if (message.empty()) {
        throw std::runtime_error("StdioTransport cannot send an empty message");
    }

    if (message.find('\n') != std::string::npos ||
        message.find('\r') != std::string::npos) {
        throw std::runtime_error(
            "StdioTransport frames messages by line; serialize compact JSON"
        );
    }

    output_ << message << '\n';
    output_.flush();

    if (!output_) {
        throw std::runtime_error("StdioTransport failed to write output");
    }
}
