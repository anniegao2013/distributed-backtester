#include "json_value.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

JsonValue::JsonValue(std::nullptr_t) : value_(nullptr) {}

JsonValue::JsonValue(bool value) : value_(value) {}

JsonValue::JsonValue(double value) : value_(value) {}

JsonValue::JsonValue(const char* value)
    : value_(std::string(value == nullptr ? "" : value)) {}

JsonValue::JsonValue(std::string value) : value_(std::move(value)) {}

JsonValue::JsonValue(Array value) : value_(std::move(value)) {}

JsonValue::JsonValue(Object value) : value_(std::move(value)) {}

JsonValue::Type JsonValue::type() const noexcept {
    switch (value_.index()) {
        case 0:
            return Type::Null;
        case 1:
            return Type::Boolean;
        case 2:
            return Type::Number;
        case 3:
            return Type::String;
        case 4:
            return Type::Array;
        default:
            return Type::Object;
    }
}

const char* JsonValue::type_name() const noexcept {
    switch (type()) {
        case Type::Null:
            return "null";
        case Type::Boolean:
            return "boolean";
        case Type::Number:
            return "number";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
    }

    return "unknown";
}

bool JsonValue::is_null() const noexcept {
    return type() == Type::Null;
}

bool JsonValue::is_boolean() const noexcept {
    return type() == Type::Boolean;
}

bool JsonValue::is_number() const noexcept {
    return type() == Type::Number;
}

bool JsonValue::is_string() const noexcept {
    return type() == Type::String;
}

bool JsonValue::is_array() const noexcept {
    return type() == Type::Array;
}

bool JsonValue::is_object() const noexcept {
    return type() == Type::Object;
}

bool JsonValue::as_boolean() const {
    if (!is_boolean()) {
        throw_type_error("boolean");
    }

    return std::get<bool>(value_);
}

double JsonValue::as_number() const {
    if (!is_number()) {
        throw_type_error("number");
    }

    return std::get<double>(value_);
}

const std::string& JsonValue::as_string() const {
    if (!is_string()) {
        throw_type_error("string");
    }

    return std::get<std::string>(value_);
}

const JsonValue::Array& JsonValue::as_array() const {
    if (!is_array()) {
        throw_type_error("array");
    }

    return std::get<Array>(value_);
}

JsonValue::Array& JsonValue::as_array() {
    if (!is_array()) {
        throw_type_error("array");
    }

    return std::get<Array>(value_);
}

const JsonValue::Object& JsonValue::as_object() const {
    if (!is_object()) {
        throw_type_error("object");
    }

    return std::get<Object>(value_);
}

JsonValue::Object& JsonValue::as_object() {
    if (!is_object()) {
        throw_type_error("object");
    }

    return std::get<Object>(value_);
}

bool JsonValue::contains(const std::string& key) const {
    if (!is_object()) {
        return false;
    }

    return as_object().find(key) != as_object().end();
}

const JsonValue& JsonValue::at(const std::string& key) const {
    if (!is_object()) {
        throw_type_error("object");
    }

    const auto& object = as_object();
    const auto it = object.find(key);

    if (it == object.end()) {
        throw JsonTypeError("JSON object has no member '" + key + "'");
    }

    return it->second;
}

JsonValue& JsonValue::at(const std::string& key) {
    if (!is_object()) {
        throw_type_error("object");
    }

    auto& object = as_object();
    const auto it = object.find(key);

    if (it == object.end()) {
        throw JsonTypeError("JSON object has no member '" + key + "'");
    }

    return it->second;
}

const JsonValue& JsonValue::at(std::size_t index) const {
    if (!is_array()) {
        throw_type_error("array");
    }

    const auto& array = as_array();
    if (index >= array.size()) {
        throw JsonTypeError("JSON array index out of range");
    }

    return array[index];
}

JsonValue& JsonValue::at(std::size_t index) {
    if (!is_array()) {
        throw_type_error("array");
    }

    auto& array = as_array();
    if (index >= array.size()) {
        throw JsonTypeError("JSON array index out of range");
    }

    return array[index];
}

JsonValue& JsonValue::operator[](const std::string& key) {
    if (!is_object()) {
        throw_type_error("object");
    }

    return as_object()[key];
}

void JsonValue::set(std::string key, JsonValue value) {
    if (!is_object()) {
        throw_type_error("object");
    }

    as_object().insert_or_assign(std::move(key), std::move(value));
}

void JsonValue::push_back(JsonValue value) {
    if (!is_array()) {
        throw_type_error("array");
    }

    as_array().push_back(std::move(value));
}

std::size_t JsonValue::size() const {
    if (is_array()) {
        return as_array().size();
    }

    if (is_object()) {
        return as_object().size();
    }

    if (is_string()) {
        return as_string().size();
    }

    return 0;
}

