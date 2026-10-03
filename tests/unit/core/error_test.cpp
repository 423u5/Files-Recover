#include "recovery/error.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace recovery {
namespace {

TEST(ErrorTest, EveryCodeHasDistinctName) {
    const ErrorCode codes[] = {
        ErrorCode::InvalidInput,       ErrorCode::IoError,       ErrorCode::UnreadableRegion,
        ErrorCode::CorruptedFilesystem, ErrorCode::UnsupportedFilesystem, ErrorCode::InvalidFormat,
        ErrorCode::PartialRecovery,    ErrorCode::DestinationError, ErrorCode::Cancelled,
        ErrorCode::InternalError,
    };
    std::set<std::string> names;
    for (const ErrorCode code : codes) {
        const std::string name(toString(code));
        EXPECT_NE(name, "UNKNOWN_ERROR");
        EXPECT_TRUE(names.insert(name).second) << "duplicate name " << name;
    }
}

TEST(ErrorTest, NamesMatchSpecification) {
    EXPECT_EQ(toString(ErrorCode::InvalidInput), "INVALID_INPUT");
    EXPECT_EQ(toString(ErrorCode::UnreadableRegion), "UNREADABLE_REGION");
    EXPECT_EQ(toString(ErrorCode::Cancelled), "CANCELLED");
}

TEST(ErrorTest, DescribeIncludesMessageAndSystemCode) {
    const Error error = makeError(ErrorCode::IoError, "read failed", 23);
    EXPECT_EQ(describe(error), "IO_ERROR: read failed (system error 23)");
}

TEST(ErrorTest, DescribeOmitsEmptyParts) {
    EXPECT_EQ(describe(makeError(ErrorCode::Cancelled, "")), "CANCELLED");
}

}  // namespace
}  // namespace recovery
