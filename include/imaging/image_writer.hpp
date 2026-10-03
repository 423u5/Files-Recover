#pragma once

#include "diagnostics/logger.hpp"
#include "imaging/image_metadata.hpp"
#include "recovery/cancellation.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/destination_file.hpp"
#include "storage/destination_guard.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace recovery::imaging {

struct ImagingProgress {
    std::uint64_t bytesCompleted = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t unreadableBytes = 0;
    std::size_t badRegionCount = 0;
    std::uint64_t resumedFrom = 0;
    std::chrono::milliseconds elapsed{0};
};

// Invoked on the imaging thread. Must not throw (exceptions are swallowed).
using ProgressCallback = std::function<void(const ImagingProgress&)>;

struct ImagingOptions {
    static constexpr std::uint32_t kMaxSectorRetries = 16;

    // Size of normal sequential reads. Must be a multiple of the source
    // sector size and at most storage::kMaxReadSize.
    std::size_t blockSize = 1 * kMiB;
    // After a failed block, the block is re-read in pieces of this size
    // before falling back to single sectors. Ignored unless it lies strictly
    // between the sector size and blockSize. Must be a sector multiple.
    std::size_t intermediateBlockSize = 64 * kKiB;
    // Additional attempts for a sector before it is recorded as unreadable.
    std::uint32_t sectorRetryCount = 2;
    // Image data is flushed and metadata committed at least this often.
    std::uint64_t checkpointIntervalBytes = 64 * kMiB;
    // Continue an interrupted image instead of creating a new one.
    bool resume = false;

    std::chrono::milliseconds progressInterval{250};
    ProgressCallback onProgress;
    CancellationToken cancellation;
    diagnostics::Logger* logger = nullptr;
    // Determines which physical disk a destination lives on. Empty selects
    // the platform resolver.
    storage::DiskResolver diskResolver;
};

enum class ImagingOutcome : std::uint8_t {
    Completed,
    Cancelled,
};

struct ImagingSummary {
    ImagingOutcome outcome = ImagingOutcome::Completed;
    std::uint64_t bytesCompleted = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t resumedFrom = 0;
    std::uint64_t unreadableBytes = 0;
    std::vector<storage::BadRegion> badRegions;
    std::filesystem::path imagePath;
    std::filesystem::path metadataPath;
};

// Creates a raw image of an open source.
//
// Reads the source strictly sequentially and never writes to it. Read
// failures are narrowed down (block -> intermediate block -> sector); each
// sector that still fails after retries is zero-filled in the image and
// recorded as a BadRegion. Imaging continues after bad sectors; only
// non-I/O failures (destination errors, invalid requests) stop it.
//
// Destination safety: refuses to create the image if it already exists
// (unless resuming), if it is a device path, if it is the source image
// itself, or if it lives on the source physical disk.
//
// Thread safety: run() executes on the calling thread. Cancellation may be
// requested from any thread through the token in ImagingOptions.
class ImageWriter {
public:
    ImageWriter(storage::IStorageSource& source, std::filesystem::path imagePath, ImagingOptions options);

    [[nodiscard]] Result<ImagingSummary> run();

private:
    struct Prepared {
        storage::DestinationFile file;
        ImageMetadata metadata;
    };

    [[nodiscard]] Status validate() const;
    [[nodiscard]] Result<Prepared> prepareNew(const storage::SourceInfo& info);
    [[nodiscard]] Result<Prepared> prepareResume(const storage::SourceInfo& info);
    [[nodiscard]] Status imageRange(std::uint64_t offset, std::span<std::byte> buffer, std::size_t level);
    [[nodiscard]] Status recordBadRegion(std::uint64_t offset, std::span<std::byte> buffer, std::uint32_t errorCode);
    [[nodiscard]] Status checkpoint(storage::DestinationFile& file, ImageMetadata& metadata, std::uint64_t position,
                                    ImageState state);
    // Bad regions clipped to [0, position): the committed part of the image.
    [[nodiscard]] std::vector<storage::BadRegion> badRegionsBefore(std::uint64_t position) const;
    void reportProgress(std::uint64_t position, std::uint64_t total, bool force);
    void log(diagnostics::LogLevel level, std::string_view message,
             std::initializer_list<diagnostics::LogField> fields = {}) const;

    storage::IStorageSource& source_;
    std::filesystem::path imagePath_;
    std::filesystem::path metadataPath_;
    ImagingOptions options_;

    std::vector<std::size_t> levels_;  // read granularities, largest first, ending with the sector size
    storage::BadRegionMap badRegions_;
    std::uint64_t resumedFrom_ = 0;
    std::uint64_t badSectorsLogged_ = 0;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point lastProgress_;
};

}  // namespace recovery::imaging
