#pragma once

// A JSON writer for the reports (RFC 8259): UTF-8, indented by two
// spaces. Strings are escaped and invalid UTF-8 in them becomes U+FFFD, so
// names from a damaged disk always give a valid document. Integers are
// written as they are (64-bit); readers that hold numbers as doubles see
// values above 2^53 rounded.

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::report {

class JsonWriter {
public:
    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    // Inside an object: the name of the next value.
    void key(std::string_view name);

    void string(std::string_view text);
    void number(std::uint64_t value);
    void number(std::int64_t value);
    void boolean(bool value);
    void null();

    // A member: key(name), then the value.
    void field(std::string_view name, std::string_view text);
    void field(std::string_view name, const char* text);
    void field(std::string_view name, const std::string& text);
    void field(std::string_view name, bool value);
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    void field(std::string_view name, T value) {
        key(name);
        if constexpr (std::is_signed_v<T>) {
            number(static_cast<std::int64_t>(value));
        } else {
            number(static_cast<std::uint64_t>(value));
        }
    }
    // The value, or null.
    template <class T>
    void field(std::string_view name, const std::optional<T>& value) {
        if (value.has_value()) {
            field(name, *value);
        } else {
            nullField(name);
        }
    }
    void nullField(std::string_view name);

    // The document (complete once every object and array is closed), with a
    // final newline.
    [[nodiscard]] std::string finish();

private:
    struct Level {
        bool object = false;
        bool empty = true;
    };

    void beforeValue();
    void close(char bracket);

    std::vector<Level> levels_;
    std::string out_;
    bool afterKey_ = false;
};

}  // namespace recovery::report
