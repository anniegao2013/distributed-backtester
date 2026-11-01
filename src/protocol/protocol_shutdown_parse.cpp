#include "protocol_schema.h"

#include <string>

namespace {

using protocol_detail::allow_schema_reference;
using protocol_detail::expect_object;
using protocol_detail::kRootPath;
using protocol_detail::reject_unknown_members;
using protocol_detail::require_member;
using protocol_detail::require_message_type;
using protocol_detail::require_protocol_version;
using protocol_detail::require_string;

}  // namespace

namespace protocol_detail {

ShutdownMessage parse_shutdown_message_document(const JsonValue& root) {
    expect_object(root, kRootPath);
    allow_schema_reference(root);

    ShutdownMessage message;
    message.protocol_version = require_protocol_version(root);
    require_message_type(root, "shutdown");
    reject_unknown_members(
        root,
        {
            "$schema",
            "protocol_version",
            "type",
            "message_id",
            "reason"
        },
        kRootPath
    );

    message.message_id = require_string(
        require_member(root, "message_id", kRootPath),
        std::string(kRootPath) + ".message_id"
    );

    if (root.contains("reason")) {
        message.reason = require_string(
            root.at("reason"),
            std::string(kRootPath) + ".reason"
        );

        if (message.reason.empty()) {
            fail(
                std::string(kRootPath) + ".reason",
                "reason must not be empty when present"
            );
        }
    }

    validate_shutdown_message(message);

    return message;
}

}  // namespace protocol_detail

ShutdownMessage parse_shutdown_json(const std::string& text) {
    return protocol_detail::parse_shutdown_message_document(parse_json(text));
}
