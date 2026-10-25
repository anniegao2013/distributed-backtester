#include "json_value.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int passed = 0;
int failed = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_parse_error(
    const std::string& text,
    const std::string& message
) {
    bool threw = false;

    try {
        (void)parse_json(text);
    } catch (const JsonParseError&) {
        threw = true;
    }

    expect(threw, message);
}

void expect_type_error(
    const std::function<void()>& action,
    const std::string& message
) {
    bool threw = false;

    try {
        action();
    } catch (const JsonTypeError&) {
        threw = true;
    }

    expect(threw, message);
}

void run_test(
    const std::string& name,
    const std::function<void()>& test
) {
    try {
        test();
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "[FAIL] " << name << ": " << error.what() << '\n';
    }
}

void test_scalars() {
    expect(parse_json("null").is_null(), "null should parse");
    expect(parse_json("true").as_boolean(), "true should parse");
    expect(!parse_json("false").as_boolean(), "false should parse");
    expect(parse_json("42").as_number() == 42.0, "integer should parse");
    expect(
        parse_json("-12.5e2").as_number() == -1250.0,
        "scientific notation should parse"
    );
    expect(
        parse_json("\"hello\"").as_string() == "hello",
        "string should parse"
    );
}

void test_containers() {
    const JsonValue value = parse_json(
        R"({"name":"sweep","count":3,"values":[1,2,3],"nested":{"ok":true}})"
    );

    expect(value.is_object(), "root should be an object");
    expect(value.at("name").as_string() == "sweep", "name should match");
    expect(value.at("count").as_number() == 3.0, "count should match");
    expect(value.at("values").size() == 3, "array should have three items");
    expect(
        value.at("values").at(2).as_number() == 3.0,
        "array indexing should work"
    );
    expect(
        value.at("nested").at("ok").as_boolean(),
        "nested object should work"
    );
    expect(value.contains("name"), "contains should find a key");
    expect(!value.contains("missing"), "contains should miss a key");
}

void test_string_escapes() {
    const JsonValue value = parse_json(
        R"({"s":"line\nbreak\t\"quoted\" \\ slash \/ \b\f\r"})"
    );

    expect(
        value.at("s").as_string() ==
            "line\nbreak\t\"quoted\" \\ slash / \b\f\r",
        "standard escapes should decode"
    );

    const JsonValue unicode = parse_json(
        R"("caf\u00e9 \ud83d\ude00")"
    );

    expect(
        unicode.as_string() == "caf\xC3\xA9 \xF0\x9F\x98\x80",
        "unicode escapes should decode to UTF-8"
    );
}

void test_roundtrip_and_determinism() {
    JsonValue::Object object;
    object.emplace("a", 1);
    object.emplace("b", true);
    object.emplace("c", nullptr);
    object.emplace(
        "d",
        JsonValue::Array{
            JsonValue("x\n"),
            JsonValue(2.5),
            JsonValue::Object{{"k", "v"}}
        }
    );

    const JsonValue original(std::move(object));
    const JsonValue roundtrip = parse_json(serialize_json(original));

    expect(roundtrip == original, "serialize/parse should round-trip");

    const std::string compact = serialize_json(original);
    expect(
        compact ==
            R"({"a":1,"b":true,"c":null,"d":["x\n",2.5,{"k":"v"}]})",
        "compact output should be stable and escaped"
    );

    const std::string pretty = serialize_json(original, true);
    expect(
        pretty.find("\n  \"a\": 1,") != std::string::npos,
        "pretty output should be indented"
    );
}

void test_rejects_malformed_documents() {
    expect_parse_error("", "empty input should fail");
    expect_parse_error("{", "unterminated object should fail");
    expect_parse_error(R"({"a":})", "missing value should fail");
    expect_parse_error("[1,]", "trailing comma should fail");
    expect_parse_error(R"({"a":1} extra)", "trailing content should fail");
    expect_parse_error(R"({"a":1,"a":2})", "duplicate keys should fail");
    expect_parse_error("01", "leading zero should fail");
    expect_parse_error("+1", "leading plus should fail");
    expect_parse_error(".5", "missing integer part should fail");
    expect_parse_error("1.", "missing fraction should fail");
    expect_parse_error("1e", "missing exponent should fail");
    expect_parse_error(R"("\q")", "invalid escape should fail");
    expect_parse_error(R"("\ud800")", "unpaired high surrogate should fail");
    expect_parse_error("\"a\nb\"", "raw control character should fail");
    expect_parse_error("1e9999", "out-of-range number should fail");
}

void test_bom_is_accepted() {
    const std::string text = "\xEF\xBB\xBF{\"a\":1}";
    expect(
        parse_json(text).at("a").as_number() == 1.0,
        "UTF-8 BOM should be tolerated"
    );
}

void test_type_errors() {
    JsonValue object = parse_json(R"({"a":1})");

    expect_type_error(
        [&object]() { (void)object.at("missing"); },
        "missing key should throw JsonTypeError"
    );
    expect_type_error(
        [&object]() { (void)object.as_array(); },
        "wrong accessor should throw JsonTypeError"
    );
    expect_type_error(
        [&object]() { object.push_back(1); },
        "push_back on an object should throw"
    );

    const JsonValue array = parse_json("[1]");
    expect_type_error(
        [&array]() { (void)array.at(5); },
        "out-of-range index should throw"
    );
}

void test_number_serialization() {
    expect(serialize_json(JsonValue(10000.0)) == "10000",
           "integral doubles should serialize without a decimal");
    expect(serialize_json(JsonValue(0.001)) == "0.001",
           "small rates should keep precision");
    expect(serialize_json(JsonValue(0.0)) == "0",
           "zero should serialize as 0");
}

}  // namespace

int main() {
    run_test("scalars", test_scalars);
    run_test("containers", test_containers);
    run_test("string escapes", test_string_escapes);
    run_test("roundtrip and determinism", test_roundtrip_and_determinism);
    run_test("rejects malformed documents", test_rejects_malformed_documents);
    run_test("BOM accepted", test_bom_is_accepted);
    run_test("type errors", test_type_errors);
    run_test("number serialization", test_number_serialization);

    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed\n";

    return failed == 0 ? 0 : 1;
}
