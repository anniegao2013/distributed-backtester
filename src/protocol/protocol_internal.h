#pragma once

#include "protocol/protocol.h"
#include "core/json_value.h"

#include <cstddef>
#include <string>

// Private declarations shared by the protocol implementation files.
namespace protocol_detail {

inline constexpr const char* kRootPath = "$";
inline constexpr std::size_t kMaxIdentifierLength = 128;

[[noreturn]] void fail(
    const std::string& path,
    const std::string& message
);

// Validates a correlation or worker identifier.
void validate_message_id(
    const std::string& value,
    const std::string& path
);

void validate_worker_id(
    const std::string& value,
    const std::string& path
);

// Full semantic validation of a message. Called by both the parsers and the
// serializers so an invalid C++ value can never be written to the wire.
void validate_job_message(const BacktestJobMessage& message);
void validate_result_message(const BacktestResultMessage& message);
void validate_error_message(const ProtocolErrorMessage& message);

// Document parsers shared by the type-specific entry points and the
// dispatcher. Each expects an already-parsed root object.
BacktestJobMessage parse_job_message_document(const JsonValue& root);
BacktestResultMessage parse_result_message_document(const JsonValue& root);
ProtocolErrorMessage parse_error_message_document(const JsonValue& root);

}  // namespace protocol_detail
