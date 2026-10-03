#include "recovery/candidate_content.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace recovery {

Result<std::unique_ptr<CandidateContentReader>> CandidateContentReader::open(storage::IStorageSource& source,
                                                                             const RecoveryCandidate& candidate,
                                                                             carving::SourceReadOptions options,
                                                                             std::size_t cacheSize) {
    if (Status valid = validateCandidate(candidate); !valid.ok()) {
        return valid.error();
    }
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "candidate content source is not open");
    }
    // As in reconstruction, the file ends with its last region that is not Missing.
    std::uint64_t size = 0;
    for (const SourceRegion& region : candidate.sourceRegions) {
        if (region.kind != RegionKind::Missing) {
            size = region.fileOffset + region.length;
        }
    }
    const std::uint64_t sourceSize = source.size();
    Result<std::unique_ptr<carving::SourceContentReader>> reader =
        carving::SourceContentReader::open(source, 0, sourceSize, std::move(options), cacheSize);
    if (!reader.ok()) {
        return reader.error();
    }
    return std::unique_ptr<CandidateContentReader>(new CandidateContentReader(
        std::move(*reader), sourceSize, candidate.sourceRegions, candidate.embeddedData, size));
}

CandidateContentReader::CandidateContentReader(std::unique_ptr<carving::SourceContentReader> source,
                                               std::uint64_t sourceSize, std::vector<SourceRegion> regions,
                                               std::vector<std::byte> embedded, std::uint64_t size)
    : source_(std::move(source)),
      sourceSize_(sourceSize),
      regions_(std::move(regions)),
      embedded_(std::move(embedded)),
      size_(size) {}

std::size_t CandidateContentReader::regionAt(std::uint64_t offset) const noexcept {
    // Regions are contiguous from 0 (validateCandidate): the last one starting at or before `offset`.
    const auto startsAfter = [](std::uint64_t value, const SourceRegion& r) { return value < r.fileOffset; };
    const auto after = std::upper_bound(regions_.begin(), regions_.end(), offset, startsAfter);
    return static_cast<std::size_t>(std::distance(regions_.begin(), after)) - 1;
}

void CandidateContentReader::noteOutside(std::uint64_t fileOffset, std::uint64_t length) {
    const std::uint64_t end = fileOffset + length;
    for (auto& range : outside_) {
        if (fileOffset <= range.second && range.first <= end) {
            range.first = std::min(range.first, fileOffset);
            range.second = std::max(range.second, end);
            return;
        }
    }
    outside_.emplace_back(fileOffset, end);
    std::sort(outside_.begin(), outside_.end());
}

Result<std::span<const std::byte>> CandidateContentReader::read(std::uint64_t offset, std::size_t length) {
    if (length > kMaxReadLength) {
        return makeError(ErrorCode::InvalidInput, "content read of " + std::to_string(length) +
                                                      " bytes exceeds the limit of " + std::to_string(kMaxReadLength));
    }
    if (!rangeWithin<std::uint64_t>(offset, length, size_)) {
        return makeError(ErrorCode::InvalidInput, "content read [" + std::to_string(offset) + ", +" +
                                                      std::to_string(length) + ") lies beyond the " +
                                                      std::to_string(size_) + " bytes of the candidate");
    }
    if (length == 0) {
        return std::span<const std::byte>{};
    }
    std::size_t index = regionAt(offset);
    // Inside one stored region and inside the source: the source reader's own view.
    const SourceRegion& first = regions_[index];
    if (first.kind == RegionKind::Stored && offset + length <= first.fileOffset + first.length) {
        const std::uint64_t at = first.sourceOffset + (offset - first.fileOffset);
        if (at <= sourceSize_ && length <= sourceSize_ - at) {
            return source_->read(at, length);
        }
    }
    buffer_.assign(length, std::byte{0});
    std::uint64_t position = offset;
    const std::uint64_t end = offset + length;
    while (position < end && index < regions_.size()) {
        const SourceRegion& region = regions_[index];
        const std::uint64_t pieceEnd = std::min(end, region.fileOffset + region.length);
        const std::uint64_t pieceLength = pieceEnd - position;
        const std::uint64_t within = position - region.fileOffset;
        std::byte* target = buffer_.data() + (position - offset);
        switch (region.kind) {
        case RegionKind::Stored: {
            const std::uint64_t at = region.sourceOffset + within;
            const std::uint64_t inside = at >= sourceSize_ ? 0 : std::min(pieceLength, sourceSize_ - at);
            if (inside > 0) {
                Result<std::span<const std::byte>> bytes = source_->read(at, static_cast<std::size_t>(inside));
                if (!bytes.ok()) {
                    return bytes.error();
                }
                std::copy(bytes->begin(), bytes->end(), target);
            }
            if (inside < pieceLength) {
                noteOutside(position + inside, pieceLength - inside);
            }
            break;
        }
        case RegionKind::Embedded: {
            // validateCandidate: Embedded regions lie inside the embedded data.
            const auto from = static_cast<std::ptrdiff_t>(region.sourceOffset + within);
            std::copy(embedded_.begin() + from, embedded_.begin() + from + static_cast<std::ptrdiff_t>(pieceLength),
                      target);
            break;
        }
        case RegionKind::Zeros:
        case RegionKind::Missing:
            break;
        }
        position = pieceEnd;
        ++index;
    }
    return std::span<const std::byte>(buffer_);
}

}  // namespace recovery
