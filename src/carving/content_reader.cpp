#include "carving/content_reader.hpp"

#include "recovery/checked_math.hpp"
#include "source_reads.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace recovery::carving {

namespace {

// Chunk size of findPattern.
constexpr std::size_t kSearchChunk = 256 * kKiB;

Status checkRequest(std::uint64_t offset, std::size_t length, std::uint64_t size) {
    if (length > IContentReader::kMaxReadLength) {
        return makeError(ErrorCode::InvalidInput, "content read of " + std::to_string(length) +
                                                      " bytes exceeds the limit of " +
                                                      std::to_string(IContentReader::kMaxReadLength));
    }
    if (!rangeWithin<std::uint64_t>(offset, length, size)) {
        return makeError(ErrorCode::InvalidInput, "content read [" + std::to_string(offset) + ", +" +
                                                      std::to_string(length) + ") lies beyond the " +
                                                      std::to_string(size) + " bytes of the content");
    }
    return success();
}

}  // namespace

Result<std::span<const std::byte>> MemoryContentReader::read(std::uint64_t offset, std::size_t length) {
    if (Status valid = checkRequest(offset, length, data_.size()); !valid.ok()) {
        return valid.error();
    }
    return data_.subspan(static_cast<std::size_t>(offset), length);
}

Status validate(const SourceReadOptions& options) {
    if (options.sectorRetryCount > SourceReadOptions::kMaxSectorRetries) {
        return makeError(ErrorCode::InvalidInput,
                         "sectorRetryCount exceeds " + std::to_string(SourceReadOptions::kMaxSectorRetries));
    }
    return success();
}

SourceContentReader::SourceContentReader(storage::IStorageSource& source, std::uint64_t start, std::uint64_t size,
                                         SourceReadOptions options, std::size_t cacheSize)
    : source_(&source), start_(start), size_(size), options_(std::move(options)), cacheSize_(cacheSize) {}

Result<std::unique_ptr<SourceContentReader>> SourceContentReader::open(storage::IStorageSource& source,
                                                                       std::uint64_t start, std::uint64_t size,
                                                                       SourceReadOptions options,
                                                                       std::size_t cacheSize) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "content source is not open");
    }
    if (!rangeWithin(start, size, source.size())) {
        return makeError(ErrorCode::InvalidInput, "content [" + std::to_string(start) + ", +" + std::to_string(size) +
                                                      ") lies outside the source");
    }
    if (cacheSize < kMinCacheSize || cacheSize > kMaxReadLength) {
        return makeError(ErrorCode::InvalidInput, "content cache size must lie between " +
                                                      std::to_string(kMinCacheSize) + " and " +
                                                      std::to_string(kMaxReadLength));
    }
    if (Status valid = validate(options); !valid.ok()) {
        return valid.error();
    }
    return std::unique_ptr<SourceContentReader>(
        new SourceContentReader(source, start, size, std::move(options), cacheSize));
}

void SourceContentReader::truncate(std::uint64_t size) noexcept {
    size_ = std::min(size_, size);
}

Result<std::span<const std::byte>> SourceContentReader::read(std::uint64_t offset, std::size_t length) {
    if (Status valid = checkRequest(offset, length, size_); !valid.ok()) {
        return valid.error();
    }
    if (length == 0) {
        return std::span<const std::byte>{};
    }
    if (offset >= cacheOffset_ && offset - cacheOffset_ <= cacheLength_ &&
        length <= cacheLength_ - (offset - cacheOffset_)) {
        return std::span<const std::byte>(cache_).subspan(static_cast<std::size_t>(offset - cacheOffset_), length);
    }

    // The first load is small: most carves end at the header check.
    const std::size_t want = bytesRead_ == 0 ? std::min(kMinCacheSize, cacheSize_) : cacheSize_;
    const auto loadLength =
        static_cast<std::size_t>(std::min<std::uint64_t>(size_ - offset, std::max(length, want)));
    if (cache_.size() < loadLength) {
        cache_.resize(loadLength);
    }
    cacheLength_ = 0;
    std::vector<storage::BadRegion> found;
    const std::span<std::byte> target(cache_.data(), loadLength);
    if (Status filled = detail::fillFromSource(*source_, start_ + offset, target, options_, found); !filled.ok()) {
        if (filled.error().code != ErrorCode::Cancelled) {
            sourceFailure_ = filled.error();
        }
        return filled.error();
    }
    for (const storage::BadRegion& region : found) {
        // Cannot fail: the region is non-empty and lies inside the source.
        (void)unreadable_.add(region);
    }
    bytesRead_ += loadLength;
    cacheOffset_ = offset;
    cacheLength_ = loadLength;
    return std::span<const std::byte>(cache_).subspan(0, length);
}

Result<std::optional<std::uint64_t>> findPattern(IContentReader& content, std::span<const std::byte> pattern,
                                                 std::uint64_t from, std::uint64_t to) {
    if (pattern.empty() || pattern.size() > kSearchChunk) {
        return makeError(ErrorCode::InvalidInput, "search pattern must have 1 to " + std::to_string(kSearchChunk) +
                                                      " bytes");
    }
    to = std::min(to, content.size());
    std::uint64_t position = from;
    while (position < to && to - position >= pattern.size()) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kSearchChunk, to - position));
        Result<std::span<const std::byte>> chunk = content.read(position, length);
        if (!chunk.ok()) {
            return chunk.error();
        }
        const auto found = std::search(chunk->begin(), chunk->end(), pattern.begin(), pattern.end());
        if (found != chunk->end()) {
            return std::optional<std::uint64_t>(position + static_cast<std::uint64_t>(found - chunk->begin()));
        }
        if (to - position == length) {
            break;
        }
        // The next chunk repeats the last pattern.size() - 1 bytes, so a match
        // across the boundary is found.
        position += length - (pattern.size() - 1);
    }
    return std::optional<std::uint64_t>{};
}

}  // namespace recovery::carving
