#include "recovery/text.hpp"

#include <gtest/gtest.h>

namespace recovery {
namespace {

TEST(TextTest, PathToUtf8HandlesNonAscii) {
    const std::filesystem::path path(L"D:\\Fotos\\\u00DCber \u5199\u771F\\\U0001F600.jpg");
    EXPECT_EQ(toUtf8(path), "D:\\Fotos\\\xC3\x9C" "ber \xE5\x86\x99\xE7\x9C\x9F\\\xF0\x9F\x98\x80.jpg");
}

TEST(TextTest, UtcTimestampFormat) {
    const auto time = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{1234};
    EXPECT_EQ(formatUtcTimestamp(time), "1970-01-01T00:00:01.234Z");

    const auto later = std::chrono::sys_days{std::chrono::year{2026} / 9 / 19} + std::chrono::hours{8} +
                       std::chrono::minutes{15} + std::chrono::seconds{30};
    EXPECT_EQ(formatUtcTimestamp(later), "2026-09-19T08:15:30.000Z");
}

}  // namespace
}  // namespace recovery
