#pragma once

#include <optional>
#include <string>

// Transport-neutral communication boundary between the coordinator and workers.
//
// A MessageChannel owns one bidirectional connection and carries exactly one
// UTF-8 JSON document per send()/receive(). It is intentionally not aware of
// protocol message types: parsing, validation, correlation, retries, and
// scheduling belong to the layers above it.
//
// Implementations own framing. For TCP that is a 4-byte big-endian payload
// length followed by the JSON bytes; for stdio it is one compact JSON document
// per line. Both directions preserve the one-call-one-document contract.
namespace comm {

class MessageChannel {
public:
    virtual ~MessageChannel() = default;

    // Establishes the connection and performs any transport handshake. The
    // default is a no-op for channels that are already connected when
    // constructed, such as a socket accepted by the coordinator.
    virtual void connect() {}

    // Blocks until the next complete document is available. Returns
    // std::nullopt when the peer closes the connection cleanly between
    // messages. A close in the middle of a frame, malformed framing, or an
    // oversized message is a transport error and is reported by throwing.
    virtual std::optional<std::string> receive() = 0;

    // Sends one complete document. Must not return until the document has been
    // handed to the transport.
    virtual void send(const std::string& message) = 0;

    // Releases the connection. Implementations may make this a no-op when the
    // destructor owns the underlying resource.
    virtual void close() {}
};

}  // namespace comm
