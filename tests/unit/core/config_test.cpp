#include "recovery/config.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

namespace recovery {
namespace {

TEST(ConfigTest, DefaultsAreValid) {
    RECOVERY_EXPECT_OK(validate(EngineConfig{}));
}

TEST(ConfigTest, RejectsBlockSizeOutOfRange) {
    EngineConfig config;
    config.io.blockSize = IoConfig::kMinBlockSize / 2;
    RECOVERY_EXPECT_ERROR(validate(config), ErrorCode::InvalidInput);

    config.io.blockSize = IoConfig::kMaxBlockSize * 2;
    RECOVERY_EXPECT_ERROR(validate(config), ErrorCode::InvalidInput);
}

TEST(ConfigTest, RejectsNonPowerOfTwoBlockSize) {
    EngineConfig config;
    config.io.blockSize = 3 * kMiB;
    RECOVERY_EXPECT_ERROR(validate(config), ErrorCode::InvalidInput);
}

TEST(ConfigTest, RejectsTooManyWorkers) {
    EngineConfig config;
    config.workerThreads = EngineConfig::kMaxWorkerThreads + 1;
    RECOVERY_EXPECT_ERROR(validate(config), ErrorCode::InvalidInput);
}

TEST(ConfigTest, EffectiveWorkerThreadsIsBounded) {
    EngineConfig config;
    config.workerThreads = 0;
    const std::uint32_t automatic = effectiveWorkerThreads(config);
    EXPECT_GE(automatic, 1u);
    EXPECT_LE(automatic, EngineConfig::kMaxWorkerThreads);

    config.workerThreads = 3;
    EXPECT_EQ(effectiveWorkerThreads(config), 3u);
}

TEST(ConfigTest, ScanModeRoundTrips) {
    EXPECT_EQ(parseScanMode(toString(ScanMode::Quick)), ScanMode::Quick);
    EXPECT_EQ(parseScanMode(toString(ScanMode::Deep)), ScanMode::Deep);
    EXPECT_FALSE(parseScanMode("DEEP").has_value());
    EXPECT_FALSE(parseScanMode("").has_value());
}

}  // namespace
}  // namespace recovery
