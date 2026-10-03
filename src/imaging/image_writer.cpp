#include "imaging/image_writer.hpp"

#include "recovery/text.hpp"
#include "recovery/version.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace recovery::imaging {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "imaging";
// Individual bad-sector warnings logged before switching to a summary only.
constexpr std::uint64_t kMaxLoggedBadSectors = 100;

bool isRetryable(storage::ReadStatus status) noexcept {
    return status == storage::ReadStatus::IoError || status == storage::ReadStatus::PartialRead;
}

std::string nowUtc() {
    return formatUtcTimestamp(std::chrono::system_clock::now());
}

}  // namespace

ImageWriter::ImageWriter(storage::IStorageSource& source, std::filesystem::path imagePath, ImagingOptions options)
    : source_(source),
      imagePath_(std::move(imagePath)),
      metadataPath_(metadataPathFor(imagePath_)),
      options_(std::move(options)) {}

Status ImageWriter::validate() const {
    if (!source_.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "imaging source is not open");
    }
    const std::size_t sector = source_.sectorSize();
    if (!storage::isValidSectorSize(sector)) {
        return makeError(ErrorCode::InvalidInput, "imaging source has an invalid sector size");
    }
    if (options_.blockSize < sector || options_.blockSize > storage::kMaxReadSize ||
        options_.blockSize % sector != 0) {
        return makeError(ErrorCode::InvalidInput, "blockSize must be a multiple of the sector size (" +
                                                      std::to_string(sector) + ") and at most " +
                                                      std::to_string(storage::kMaxReadSize));
    }
    if (options_.intermediateBlockSize % sector != 0) {
        return makeError(ErrorCode::InvalidInput, "intermediateBlockSize must be a multiple of the sector size");
    }
    if (options_.sectorRetryCount > ImagingOptions::kMaxSectorRetries) {
        return makeError(ErrorCode::InvalidInput, "sectorRetryCount exceeds " +
                                                      std::to_string(ImagingOptions::kMaxSectorRetries));
    }
    if (imagePath_.empty()) {
        return makeError(ErrorCode::InvalidInput, "image path is empty");
    }
    return success();
}

Result<ImagingSummary> ImageWriter::run() {
    if (Status valid = validate(); !valid.ok()) {
        return valid.error();
    }

    const std::size_t sector = source_.sectorSize();
    levels_.clear();
    levels_.push_back(options_.blockSize);
    if (options_.intermediateBlockSize > sector && options_.intermediateBlockSize < options_.blockSize) {
        levels_.push_back(options_.intermediateBlockSize);
    }
    if (levels_.back() != sector) {
        levels_.push_back(sector);
    }
    badRegions_.clear();
    badSectorsLogged_ = 0;
    resumedFrom_ = 0;

    const storage::SourceInfo info = source_.getInfo();
    const storage::DiskResolver resolver =
        options_.diskResolver ? options_.diskResolver : storage::makePlatformDiskResolver();
    for (const std::filesystem::path& target : {imagePath_, metadataPath_}) {
        if (Status safe = storage::checkDestinationSafety(info, target, resolver); !safe.ok()) {
            return safe.error();
        }
    }

    Result<Prepared> prepared = options_.resume ? prepareResume(info) : prepareNew(info);
    if (!prepared.ok()) {
        return prepared.error();
    }
    storage::DestinationFile file = std::move(prepared->file);
    ImageMetadata metadata = std::move(prepared->metadata);

    const std::uint64_t total = metadata.sourceSize;
    started_ = std::chrono::steady_clock::now();
    lastProgress_ = started_;
    log(LogLevel::Info, options_.resume ? "imaging resumed" : "imaging started",
        {field("source", toUtf8(info.path)), field("image", toUtf8(imagePath_)), field("size", total),
         field("sector_size", sector), field("block_size", options_.blockSize), field("resume_offset", resumedFrom_)});

    std::vector<std::byte> buffer(options_.blockSize);
    std::uint64_t position = metadata.bytesCompleted;
    std::uint64_t lastCheckpoint = position;
    ImagingOutcome outcome = ImagingOutcome::Completed;

    while (position < total) {
        if (options_.cancellation.isCancellationRequested()) {
            outcome = ImagingOutcome::Cancelled;
            break;
        }
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(options_.blockSize, total - position));
        const std::span<std::byte> block(buffer.data(), length);

        if (Status read = imageRange(position, block, 0); !read.ok()) {
            if (read.error().code == ErrorCode::Cancelled) {
                // The partially processed block is discarded; resume redoes it.
                outcome = ImagingOutcome::Cancelled;
                break;
            }
            (void)checkpoint(file, metadata, position, ImageState::Failed);
            log(LogLevel::Error, "imaging failed", {field("offset", position), field("error", describe(read.error()))});
            return read.error();
        }
        if (Status written = file.writeAt(position, block); !written.ok()) {
            (void)checkpoint(file, metadata, position, ImageState::Failed);
            log(LogLevel::Error, "writing image failed",
                {field("offset", position), field("error", describe(written.error()))});
            return written.error();
        }
        position += length;

        if (position < total && position - lastCheckpoint >= options_.checkpointIntervalBytes) {
            if (Status saved = checkpoint(file, metadata, position, ImageState::InProgress); !saved.ok()) {
                return saved.error();
            }
            lastCheckpoint = position;
        }
        reportProgress(position, total, false);
    }

    if (outcome == ImagingOutcome::Cancelled) {
        // Forget bad sectors found in the discarded partial block; resume re-reads them.
        storage::BadRegionMap committed;
        for (const storage::BadRegion& region : badRegionsBefore(position)) {
            (void)committed.add(region);
        }
        badRegions_ = std::move(committed);
    }

    const ImageState finalState = outcome == ImagingOutcome::Completed ? ImageState::Completed : ImageState::Cancelled;
    if (Status saved = checkpoint(file, metadata, position, finalState); !saved.ok()) {
        return saved.error();
    }
    file.close();
    reportProgress(position, total, true);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_);
    log(outcome == ImagingOutcome::Completed ? LogLevel::Info : LogLevel::Warning,
        outcome == ImagingOutcome::Completed ? "imaging completed" : "imaging cancelled",
        {field("bytes_completed", position), field("unreadable_bytes", badRegions_.totalBytes()),
         field("bad_regions", badRegions_.size()), field("elapsed_ms", elapsed.count())});

    ImagingSummary summary;
    summary.outcome = outcome;
    summary.bytesCompleted = position;
    summary.totalBytes = total;
    summary.resumedFrom = resumedFrom_;
    summary.unreadableBytes = badRegions_.totalBytes();
    summary.badRegions = badRegions_.regions();
    summary.imagePath = imagePath_;
    summary.metadataPath = metadataPath_;
    return summary;
}

