#include "recovery/config.hpp"

#include <algorithm>
#include <bit>
#include <string>
#include <thread>

namespace recovery {

std::string_view toString(ScanMode mode) noexcept {
    switch (mode) {
    case ScanMode::Quick:
        return "quick";
    case ScanMode::Deep:
        return "deep";
    }
    return "unknown";
}

std::optional<ScanMode> parseScanMode(std::string_view text) noexcept {
    if (text == "quick") {
        return ScanMode::Quick;
    }
    if (text == "deep") {
        return ScanMode::Deep;
    }
    return std::nullopt;
}

Status validate(const EngineConfig& config) {
    const std::size_t block = config.io.blockSize;
    if (block < IoConfig::kMinBlockSize || block > IoConfig::kMaxBlockSize || !std::has_single_bit(block)) {
        return makeError(ErrorCode::InvalidInput,
                         "io.blockSize must be a power of two between " + std::to_string(IoConfig::kMinBlockSize) +
                             " and " + std::to_string(IoConfig::kMaxBlockSize) + " bytes");
    }
    if (config.workerThreads > EngineConfig::kMaxWorkerThreads) {
        return makeError(ErrorCode::InvalidInput,
                         "workerThreads must not exceed " + std::to_string(EngineConfig::kMaxWorkerThreads));
    }
    return success();
}

std::uint32_t effectiveWorkerThreads(const EngineConfig& config) noexcept {
    std::uint32_t threads = config.workerThreads;
    if (threads == 0) {
        // hardware_concurrency() may legitimately return 0 ("unknown").
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    return std::clamp(threads, 1U, EngineConfig::kMaxWorkerThreads);
}

}  // namespace recovery