bool JsonValue::empty() const {
    return size() == 0;
}

bool JsonValue::operator==(const JsonValue& other) const {
    return value_ == other.value_;
}

bool JsonValue::operator!=(const JsonValue& other) const {
    return !(*this == other);
}

void JsonValue::throw_type_error(const char* expected) const {
    throw JsonTypeError(
        std::string("Expected JSON ") + expected + ", got " + type_name()
    );
}

namespace {

constexpr int kMaxDepth = 100;

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    JsonValue parse_document() {
        // Tolerate a UTF-8 byte-order mark at the start of a document.
        if (text_.size() >= 3 &&
            static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            position_ = 3;
        }

        skip_whitespace();
        JsonValue value = parse_value(0);
        skip_whitespace();

        if (!at_end()) {
            fail("Unexpected trailing content");
        }

        return value;
    }

private:
    const std::string& text_;
    std::size_t position_ = 0;

    bool at_end() const {
        return position_ >= text_.size();
    }

    char peek() const {
        return at_end() ? '\0' : text_[position_];
    }

    char consume() {
        if (at_end()) {
            fail("Unexpected end of input");
        }

        return text_[position_++];
    }

    void skip_whitespace() {
        while (!at_end()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    [[noreturn]] void fail(const std::string& message) const {
        std::size_t line = 1;
        std::size_t column = 1;

        for (std::size_t i = 0; i < position_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }

        throw JsonParseError(
            message + " at line " + std::to_string(line) +
            ", column " + std::to_string(column)
        );
    }

    void expect(char expected, const std::string& context) {
        if (consume() != expected) {
            fail("Expected '" + std::string(1, expected) + "' " + context);
        }
    }

    JsonValue parse_value(int depth) {
        if (depth > kMaxDepth) {
            fail("Maximum JSON nesting depth exceeded");
        }

        if (at_end()) {
            fail("Unexpected end of input");
        }

        switch (peek()) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"':
                return JsonValue(parse_string());
            case 't':
                parse_literal("true");
                return JsonValue(true);
            case 'f':
                parse_literal("false");
                return JsonValue(false);
            case 'n':
                parse_literal("null");
                return JsonValue(nullptr);
            default:
                if (peek() == '-' ||
                    (peek() >= '0' && peek() <= '9')) {
                    return JsonValue(parse_number());
                }

                fail("Unexpected character while parsing a value");
        }
    }

    JsonValue parse_object(int depth) {
        expect('{', "to start an object");
        skip_whitespace();

        JsonValue::Object object;

        if (peek() == '}') {
            consume();
            return JsonValue(std::move(object));
        }

        while (true) {
            skip_whitespace();

            if (peek() != '"') {
                fail("Expected a string key in an object");
            }

            std::string key = parse_string();
            skip_whitespace();
            expect(':', "after an object key");
            skip_whitespace();

            JsonValue value = parse_value(depth + 1);

            const auto inserted = object.emplace(key, std::move(value));
            if (!inserted.second) {
                fail("Duplicate object key '" + key + "'");
            }

            skip_whitespace();

            if (peek() == ',') {
                consume();
                continue;
            }

            if (peek() == '}') {
                consume();
                break;
            }

            fail("Expected ',' or '}' in an object");
        }

        return JsonValue(std::move(object));
    }

    JsonValue parse_array(int depth) {
        expect('[', "to start an array");
        skip_whitespace();

        JsonValue::Array array;

        if (peek() == ']') {
            consume();
            return JsonValue(std::move(array));
        }

        while (true) {
            skip_whitespace();
            array.push_back(parse_value(depth + 1));
            skip_whitespace();

            if (peek() == ',') {
                consume();
                continue;
            }

            if (peek() == ']') {
                consume();
                break;
            }

            fail("Expected ',' or ']' in an array");
        }

        return JsonValue(std::move(array));
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    unsigned int parse_hex_quad() {
        unsigned int value = 0;

        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(consume());
            if (digit < 0) {
                fail("Invalid \\u escape in string");
            }
            value = (value << 4) | static_cast<unsigned int>(digit);
        }

        return value;
    }

    static void append_utf8(std::string& output, unsigned int code_point) {
        if (code_point <= 0x7F) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FF) {
            output.push_back(
                static_cast<char>(0xC0 | (code_point >> 6)));
            output.push_back(
                static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0xFFFF) {
            output.push_back(
                static_cast<char>(0xE0 | (code_point >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            output.push_back(
                static_cast<char>(0xF0 | (code_point >> 18)));
            output.push_back(
                static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"', "to start a string");

        std::string output;

        while (true) {
            if (at_end()) {
                fail("Unterminated string");
            }

            const unsigned char c =
                static_cast<unsigned char>(consume());

            if (c == '"') {
                break;
            }

            if (c < 0x20) {
                fail("Unescaped control character in string");
            }

            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }

            const char escape = consume();
            switch (escape) {
                case '"':
                    output.push_back('"');
                    break;
                case '\\':
                    output.push_back('\\');
                    break;
                case '/':
                    output.push_back('/');
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    unsigned int code_point = parse_hex_quad();

                    // Combine a UTF-16 surrogate pair.
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (consume() != '\\' || consume() != 'u') {
                            fail("Unpaired high surrogate in string");
                        }

                        const unsigned int low = parse_hex_quad();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("Invalid low surrogate in string");
                        }

                        code_point = 0x10000 +
                            ((code_point - 0xD800) << 10) +
                            (low - 0xDC00);
                    } else if (
                        code_point >= 0xDC00 && code_point <= 0xDFFF
                    ) {
                        fail("Unpaired low surrogate in string");
                    }

                    append_utf8(output, code_point);
                    break;
                }
                default:
                    fail("Invalid escape sequence in string");
            }
        }

        return output;
    }

    double parse_number() {
        const std::size_t start = position_;

        if (peek() == '-') {
            ++position_;
        }

        if (peek() == '0') {
            ++position_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        } else {
            fail("Invalid number");
        }

        if (peek() == '.') {
            ++position_;
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("Expected a digit after the decimal point");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }

        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') {
                ++position_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("Expected a digit in the exponent");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }

        const std::string token =
            text_.substr(start, position_ - start);

        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);

        if (end == token.c_str() ||
            static_cast<std::size_t>(end - token.c_str()) != token.size() ||
            !std::isfinite(value)) {
            fail("Number is out of range");
        }

        return value;
    }

