#include "recovery/candidate_reader.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <string>

namespace recovery {

namespace {

bool isRetryable(storage::ReadStatus status) noexcept {
    return status == storage::ReadStatus::IoError || status == storage::ReadStatus::PartialRead;
}

Error cancelled() {
    return makeError(ErrorCode::Cancelled, "reconstruction was cancelled");
}

class Reconstructor {
public:
    Reconstructor(storage::IStorageSource& source, const RecoveryCandidate& candidate, const CandidateSink& sink,
                  const ReconstructionOptions& options)
        : source_(source), candidate_(candidate), sink_(sink), options_(options) {}

    Result<ReconstructionReport> run() {
        if (Status valid = validate(); !valid.ok()) {
            return valid.error();
        }
        report_.expectedSize = candidate_.expectedSize;
        for (auto region = candidate_.sourceRegions.rbegin(); region != candidate_.sourceRegions.rend(); ++region) {
            if (region->kind != RegionKind::Missing) {
                report_.outputSize = region->fileOffset + region->length;
                break;
            }
        }

        std::uint64_t largestStored = 0;
        for (const SourceRegion& region : candidate_.sourceRegions) {
            if (region.kind == RegionKind::Stored) {
                largestStored = std::max(largestStored, region.length);
            }
        }
        buffer_.resize(static_cast<std::size_t>(std::min<std::uint64_t>(options_.chunkSize, largestStored)));

        for (const SourceRegion& region : candidate_.sourceRegions) {
            if (options_.cancellation.isCancellationRequested()) {
                return cancelled();
            }
            switch (region.kind) {
            case RegionKind::Stored:
                if (Status read = readStored(region); !read.ok()) {
                    return read.error();
                }
                if (region.reallocated) {
                    report_.reallocatedBytes += region.length;
                }
                break;
            case RegionKind::Embedded: {
                // validateCandidate checked the range against embeddedData.
                const std::span<const std::byte> data = std::span<const std::byte>(candidate_.embeddedData)
                                                            .subspan(static_cast<std::size_t>(region.sourceOffset),
                                                                     static_cast<std::size_t>(region.length));
                if (Status delivered = sink_(region.fileOffset, data); !delivered.ok()) {
                    return delivered.error();
                }
                report_.embeddedBytes += region.length;
                break;
            }
            case RegionKind::Zeros:
                report_.zeroBytes += region.length;
                break;
            case RegionKind::Missing:
                report_.missingBytes += region.length;
                break;
            }
        }
        report_.unreadableRegions = unreadable_.regions();
        return std::move(report_);
    }

private:
    Status validate() const {
        if (!source_.isOpen()) {
            return makeError(ErrorCode::InvalidInput, "reconstruction source is not open");
        }
        if (!sink_) {
            return makeError(ErrorCode::InvalidInput, "reconstruction needs a sink");
        }
        if (options_.chunkSize < ReconstructionOptions::kMinChunkSize || options_.chunkSize > storage::kMaxReadSize) {
            return makeError(ErrorCode::InvalidInput, "chunkSize must lie between " +
                                                          std::to_string(ReconstructionOptions::kMinChunkSize) +
                                                          " and " + std::to_string(storage::kMaxReadSize));
        }
        if (options_.sectorRetryCount > ReconstructionOptions::kMaxSectorRetries) {
            return makeError(ErrorCode::InvalidInput, "sectorRetryCount exceeds " +
                                                          std::to_string(ReconstructionOptions::kMaxSectorRetries));
        }
        return validateCandidate(candidate_);
    }

