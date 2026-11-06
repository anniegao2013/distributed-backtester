#include "core/identifier.h"

#include <algorithm>
#include <string>

namespace {

bool is_ascii_alphanumeric(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

bool starts_with_alphanumeric(const std::string& value) {
    return !value.empty() && is_ascii_alphanumeric(value.front());
}

}  // namespace

bool is_identifier(const std::string& value) {
    if (!starts_with_alphanumeric(value)) {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char c) {
            return is_ascii_alphanumeric(c) || c == '_' || c == '-';
        }
    );
}

bool is_dotted_identifier(const std::string& value) {
    if (!starts_with_alphanumeric(value)) {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char c) {
            return is_ascii_alphanumeric(c) ||
                   c == '_' || c == '-' || c == '.';
        }
    );
}
