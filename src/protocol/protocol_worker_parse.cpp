#include "protocol_schema.h"

#include <string>

namespace {

using protocol_detail::allow_schema_reference;
using protocol_detail::expect_object;
using protocol_detail::fail;
using protocol_detail::kRootPath;
using protocol_detail::reject_unknown_members;
using protocol_detail::require_member;
using protocol_detail::require_message_type;
using protocol_detail::require_protocol_version;
using protocol_detail::require_string;

WorkerErrorDetail parse_worker_error_detail(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(value, {"type", "path", "message"}, path);

    WorkerErrorDetail error;
    error.type = require_string(
        require_member(value, "type", path),
        path + ".type"
    );
    error.message = require_string(
        require_member(value, "message", path),
        path + ".message"
    );

    if (value.contains("path")) {
        error.path = require_string(
            value.at("path"),
            path + ".path"
        );
    }

    return error;
}

void require_worker_error_type(const JsonValue& root) {
    const std::string type = require_string(
        require_member(root, "type", kRootPath),
        std::string(kRootPath) + ".type"
    );

    if (type != "worker_error" && type != "protocol_error") {
        fail(
            std::string(kRootPath) + ".type",
            "Expected message type 'worker_error' (or legacy "
            "'protocol_error'), got '" + type + "'"
        );
    }
}

}  // namespace

namespace protocol_detail {

WorkerReadyMessage parse_worker_ready_message_document(
    const JsonValue& root
) {
    expect_object(root, kRootPath);
    allow_schema_reference(root);

    WorkerReadyMessage message;
    message.protocol_version = require_protocol_version(root);
    require_message_type(root, "worker_ready");
    reject_unknown_members(
        root,
        {
            "$schema",
            "protocol_version",
            "type",
            "message_id",
            "worker_id"
        },
        kRootPath
    );

    message.message_id = require_string(
        require_member(root, "message_id", kRootPath),
        std::string(kRootPath) + ".message_id"
    );
    message.worker_id = require_string(
        require_member(root, "worker_id", kRootPath),
        std::string(kRootPath) + ".worker_id"
    );

    validate_worker_ready_message(message);

    return message;
}

WorkerErrorMessage parse_worker_error_message_document(
    const JsonValue& root
) {
    expect_object(root, kRootPath);
    allow_schema_reference(root);

    WorkerErrorMessage message;
    message.protocol_version = require_protocol_version(root);
    require_worker_error_type(root);
    reject_unknown_members(
        root,
        {
            "$schema",
            "protocol_version",
            "type",
            "message_id",
            "worker_id",
            "error"
        },
        kRootPath
    );

    if (root.contains("message_id")) {
        message.message_id = require_string(
            root.at("message_id"),
            std::string(kRootPath) + ".message_id"
        );

        if (message.message_id.empty()) {
            fail(
                std::string(kRootPath) + ".message_id",
                "message_id must not be empty when present"
            );
        }
    }

    if (root.contains("worker_id")) {
        message.worker_id = require_string(
            root.at("worker_id"),
            std::string(kRootPath) + ".worker_id"
        );

        if (message.worker_id.empty()) {
            fail(
                std::string(kRootPath) + ".worker_id",
                "worker_id must not be empty when present"
            );
        }
    }

    message.error = parse_worker_error_detail(
        require_member(root, "error", kRootPath),
        std::string(kRootPath) + ".error"
    );

    validate_worker_error_message(message);

    return message;
}

}  // namespace protocol_detail

WorkerReadyMessage parse_worker_ready_json(const std::string& text) {
    return protocol_detail::parse_worker_ready_message_document(
        parse_json(text)
    );
}

WorkerErrorMessage parse_worker_error_json(const std::string& text) {
    return protocol_detail::parse_worker_error_message_document(
        parse_json(text)
    );
}

ProtocolErrorMessage parse_protocol_error_json(const std::string& text) {
    return parse_worker_error_json(text);
}
