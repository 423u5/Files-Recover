#pragma once

// A strict JSON reader for tests (RFC 8259): what the engine's reports (P18,
// P19) write must be a document any reader takes, so the reader refuses
// what a lenient one would forgive (invalid UTF-8, bad escapes, trailing
// commas, duplicate names, anything after the value).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace recovery::test {

namespace json {

struct Value;
using Array = std::vector<Value>;
// Members in document order (duplicate names are refused by the reader).
using Object = std::vector<std::pair<std::string, Value>>;
// A number, as written (64-bit integers are kept exact).
struct Number {
    std::string text;
};

struct Value {
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> data;

    [[nodiscard]] bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
    [[nodiscard]] bool isObject() const noexcept { return std::holds_alternative<Object>(data); }
    [[nodiscard]] bool isArray() const noexcept { return std::holds_alternative<Array>(data); }
    [[nodiscard]] bool has(std::string_view key) const;
    // A member (ADD_FAILURE and null when there is none).
    [[nodiscard]] const Value& operator[](std::string_view key) const;
    // An element (ADD_FAILURE and null when there is none).
    [[nodiscard]] const Value& operator[](std::size_t index) const;
    [[nodiscard]] std::size_t size() const;
    // ADD_FAILURE when the value is of another type.
    [[nodiscard]] std::string string() const;
    [[nodiscard]] std::uint64_t u64() const;
    [[nodiscard]] bool boolean() const;
};

// Parses an RFC 8259 document strictly (valid UTF-8, escapes, no trailing
// commas, nothing after the value). nullopt, with `error` set, otherwise.
[[nodiscard]] std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

}  // namespace json

}  // namespace recovery::test
