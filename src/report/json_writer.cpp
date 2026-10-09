#include "report/json_writer.hpp"

#include "report/utf8.hpp"

#include <format>

namespace recovery::report {

namespace {

void appendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const char32_t value = decodeUtf8(text, i);
        switch (value) {
        case U'"':
            out += "\\\"";
            break;
        case U'\\':
            out += "\\\\";
            break;
        case U'\b':
            out += "\\b";
            break;
        case U'\f':
            out += "\\f";
            break;
        case U'\n':
            out += "\\n";
            break;
        case U'\r':
            out += "\\r";
            break;
        case U'\t':
            out += "\\t";
            break;
        default:
            // Controls must be escaped; U+2028 and U+2029 are, so that the
            // document is also valid JavaScript.
            if (value < 0x20 || value == 0x2028 || value == 0x2029) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(value));
            } else {
                appendUtf8(out, value);
            }
            break;
        }
    }
    out.push_back('"');
}

}  // namespace

void JsonWriter::beforeValue() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (!levels_.empty()) {
        if (!levels_.back().empty) {
            out_.push_back(',');
        }
        levels_.back().empty = false;
        out_.push_back('\n');
        out_.append(levels_.size() * 2, ' ');
    }
}

void JsonWriter::beginObject() {
    beforeValue();
    out_.push_back('{');
    levels_.push_back(Level{true, true});
}

void JsonWriter::endObject() {
    close('}');
}

void JsonWriter::beginArray() {
    beforeValue();
    out_.push_back('[');
    levels_.push_back(Level{false, true});
}

void JsonWriter::endArray() {
    close(']');
}

void JsonWriter::close(char bracket) {
    if (levels_.empty()) {
        return;
    }
    const bool empty = levels_.back().empty;
    levels_.pop_back();
    if (!empty) {
        out_.push_back('\n');
        out_.append(levels_.size() * 2, ' ');
    }
    out_.push_back(bracket);
}

void JsonWriter::key(std::string_view name) {
    beforeValue();
    appendEscaped(out_, name);
    out_ += ": ";
    afterKey_ = true;
}

void JsonWriter::string(std::string_view text) {
    beforeValue();
    appendEscaped(out_, text);
}

void JsonWriter::number(std::uint64_t value) {
    beforeValue();
    out_ += std::to_string(value);
}

void JsonWriter::number(std::int64_t value) {
    beforeValue();
    out_ += std::to_string(value);
}

void JsonWriter::boolean(bool value) {
    beforeValue();
    out_ += value ? "true" : "false";
}

void JsonWriter::null() {
    beforeValue();
    out_ += "null";
}

void JsonWriter::field(std::string_view name, std::string_view text) {
    key(name);
    string(text);
}

void JsonWriter::field(std::string_view name, const char* text) {
    field(name, std::string_view(text));
}

void JsonWriter::field(std::string_view name, const std::string& text) {
    field(name, std::string_view(text));
}

void JsonWriter::field(std::string_view name, bool value) {
    key(name);
    boolean(value);
}

void JsonWriter::nullField(std::string_view name) {
    key(name);
    null();
}

std::string JsonWriter::finish() {
    std::string document = std::move(out_);
    document.push_back('\n');
    out_.clear();
    levels_.clear();
    afterKey_ = false;
    return document;
}

}  // namespace recovery::report
