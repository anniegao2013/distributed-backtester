#include "protocol_schema.h"

#include <string>

using protocol_detail::expect_object;
using protocol_detail::fail;
using protocol_detail::kRootPath;
using protocol_detail::require_member;
using protocol_detail::require_protocol_version;
using protocol_detail::require_protocol_version_number;
using protocol_detail::require_string;

ProtocolEnvelope parse_protocol_envelope(const std::string& text) {
    const JsonValue root = parse_json(text);
    expect_object(root, kRootPath);

    ProtocolEnvelope envelope;
    envelope.protocol_version = require_protocol_version_number(root);
    envelope.type = require_string(
        require_member(root, "type", kRootPath),
        std::string(kRootPath) + ".type"
    );

    if (envelope.type.empty()) {
        fail(
            std::string(kRootPath) + ".type",
            "Message type must not be empty"
        );
    }

    if (root.contains("message_id")) {
        envelope.message_id = require_string(
            root.at("message_id"),
            std::string(kRootPath) + ".message_id"
        );

        protocol_detail::validate_message_id(
            envelope.message_id,
            std::string(kRootPath) + ".message_id"
        );
    }

    return envelope;
}

ProtocolMessage parse_protocol_message_json(const std::string& text) {
    const JsonValue root = parse_json(text);
    expect_object(root, kRootPath);
    (void)require_protocol_version(root);

    const std::string type = require_string(
        require_member(root, "type", kRootPath),
        std::string(kRootPath) + ".type"
    );

    if (type == "backtest_job") {
        return protocol_detail::parse_job_message_document(root);
    }

    if (type == "backtest_result") {
        return protocol_detail::parse_result_message_document(root);
    }

    if (type == "protocol_error") {
        return protocol_detail::parse_error_message_document(root);
    }

    fail(
        std::string(kRootPath) + ".type",
        "Unknown message type '" + type +
            "'; expected 'backtest_job', 'backtest_result', "
            "or 'protocol_error'"
    );
}
