#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::diagnostics {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off,
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

// Structured key/value attached to a log record. Values must never contain
// recovered file content; log offsets, sizes and identifiers instead.
struct LogField {
    std::string key;
    std::string value;
};

[[nodiscard]] inline LogField field(std::string_view key, std::string_view value) {
    return LogField{std::string(key), std::string(value)};
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
[[nodiscard]] LogField field(std::string_view key, T value) {
    return LogField{std::string(key), std::to_string(value)};
}

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    std::string component;
    std::string message;
    std::vector<LogField> fields;
};

// Single-line text rendering: `<timestamp> <LEVEL> [component] message key=value ...`.
// Control characters are escaped, so untrusted strings (e.g. file names read
// from disk) cannot forge additional log lines.
[[nodiscard]] std::string formatRecord(const LogRecord& record);

class ILogSink {
public:
    virtual ~ILogSink() = default;
    // Called with the logger's lock held; implementations need no own locking
    // for writes but must not call back into the logger.
    virtual void write(const LogRecord& record) = 0;
};

// Thread safety: all member functions may be called concurrently.
class Logger {
public:
    explicit Logger(LogLevel minLevel = LogLevel::Info) noexcept;

    void addSink(std::shared_ptr<ILogSink> sink);
    void setMinLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel minLevel() const noexcept;
    [[nodiscard]] bool isEnabled(LogLevel level) const noexcept;

    // Never throws; failures inside sinks are swallowed.
    void log(LogLevel level, std::string_view component, std::string_view message,
             std::initializer_list<LogField> fields = {}) noexcept;

private:
    std::atomic<LogLevel> minLevel_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
};

// Writes formatted records to a stream (e.g. std::clog).
class StreamSink final : public ILogSink {
public:
    explicit StreamSink(std::ostream& stream) noexcept;
    void write(const LogRecord& record) override;

private:
    std::ostream* stream_;
};

// Keeps records in memory; intended for tests and diagnostics capture.
class MemorySink final : public ILogSink {
public:
    void write(const LogRecord& record) override;
    [[nodiscard]] std::vector<LogRecord> records() const;
    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
};

}  // namespace recovery::diagnostics
