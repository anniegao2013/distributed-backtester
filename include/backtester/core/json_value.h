#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

// A small, dependency-free JSON value used by the experiment schema.
//
// The experiment layer needs to both read request documents and emit
// machine-readable responses, so this type supports parsing, construction,
// and serialization. Object keys are stored in a std::map, which makes
// serialized output deterministic.
class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    JsonValue() = default;
    JsonValue(std::nullptr_t);
    JsonValue(bool value);

    // Integral values are stored as doubles. Excludes bool so that
    // JsonValue(true) selects the boolean overload.
    template <
        typename Integer,
        typename = std::enable_if_t<
            std::is_integral_v<Integer> &&
            !std::is_same_v<std::remove_cv_t<Integer>, bool>
        >
    >
    JsonValue(Integer value) : value_(static_cast<double>(value)) {}

    JsonValue(double value);
    JsonValue(const char* value);
    JsonValue(std::string value);
    JsonValue(Array value);
    JsonValue(Object value);

    Type type() const noexcept;
    const char* type_name() const noexcept;

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool as_boolean() const;
    double as_number() const;
    const std::string& as_string() const;

    const Array& as_array() const;
    Array& as_array();

    const Object& as_object() const;
    Object& as_object();

    bool contains(const std::string& key) const;

    // Object access. `at` throws JsonTypeError when the key is absent or
    // when this value is not an object.
    const JsonValue& at(const std::string& key) const;
    JsonValue& at(const std::string& key);

    // Array access. `at` throws JsonTypeError when out of range or when this
    // value is not an array.
    const JsonValue& at(std::size_t index) const;
    JsonValue& at(std::size_t index);

    // Inserts or replaces an object member. Throws JsonTypeError when this
    // value is not an object.
    JsonValue& operator[](const std::string& key);
    void set(std::string key, JsonValue value);

    // Appends to an array. Throws JsonTypeError when this value is not an
    // array.
    void push_back(JsonValue value);

    std::size_t size() const;
    bool empty() const;

    bool operator==(const JsonValue& other) const;
    bool operator!=(const JsonValue& other) const;

private:
    std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        Array,
        Object
    > value_{nullptr};

    [[noreturn]] void throw_type_error(const char* expected) const;
};

// Thrown for malformed JSON text. The message includes the line and column.
class JsonParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown when a JsonValue is accessed as the wrong type.
class JsonTypeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parses a complete JSON document. Throws JsonParseError on malformed input.
// Duplicate object keys, trailing content, and non-finite numbers are
// rejected.
JsonValue parse_json(const std::string& text);

// Serializes a JSON value. When `pretty` is true the output is indented with
// two spaces per nesting level. Throws std::invalid_argument for non-finite
// numbers.
std::string serialize_json(const JsonValue& value, bool pretty = false);
