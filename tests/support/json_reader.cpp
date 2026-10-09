#include "support/json_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>

namespace recovery::test {

namespace json {

namespace {

const Value& nullValue() {
    static const Value value{nullptr};
    return value;
}

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    std::optional<Value> document(std::string* error) {
        std::optional<Value> value = parseValue(0);
        skipSpace();
        if (value.has_value() && at_ != text_.size()) {
            fail("text after the value");
            value.reset();
        }
        if (!value.has_value() && error != nullptr) {
            *error = error_ + " at byte " + std::to_string(at_);
        }
        return value;
    }

private:
    void fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
    }

    void skipSpace() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) {
            return false;
        }
        at_ += word.size();
        return true;
    }

    std::optional<Value> parseValue(int depth) {
        if (depth > 64) {
            fail("nested too deeply");
            return std::nullopt;
        }
        skipSpace();
        if (at_ >= text_.size()) {
            fail("a value is missing");
            return std::nullopt;
        }
        const char c = text_[at_];
        if (c == '{') {
            return parseObject(depth);
        }
        if (c == '[') {
            return parseArray(depth);
        }
        if (c == '"') {
            std::optional<std::string> text = parseString();
            return text.has_value() ? std::optional<Value>(Value{std::move(*text)}) : std::nullopt;
        }
        if (literal("true")) {
            return Value{true};
        }
        if (literal("false")) {
            return Value{false};
        }
        if (literal("null")) {
            return Value{nullptr};
        }
        return parseNumber();
    }

    std::optional<Value> parseObject(int depth) {
        ++at_;
        Object members;
        skipSpace();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return Value{std::move(members)};
        }
        for (;;) {
            skipSpace();
            if (at_ >= text_.size() || text_[at_] != '"') {
                fail("a member name is missing");
                return std::nullopt;
            }
            std::optional<std::string> name = parseString();
            if (!name.has_value()) {
                return std::nullopt;
            }
            for (const auto& member : members) {
                if (member.first == *name) {
                    fail("the member \"" + *name + "\" twice");
                    return std::nullopt;
                }
            }
            skipSpace();
            if (at_ >= text_.size() || text_[at_] != ':') {
                fail("':' is missing");
                return std::nullopt;
            }
            ++at_;
            std::optional<Value> value = parseValue(depth + 1);
            if (!value.has_value()) {
                return std::nullopt;
            }
            members.emplace_back(std::move(*name), std::move(*value));
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return Value{std::move(members)};
            }
            fail("',' or '}' is missing");
            return std::nullopt;
        }
    }

    std::optional<Value> parseArray(int depth) {
        ++at_;
        Array elements;
        skipSpace();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return Value{std::move(elements)};
        }
        for (;;) {
            std::optional<Value> value = parseValue(depth + 1);
            if (!value.has_value()) {
                return std::nullopt;
            }
            elements.push_back(std::move(*value));
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return Value{std::move(elements)};
            }
            fail("',' or ']' is missing");
            return std::nullopt;
        }
    }

    std::optional<std::uint32_t> hex4() {
        if (text_.size() - at_ < 4) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        const auto [end, ec] = std::from_chars(text_.data() + at_, text_.data() + at_ + 4, value, 16);
        if (ec != std::errc{} || end != text_.data() + at_ + 4) {
            return std::nullopt;
        }
        at_ += 4;
        return value;
    }

    static void appendUtf8(std::string& out, std::uint32_t value) {
        if (value < 0x80) {
            out.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (value >> 6)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (value >> 12)));
            out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (value >> 18)));
            out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
    }

    // Validates one UTF-8 sequence starting at at_ and copies it.
    bool copyUtf8(std::string& out) {
        const auto lead = static_cast<unsigned char>(text_[at_]);
        std::size_t length = lead < 0x80 ? 1 : (lead >= 0xC2 && lead <= 0xDF) ? 2 : (lead >= 0xE0 && lead <= 0xEF) ? 3
                                             : (lead >= 0xF0 && lead <= 0xF4) ? 4
                                                                                : 0;
        if (length == 0 || text_.size() - at_ < length) {
            return false;
        }
        std::uint32_t value = length == 1 ? lead : lead & (0xFFu >> (length + 1));
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text_[at_ + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            value = (value << 6) | (next & 0x3Fu);
        }
        constexpr std::uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (value < kMinimum[length] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
        out.append(text_.substr(at_, length));
        at_ += length;
        return true;
    }

    std::optional<std::string> parseString() {
        ++at_;
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == '"') {
                ++at_;
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("a control character in a string");
                return std::nullopt;
            }
            if (c != '\\') {
                if (!copyUtf8(out)) {
                    fail("invalid UTF-8 in a string");
                    return std::nullopt;
                }
                continue;
            }
            ++at_;
            if (at_ >= text_.size()) {
                break;
            }
            const char escape = text_[at_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::optional<std::uint32_t> unit = hex4();
                if (!unit.has_value()) {
                    fail("a bad \\u escape");
                    return std::nullopt;
                }
                std::uint32_t value = *unit;
                if (value >= 0xD800 && value <= 0xDBFF) {
                    if (!literal("\\u")) {
                        fail("an unpaired surrogate");
                        return std::nullopt;
                    }
                    const std::optional<std::uint32_t> low = hex4();
                    if (!low.has_value() || *low < 0xDC00 || *low > 0xDFFF) {
                        fail("an unpaired surrogate");
                        return std::nullopt;
                    }
                    value = 0x10000 + ((value - 0xD800) << 10) + (*low - 0xDC00);
                } else if (value >= 0xDC00 && value <= 0xDFFF) {
                    fail("an unpaired surrogate");
                    return std::nullopt;
                }
                appendUtf8(out, value);
                break;
            }
            default:
                fail("an unknown escape");
                return std::nullopt;
            }
        }
        fail("a string is not closed");
        return std::nullopt;
    }

    std::optional<Value> parseNumber() {
        const std::size_t start = at_;
        const auto digit = [&] { return at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9'; };
        if (at_ < text_.size() && text_[at_] == '-') {
            ++at_;
        }
        if (!digit()) {
            fail("not a value");
            return std::nullopt;
        }
        if (text_[at_] == '0') {
            ++at_;
        } else {
            while (digit()) {
                ++at_;
            }
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (!digit()) {
                fail("a bad number");
                return std::nullopt;
            }
            while (digit()) {
                ++at_;
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (!digit()) {
                fail("a bad number");
                return std::nullopt;
            }
            while (digit()) {
                ++at_;
            }
        }
        return Value{Number{std::string(text_.substr(start, at_ - start))}};
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::string error_;
};

}  // namespace

bool Value::has(std::string_view key) const {
    const Object* object = std::get_if<Object>(&data);
    return object != nullptr &&
           std::any_of(object->begin(), object->end(), [&](const auto& member) { return member.first == key; });
}

const Value& Value::operator[](std::string_view key) const {
    if (const Object* object = std::get_if<Object>(&data)) {
        for (const auto& member : *object) {
            if (member.first == key) {
                return member.second;
            }
        }
    }
    ADD_FAILURE() << "no JSON member \"" << key << "\"";
    return nullValue();
}

const Value& Value::operator[](std::size_t index) const {
    if (const Array* array = std::get_if<Array>(&data); array != nullptr && index < array->size()) {
        return (*array)[index];
    }
    ADD_FAILURE() << "no JSON element " << index;
    return nullValue();
}

std::size_t Value::size() const {
    if (const Array* array = std::get_if<Array>(&data)) {
        return array->size();
    }
    if (const Object* object = std::get_if<Object>(&data)) {
        return object->size();
    }
    ADD_FAILURE() << "not a JSON array or object";
    return 0;
}

std::string Value::string() const {
    if (const std::string* text = std::get_if<std::string>(&data)) {
        return *text;
    }
    ADD_FAILURE() << "not a JSON string";
    return {};
}

std::uint64_t Value::u64() const {
    if (const Number* number = std::get_if<Number>(&data)) {
        std::uint64_t value = 0;
        const auto [end, ec] =
            std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
        if (ec == std::errc{} && end == number->text.data() + number->text.size()) {
            return value;
        }
    }
    ADD_FAILURE() << "not a JSON unsigned integer";
    return 0;
}

bool Value::boolean() const {
    if (const bool* value = std::get_if<bool>(&data)) {
        return *value;
    }
    ADD_FAILURE() << "not a JSON boolean";
    return false;
}

std::optional<Value> parse(std::string_view text, std::string* error) {
    return Reader(text).document(error);
}

}  // namespace json

}  // namespace recovery::test
