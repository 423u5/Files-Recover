#include "diagnostics/logger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace recovery::diagnostics {
namespace {

TEST(LoggerTest, DeliversRecordsWithFields) {
    auto sink = std::make_shared<MemorySink>();
    Logger logger(LogLevel::Debug);
    logger.addSink(sink);

    logger.log(LogLevel::Info, "storage", "opened source", {field("size", 4096), field("path", "disk.img")});

    const auto records = sink->records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, LogLevel::Info);
    EXPECT_EQ(records[0].component, "storage");
    EXPECT_EQ(records[0].message, "opened source");
    ASSERT_EQ(records[0].fields.size(), 2u);
    EXPECT_EQ(records[0].fields[0].key, "size");
    EXPECT_EQ(records[0].fields[0].value, "4096");
}

TEST(LoggerTest, FiltersBelowMinimumLevel) {
    auto sink = std::make_shared<MemorySink>();
    Logger logger(LogLevel::Warning);
    logger.addSink(sink);

    logger.log(LogLevel::Info, "test", "dropped");
    logger.log(LogLevel::Error, "test", "kept");
    EXPECT_EQ(sink->size(), 1u);

    logger.setMinLevel(LogLevel::Off);
    logger.log(LogLevel::Critical, "test", "dropped");
    EXPECT_EQ(sink->size(), 1u);
    EXPECT_FALSE(logger.isEnabled(LogLevel::Off));
}

TEST(LoggerTest, ThrowingSinkDoesNotPropagate) {
    struct ThrowingSink final : ILogSink {
        void write(const LogRecord&) override { throw std::runtime_error("sink failure"); }
    };
    auto memory = std::make_shared<MemorySink>();
    Logger logger;
    logger.addSink(std::make_shared<ThrowingSink>());
    logger.addSink(memory);

    logger.log(LogLevel::Error, "test", "still delivered");
    EXPECT_EQ(memory->size(), 1u);
}

TEST(LoggerTest, FormatEscapesControlCharacters) {
    LogRecord record;
    record.level = LogLevel::Warning;
    record.component = "fs";
    record.message = "name\nFAKE ERROR line";
    record.fields = {field("file", "a b\r\n\"c\""), field("n", 5)};

    const std::string line = formatRecord(record);
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find('\r'), std::string::npos);
    EXPECT_NE(line.find("WARN [fs] name\\nFAKE ERROR line"), std::string::npos) << line;
    EXPECT_NE(line.find("file=\"a b\\r\\n\\\"c\\\"\""), std::string::npos) << line;
    EXPECT_NE(line.find(" n=5"), std::string::npos) << line;
}

TEST(LoggerTest, StreamSinkWritesOneLinePerRecord) {
    std::ostringstream stream;
    Logger logger;
    logger.addSink(std::make_shared<StreamSink>(stream));
    logger.log(LogLevel::Info, "core", "first");
    logger.log(LogLevel::Info, "core", "second");

    const std::string text = stream.str();
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2);
    EXPECT_NE(text.find("INFO [core] first"), std::string::npos);
}

TEST(LoggerTest, ConcurrentLoggingIsSafe) {
    auto sink = std::make_shared<MemorySink>();
    Logger logger;
    logger.addSink(sink);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < kPerThread; ++i) {
                logger.log(LogLevel::Info, "worker", "tick", {field("thread", t), field("i", i)});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(sink->size(), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace
}  // namespace recovery::diagnostics
