#pragma once

#include "worker/worker.h"

#include <istream>
#include <ostream>

// Development transport: newline-delimited JSON over a pair of streams.
//
// Each line carries exactly one compact JSON document. This exists so the
// worker executable can be exercised end-to-end before the TCP adapter is
// written; it is not the production transport. Because messages are line
// framed, documents must not contain raw newlines in this mode. Serializers
// called with pretty = false satisfy that requirement.
class StdioTransport : public WorkerTransport {
public:
    StdioTransport(std::istream& input, std::ostream& output);

    // Verifies both streams are usable. There is no handshake.
    void connect() override;

    // Reads one non-empty line. Blank lines are skipped. Returns std::nullopt
    // at end of input.
    std::optional<std::string> receive() override;

    // Writes the message followed by a newline and flushes.
    void send(const std::string& message) override;

private:
    std::istream& input_;
    std::ostream& output_;
};
