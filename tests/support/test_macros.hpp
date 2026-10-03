#pragma once

#include "recovery/result.hpp"

#include <gtest/gtest.h>

// Assert that a Status/Result succeeded, printing the error when it did not.
#define RECOVERY_ASSERT_OK(expr)                                                      \
    do {                                                                              \
        const auto& recoveryAssertOkValue_ = (expr);                                  \
        ASSERT_TRUE(recoveryAssertOkValue_.ok())                                      \
            << ::recovery::describe(recoveryAssertOkValue_.error());                  \
    } while (false)

#define RECOVERY_EXPECT_OK(expr)                                                      \
    do {                                                                              \
        const auto& recoveryExpectOkValue_ = (expr);                                  \
        EXPECT_TRUE(recoveryExpectOkValue_.ok())                                      \
            << ::recovery::describe(recoveryExpectOkValue_.error());                  \
    } while (false)

// Assert that a Status/Result failed with the given ErrorCode.
#define RECOVERY_EXPECT_ERROR(expr, expectedCode)                                     \
    do {                                                                              \
        const auto& recoveryExpectErrValue_ = (expr);                                 \
        ASSERT_FALSE(recoveryExpectErrValue_.ok());                                   \
        EXPECT_EQ(recoveryExpectErrValue_.error().code, (expectedCode))               \
            << ::recovery::describe(recoveryExpectErrValue_.error());                 \
    } while (false)