Result<ImageWriter::Prepared> ImageWriter::prepareNew(const storage::SourceInfo& info) {
    std::error_code ec;
    if (std::filesystem::exists(metadataPath_, ec)) {
        return makeError(ErrorCode::DestinationError,
                         "image metadata '" + toUtf8(metadataPath_) + "' already exists; refusing to overwrite");
    }

    Result<storage::DestinationFile> file =
        storage::DestinationFile::open(imagePath_, storage::DestinationFile::OpenMode::CreateNew);
    if (!file.ok()) {
        return file.error();
    }

    ImageMetadata metadata;
    metadata.engineVersion = std::string(kEngineVersion);
    metadata.state = ImageState::InProgress;
    metadata.sourceType = info.type;
    metadata.sourcePath = toUtf8(info.path);
    metadata.sourceVendor = info.vendor;
    metadata.sourceProduct = info.product;
    metadata.sourceSize = source_.size();
    metadata.sectorSize = source_.sectorSize();
    metadata.blockSize = options_.blockSize;
    metadata.bytesCompleted = 0;
    metadata.startedUtc = nowUtc();
    metadata.updatedUtc = metadata.startedUtc;

    if (Status saved = writeImageMetadata(metadataPath_, metadata); !saved.ok()) {
        // Remove only the empty image this call just created.
        file->close();
        std::filesystem::remove(imagePath_, ec);
        return saved.error();
    }
    return Prepared{std::move(file).value(), std::move(metadata)};
}

Result<ImageWriter::Prepared> ImageWriter::prepareResume(const storage::SourceInfo& info) {
    Result<ImageMetadata> loaded = readImageMetadata(metadataPath_);
    if (!loaded.ok()) {
        return makeError(loaded.error().code, "cannot resume: " + loaded.error().message,
                         loaded.error().systemErrorCode);
    }
    ImageMetadata metadata = std::move(loaded).value();

    if (metadata.state == ImageState::Completed) {
        return makeError(ErrorCode::InvalidInput, "cannot resume: the image is already complete");
    }
    if (metadata.sourceType != info.type || metadata.sourcePath != toUtf8(info.path) ||
        metadata.sourceSize != source_.size() || metadata.sectorSize != source_.sectorSize()) {
        return makeError(ErrorCode::InvalidInput, "cannot resume: the image metadata describes a different source");
    }
    if (metadata.bytesCompleted != metadata.sourceSize && metadata.bytesCompleted % metadata.sectorSize != 0) {
        return makeError(ErrorCode::InvalidFormat, "cannot resume: recorded progress is not sector-aligned");
    }

    Result<storage::DestinationFile> file =
        storage::DestinationFile::open(imagePath_, storage::DestinationFile::OpenMode::OpenExisting);
    if (!file.ok()) {
        return file.error();
    }
    const Result<std::uint64_t> size = file->size();
    if (!size.ok()) {
        return size.error();
    }
    if (size.value() < metadata.bytesCompleted) {
        return makeError(ErrorCode::InvalidFormat, "cannot resume: the image is shorter than its recorded progress");
    }
    // Data past the last checkpoint was never committed; discard it.
    if (Status truncated = file->truncate(metadata.bytesCompleted); !truncated.ok()) {
        return truncated.error();
    }

    for (const storage::BadRegion& region : metadata.badRegions) {
        if (region.offset >= metadata.bytesCompleted) {
            continue;
        }
        storage::BadRegion kept = region;
        kept.length = std::min(region.length, metadata.bytesCompleted - region.offset);
        if (Status added = badRegions_.add(kept); !added.ok()) {
            return added.error();
        }
    }

    resumedFrom_ = metadata.bytesCompleted;
    metadata.state = ImageState::InProgress;
    metadata.blockSize = options_.blockSize;
    metadata.badRegions = badRegions_.regions();
    metadata.updatedUtc = nowUtc();
    if (Status saved = writeImageMetadata(metadataPath_, metadata); !saved.ok()) {
        return saved.error();
    }
    return Prepared{std::move(file).value(), std::move(metadata)};
}

