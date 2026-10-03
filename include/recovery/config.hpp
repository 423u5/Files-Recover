#pragma once

#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace recovery {

inline constexpr std::size_t kKiB = 1024;
inline constexpr std::size_t kMiB = 1024 * kKiB;
inline constexpr std::size_t kGiB = 1024 * kMiB;

enum class ScanMode : std::uint8_t {
    Quick,
    Deep,
};

[[nodiscard]] std::string_view toString(ScanMode mode) noexcept;
[[nodiscard]] std::optional<ScanMode> parseScanMode(std::string_view text) noexcept;

struct IoConfig {
    static constexpr std::size_t kMinBlockSize = 4 * kKiB;
    static constexpr std::size_t kMaxBlockSize = 64 * kMiB;

    // Size of sequential reads. Must be a power of two within the limits above.
    std::size_t blockSize = 1 * kMiB;
};

struct EngineConfig {
    static constexpr std::uint32_t kMaxWorkerThreads = 64;

    IoConfig io;
    // 0 selects a value derived from the hardware concurrency.
    std::uint32_t workerThreads = 0;
};

[[nodiscard]] Status validate(const EngineConfig& config);

// Worker count to use for `config`, always within [1, kMaxWorkerThreads].
[[nodiscard]] std::uint32_t effectiveWorkerThreads(const EngineConfig& config) noexcept;

}  // namespace recovery
