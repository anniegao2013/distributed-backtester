#pragma once

#include "protocol_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

// Small JSON-schema helpers shared by the protocol parsers. They throw
// ProtocolValidationError at a JSONPath-like location.
namespace protocol_detail {

inline void expect_object(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_object()) {
        fail(
            path,
            std::string("Expected an object, got ") + value.type_name()
        );
    }
}

inline void expect_array(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_array()) {
        fail(
            path,
            std::string("Expected an array, got ") + value.type_name()
        );
    }
}

inline const JsonValue& require_member(
    const JsonValue& object,
    const std::string& key,
    const std::string& path
) {
    if (!object.contains(key)) {
        fail(path, "Missing required field '" + key + "'");
    }

    return object.at(key);
}

inline void reject_unknown_members(
    const JsonValue& object,
    const std::vector<std::string>& allowed,
    const std::string& path
) {
    for (const auto& [key, value] : object.as_object()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) ==
            allowed.end()) {
            fail(path + "." + key, "Unknown field '" + key + "'");
        }
    }
}

inline std::string require_string(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_string()) {
        fail(
            path,
            std::string("Expected a string, got ") + value.type_name()
        );
    }

    return value.as_string();
}

inline double require_number(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_number()) {
        fail(
            path,
            std::string("Expected a number, got ") + value.type_name()
        );
    }

    const double result = value.as_number();
    if (!std::isfinite(result)) {
        fail(path, "Expected a finite number");
    }

    return result;
}

inline bool require_boolean(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_boolean()) {
        fail(
            path,
            std::string("Expected a boolean, got ") + value.type_name()
        );
    }

    return value.as_boolean();
}

inline ParameterMap parse_parameters(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);

    ParameterMap parameters;

    for (const auto& [name, parameter] : value.as_object()) {
        parameters.emplace(
            name,
            require_number(parameter, path + "." + name)
        );
    }

    return parameters;
}

inline std::size_t require_non_negative_integer(
    const JsonValue& value,
    const std::string& path
) {
    const double number = require_number(value, path);

    if (std::floor(number) != number || number < 0.0) {
        fail(path, "Expected a non-negative integer");
    }

    // Keep the value in the range a double represents exactly and a
    // size_t can hold on every supported platform.
    if (number > 1.0e15) {
        fail(path, "Integer value is too large");
    }

    return static_cast<std::size_t>(number);
}

inline void allow_schema_reference(const JsonValue& root) {
    if (root.contains("$schema")) {
        (void)require_string(
            root.at("$schema"),
            std::string(kRootPath) + ".$schema"
        );
    }
}

inline int require_protocol_version_number(const JsonValue& root) {
    const std::string path =
        std::string(kRootPath) + ".protocol_version";
    const double version = require_number(
        require_member(root, "protocol_version", kRootPath),
        path
    );

    if (std::floor(version) != version ||
        version < 0.0 ||
        version > static_cast<double>(std::numeric_limits<int>::max())) {
        fail(path, "Expected a non-negative integer");
    }

    return static_cast<int>(version);
}

inline int require_protocol_version(const JsonValue& root) {
    const int version = require_protocol_version_number(root);

    if (version != kBacktestProtocolVersion) {
        fail(
            std::string(kRootPath) + ".protocol_version",
            "Unsupported protocol_version; this build speaks version " +
                std::to_string(kBacktestProtocolVersion)
        );
    }

    return version;
}

inline void require_message_type(
    const JsonValue& root,
    const std::string& expected
) {
    const std::string type = require_string(
        require_member(root, "type", kRootPath),
        std::string(kRootPath) + ".type"
    );

    if (type != expected) {
        fail(
            std::string(kRootPath) + ".type",
            "Expected message type '" + expected + "', got '" + type + "'"
        );
    }
}

}  // namespace protocol_detail