    void parse_literal(const char* literal) {
        for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
            if (consume() != *cursor) {
                fail(std::string("Expected literal '") + literal + "'");
            }
        }
    }
};

std::string format_number(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("Cannot serialize a non-finite number");
    }

    if (value == 0.0) {
        return "0";
    }

    if (std::floor(value) == value &&
        std::abs(value) < 1.0e15) {
        std::ostringstream integer;
        integer.imbue(std::locale::classic());
        integer << std::fixed << std::setprecision(0) << value;
        return integer.str();
    }

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(15) << value;
    return out.str();
}

void write_string(
    std::string& output,
    const std::string& value
) {
    static const char* kHex = "0123456789abcdef";

    output.push_back('"');

    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (c < 0x20) {
                    output += "\\u00";
                    output.push_back(kHex[(c >> 4) & 0x0F]);
                    output.push_back(kHex[c & 0x0F]);
                } else {
                    output.push_back(static_cast<char>(c));
                }
                break;
        }
    }

    output.push_back('"');
}

void write_value(
    std::string& output,
    const JsonValue& value,
    bool pretty,
    int depth
) {
    const auto newline = [&](int level) {
        if (!pretty) {
            return;
        }

        output.push_back('\n');
        output.append(static_cast<std::size_t>(level) * 2, ' ');
    };

    switch (value.type()) {
        case JsonValue::Type::Null:
            output += "null";
            break;
        case JsonValue::Type::Boolean:
            output += value.as_boolean() ? "true" : "false";
            break;
        case JsonValue::Type::Number:
            output += format_number(value.as_number());
            break;
        case JsonValue::Type::String:
            write_string(output, value.as_string());
            break;
        case JsonValue::Type::Array: {
            const auto& array = value.as_array();
            if (array.empty()) {
                output += "[]";
                break;
            }

            output.push_back('[');
            for (std::size_t i = 0; i < array.size(); ++i) {
                if (i > 0) {
                    output.push_back(',');
                }
                if (pretty) {
                    newline(depth + 1);
                }
                write_value(output, array[i], pretty, depth + 1);
            }
            if (pretty) {
                newline(depth);
            }
            output.push_back(']');
            break;
        }
        case JsonValue::Type::Object: {
            const auto& object = value.as_object();
            if (object.empty()) {
                output += "{}";
                break;
            }

            output.push_back('{');
            bool first = true;
            for (const auto& [key, member] : object) {
                if (!first) {
                    output.push_back(',');
                }
                if (pretty) {
                    newline(depth + 1);
                }
                write_string(output, key);
                output.push_back(':');
                if (pretty) {
                    output.push_back(' ');
                }
                write_value(output, member, pretty, depth + 1);
                first = false;
            }
            if (pretty) {
                newline(depth);
            }
            output.push_back('}');
            break;
        }
    }
}

}  // namespace

JsonValue parse_json(const std::string& text) {
    return Parser(text).parse_document();
}

std::string serialize_json(const JsonValue& value, bool pretty) {
    std::string output;
    write_value(output, value, pretty, 0);

    if (pretty) {
        output.push_back('\n');
    }

    return output;
}
