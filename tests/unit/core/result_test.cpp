#include "recovery/result.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace recovery {
namespace {

Result<int> parsePositive(int value) {
    if (value <= 0) {
        return makeError(ErrorCode::InvalidInput, "value must be positive");
    }
    return value;
}

TEST(ResultTest, HoldsValue) {
    const Result<int> result = parsePositive(42);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(*result, 42);
}

TEST(ResultTest, HoldsError) {
    const Result<int> result = parsePositive(-1);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(result.error().message, "value must be positive");
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
    ASSERT_TRUE(result.ok());
    const std::unique_ptr<int> owned = std::move(result).value();
    ASSERT_NE(owned, nullptr);
    EXPECT_EQ(*owned, 7);
}

TEST(ResultTest, ConvertsCompatibleValues) {
    struct Base {
        virtual ~Base() = default;
    };
    struct Derived : Base {};
    const Result<std::unique_ptr<Base>> result = std::make_unique<Derived>();
    EXPECT_TRUE(result.ok());

    const Result<std::string> text = "literal";
    ASSERT_TRUE(text.ok());
    EXPECT_EQ(text.value(), "literal");
}

TEST(ResultTest, ArrowOperatorReachesMembers) {
    const Result<std::string> result = std::string("abc");
    EXPECT_EQ(result->size(), 3u);
}

TEST(StatusTest, DefaultIsSuccess) {
    const Status status;
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(success().ok());
}

TEST(StatusTest, HoldsError) {
    const Status status = makeError(ErrorCode::IoError, "device failed", 23);
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error().code, ErrorCode::IoError);
    EXPECT_EQ(status.error().systemErrorCode, 23u);
}

TEST(ResultDeathTest, ValueOnErrorTerminates) {
    const Result<int> result = makeError(ErrorCode::InternalError, "boom");
    EXPECT_DEATH({ (void)result.value(); }, "invalid Result access");
}

TEST(ResultDeathTest, ErrorOnSuccessTerminates) {
    const Status status = success();
    EXPECT_DEATH({ (void)status.error(); }, "invalid Result access");
}

}  // namespace
}  // namespace recovery