Status ImageWriter::imageRange(std::uint64_t offset, std::span<std::byte> buffer, std::size_t level) {
    storage::ReadResult result = source_.read(ByteOffset{offset}, buffer);
    if (result.ok()) {
        return success();
    }
    if (!isRetryable(result.status)) {
        return storage::toError(result);
    }

    const bool sectorLevel = level + 1 >= levels_.size();
    if (sectorLevel) {
        for (std::uint32_t attempt = 0; attempt < options_.sectorRetryCount; ++attempt) {
            if (options_.cancellation.isCancellationRequested()) {
                return makeError(ErrorCode::Cancelled, "imaging cancelled");
            }
            result = source_.read(ByteOffset{offset}, buffer);
            if (result.ok()) {
                return success();
            }
            if (!isRetryable(result.status)) {
                return storage::toError(result);
            }
        }
        return recordBadRegion(offset, buffer, result.systemErrorCode);
    }

    const std::size_t step = levels_[level + 1];
    for (std::size_t done = 0; done < buffer.size(); done += step) {
        if (options_.cancellation.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "imaging cancelled");
        }
        const std::size_t length = std::min(step, buffer.size() - done);
        if (Status status = imageRange(offset + done, buffer.subspan(done, length), level + 1); !status.ok()) {
            return status;
        }
    }
    return success();
}

Status ImageWriter::recordBadRegion(std::uint64_t offset, std::span<std::byte> buffer, std::uint32_t errorCode) {
    // Unreadable data is represented by zeros in the image; the metadata
    // records exactly which ranges those are.
    std::memset(buffer.data(), 0, buffer.size());
    if (Status added = badRegions_.add(storage::BadRegion{offset, buffer.size(), errorCode}); !added.ok()) {
        return added;
    }
    if (badSectorsLogged_ < kMaxLoggedBadSectors) {
        log(LogLevel::Warning, "unreadable sector",
            {field("offset", offset), field("length", buffer.size()), field("error_code", errorCode)});
    } else if (badSectorsLogged_ == kMaxLoggedBadSectors) {
        log(LogLevel::Warning, "further unreadable sectors are recorded in the image metadata only");
    }
    ++badSectorsLogged_;
    return success();
}

Status ImageWriter::checkpoint(storage::DestinationFile& file, ImageMetadata& metadata, std::uint64_t position,
                               ImageState state) {
    // Image data must be durable before metadata claims it is complete.
    if (Status flushed = file.flush(); !flushed.ok()) {
        return flushed;
    }
    metadata.bytesCompleted = position;
    metadata.state = state;
    // Only regions inside the committed part of the image describe the image.
    metadata.badRegions = badRegionsBefore(position);
    metadata.updatedUtc = nowUtc();
    return writeImageMetadata(metadataPath_, metadata);
}

std::vector<storage::BadRegion> ImageWriter::badRegionsBefore(std::uint64_t position) const {
    std::vector<storage::BadRegion> committed = badRegions_.overlapping(0, position);
    for (storage::BadRegion& region : committed) {
        region.length = std::min(region.end(), position) - region.offset;
    }
    return committed;
}

void ImageWriter::reportProgress(std::uint64_t position, std::uint64_t total, bool force) {
    if (!options_.onProgress) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastProgress_ < options_.progressInterval) {
        return;
    }
    lastProgress_ = now;

    ImagingProgress progress;
    progress.bytesCompleted = position;
    progress.totalBytes = total;
    progress.unreadableBytes = badRegions_.totalBytes();
    progress.badRegionCount = badRegions_.size();
    progress.resumedFrom = resumedFrom_;
    progress.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_);
    try {
        options_.onProgress(progress);
    } catch (...) {
        log(LogLevel::Warning, "progress callback threw an exception");
    }
}

void ImageWriter::log(LogLevel level, std::string_view message,
                      std::initializer_list<diagnostics::LogField> fields) const {
    if (options_.logger != nullptr) {
        options_.logger->log(level, kComponent, message, fields);
    }
}

}  // namespace recovery::imaging
