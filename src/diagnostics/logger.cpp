#include "diagnostics/logger.hpp"

#include "recovery/text.hpp"

#include <format>
#include <ostream>

namespace recovery::diagnostics {

namespace {

bool needsQuoting(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '"' || c == '=' || c == '\\') {
            return true;
        }
    }
    return false;
}

void appendEscaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (u < 0x20 || u == 0x7F) {
                out += std::format("\\x{:02X}", u);
            } else {
                out += c;
            }
            break;
        }
    }
}

}  // namespace

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Critical:
        return "CRITICAL";
    case LogLevel::Off:
        return "OFF";
    }
    return "UNKNOWN";
}

std::string formatRecord(const LogRecord& record) {
    std::string out = formatUtcTimestamp(record.timestamp);
    out += ' ';
    out += toString(record.level);
    out += " [";
    appendEscaped(out, record.component);
    out += "] ";
    appendEscaped(out, record.message);
    for (const LogField& f : record.fields) {
        out += ' ';
        appendEscaped(out, f.key);
        out += '=';
        if (needsQuoting(f.value)) {
            out += '"';
            appendEscaped(out, f.value);
            out += '"';
        } else {
            out += f.value;
        }
    }
    return out;
}

Logger::Logger(LogLevel minLevel) noexcept : minLevel_(minLevel) {}

void Logger::addSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::setMinLevel(LogLevel level) noexcept {
    minLevel_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::minLevel() const noexcept {
    return minLevel_.load(std::memory_order_relaxed);
}

bool Logger::isEnabled(LogLevel level) const noexcept {
    return level != LogLevel::Off && level >= minLevel();
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message,
                 std::initializer_list<LogField> fields) noexcept {
    if (!isEnabled(level)) {
        return;
    }
    try {
        const LogRecord record{std::chrono::system_clock::now(), level, std::string(component),
                               std::string(message), std::vector<LogField>(fields)};
        const std::scoped_lock lock(mutex_);
        for (const auto& sink : sinks_) {
            try {
                sink->write(record);
            } catch (...) {
                // A failing sink must never break the operation being logged.
            }
        }
    } catch (...) {
        // Allocation failure while building the record: drop it.
    }
}

StreamSink::StreamSink(std::ostream& stream) noexcept : stream_(&stream) {}

void StreamSink::write(const LogRecord& record) {
    *stream_ << formatRecord(record) << '\n';
}

void MemorySink::write(const LogRecord& record) {
    const std::scoped_lock lock(mutex_);
    records_.push_back(record);
}

std::vector<LogRecord> MemorySink::records() const {
    const std::scoped_lock lock(mutex_);
    return records_;
}

std::size_t MemorySink::size() const {
    const std::scoped_lock lock(mutex_);
    return records_.size();
}

void MemorySink::clear() {
    const std::scoped_lock lock(mutex_);
    records_.clear();
}

}  // namespace recovery::diagnostics
