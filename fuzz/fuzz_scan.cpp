// Fuzz target: a whole deep scan of the input as a disk: partitions,
// filesystems and their candidates, MP4 recovery, fragment reconstruction,
// the source pass with every format, and the evaluation of every candidate
// (validation at every level the engine runs itself, SHA-256, duplicates).
// The slowest target: keep inputs small (-max_len).

#include "fuzz_support.hpp"
#include "scan/scan_coordinator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    fuzz::FuzzSource source(fuzz::bytesOf(data, size));
    fuzz::require(source.open().ok(), "opening the source");
    scan::ScanConfiguration configuration;
    configuration.mode = ScanMode::Deep;
    scan::ScanRunOptions options;
    options.workerThreads = 1;
    options.blockSize = 64 * 1024;
    options.cacheBlocks = 4;
    options.progressInterval = std::chrono::milliseconds{0};
    scan::ScanCoordinator coordinator(source, fuzz::allFormats(), fuzz::allMedia(), configuration, options);
    std::uint64_t candidates = 0;
    const Result<scan::ScanSummary> summary = coordinator.run([&](const scan::ScanUpdate& update) {
        candidates += update.candidates.size();
        return success();
    });
    fuzz::require(summary.ok(), "a scan of an in-memory source failed");
    return 0;
}