    // Reads a stored region: the part inside the source, skipping known bad
    // regions; what lies beyond the end of the source is unreadable.
    Status readStored(const SourceRegion& region) {
        const std::uint64_t sourceSize = source_.size();
        const std::uint64_t inside =
            region.sourceOffset < sourceSize ? std::min(region.length, sourceSize - region.sourceOffset) : 0;
        const std::uint64_t outside = region.length - inside;
        const std::uint64_t end = region.sourceOffset + inside;  // validateCandidate: no overflow
        const auto fileOffsetOf = [&](std::uint64_t sourceOffset) {
            return region.fileOffset + (sourceOffset - region.sourceOffset);
        };

        std::uint64_t position = region.sourceOffset;
        if (options_.knownBadRegions != nullptr && inside > 0) {
            for (const storage::BadRegion& bad : options_.knownBadRegions->overlapping(position, inside)) {
                const std::uint64_t badStart = std::max(bad.offset, position);
                const std::uint64_t badEnd = std::min(bad.end(), end);
                if (badStart >= badEnd) {
                    continue;
                }
                if (badStart > position) {
                    if (Status read = readRange(position, badStart - position, fileOffsetOf(position)); !read.ok()) {
                        return read;
                    }
                }
                markUnreadable(badStart, badEnd - badStart, bad.errorCode);
                position = badEnd;
            }
        }
        if (position < end) {
            if (Status read = readRange(position, end - position, fileOffsetOf(position)); !read.ok()) {
                return read;
            }
        }
        report_.unreadableBytes += outside;
        report_.outsideSourceBytes += outside;
        return success();
    }

    Status readRange(std::uint64_t offset, std::uint64_t length, std::uint64_t fileOffset) {
        while (length > 0) {
            if (options_.cancellation.isCancellationRequested()) {
                return cancelled();
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(length, buffer_.size()));
            const std::span<std::byte> chunk(buffer_.data(), count);
            const storage::ReadResult read = source_.read(ByteOffset{offset}, chunk);
            if (read.ok()) {
                if (Status delivered = deliver(fileOffset, chunk); !delivered.ok()) {
                    return delivered;
                }
            } else if (isRetryable(read.status)) {
                if (Status narrowed = readSectors(offset, chunk, fileOffset); !narrowed.ok()) {
                    return narrowed;
                }
            } else {
                return storage::toError(read);
            }
            offset += count;
            fileOffset += count;
            length -= count;
        }
        return success();
    }

    // Re-reads a failed chunk one sector at a time; sectors that keep failing
    // are left as zeros and reported.
    Status readSectors(std::uint64_t offset, std::span<std::byte> chunk, std::uint64_t fileOffset) {
        const std::uint64_t sectorSize = storage::isValidSectorSize(source_.sectorSize()) ? source_.sectorSize() : 512;
        const std::uint64_t end = offset + chunk.size();
        std::uint64_t position = offset;
        while (position < end) {
            if (options_.cancellation.isCancellationRequested()) {
                return cancelled();
            }
            const std::uint64_t next = std::min(end, (position / sectorSize + 1) * sectorSize);
            const std::span<std::byte> piece =
                chunk.subspan(static_cast<std::size_t>(position - offset), static_cast<std::size_t>(next - position));
            storage::ReadResult read;
            for (std::uint32_t attempt = 0; attempt <= options_.sectorRetryCount; ++attempt) {
                read = source_.read(ByteOffset{position}, piece);
                if (read.ok() || !isRetryable(read.status)) {
                    break;
                }
            }
            if (read.ok()) {
                if (Status delivered = deliver(fileOffset + (position - offset), piece); !delivered.ok()) {
                    return delivered;
                }
            } else if (isRetryable(read.status)) {
                markUnreadable(position, next - position, read.systemErrorCode);
            } else {
                return storage::toError(read);
            }
            position = next;
        }
        return success();
    }

    Status deliver(std::uint64_t fileOffset, std::span<const std::byte> data) {
        if (Status delivered = sink_(fileOffset, data); !delivered.ok()) {
            return delivered;
        }
        report_.storedBytes += data.size();
        return success();
    }

    void markUnreadable(std::uint64_t offset, std::uint64_t length, std::uint32_t errorCode) {
        report_.unreadableBytes += length;
        // Cannot fail: the range is non-empty and lies inside the source.
        (void)unreadable_.add(storage::BadRegion{offset, length, errorCode});
    }

    storage::IStorageSource& source_;
    const RecoveryCandidate& candidate_;
    const CandidateSink& sink_;
    const ReconstructionOptions& options_;
    ReconstructionReport report_;
    storage::BadRegionMap unreadable_;
    std::vector<std::byte> buffer_;
};

}  // namespace

Result<ReconstructionReport> reconstructCandidate(storage::IStorageSource& source, const RecoveryCandidate& candidate,
                                                  const CandidateSink& sink, const ReconstructionOptions& options) {
    return Reconstructor(source, candidate, sink, options).run();
}

}  // namespace recovery
