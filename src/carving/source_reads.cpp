#include "source_reads.hpp"

#include <algorithm>
#include <cstring>

namespace recovery::carving::detail {

namespace {

bool isRetryable(storage::ReadStatus status) noexcept {
    return status == storage::ReadStatus::IoError || status == storage::ReadStatus::PartialRead;
}

void zeroFill(std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        std::memset(bytes.data(), 0, bytes.size());
    }
}

// Appends [offset, offset + length), merging it with the last range when
// they touch and have the same error code.
void appendUnreadable(std::vector<storage::BadRegion>& unreadable, std::uint64_t offset, std::uint64_t length,
                      std::uint32_t errorCode) {
    if (!unreadable.empty()) {
        storage::BadRegion& last = unreadable.back();
        if (last.end() == offset && last.errorCode == errorCode) {
            last.length += length;
            return;
        }
    }
    unreadable.push_back(storage::BadRegion{offset, length, errorCode});
}

class Filler {
public:
    Filler(storage::IStorageSource& source, const SourceReadOptions& options,
           std::vector<storage::BadRegion>& unreadable)
        : source_(source), options_(options), unreadable_(unreadable) {}

    // `buffer` holds source bytes [offset, offset + buffer.size()).
    Status fill(std::uint64_t offset, std::span<std::byte> buffer) {
        const std::uint64_t end = offset + buffer.size();
        std::uint64_t position = offset;
        if (options_.knownBadRegions != nullptr) {
            for (const storage::BadRegion& bad : options_.knownBadRegions->overlapping(offset, buffer.size())) {
                const std::uint64_t badStart = std::max(bad.offset, position);
                const std::uint64_t badEnd = std::min(bad.end(), end);
                if (badStart >= badEnd) {
                    continue;
                }
                if (badStart > position) {
                    if (Status read = readRange(position, piece(buffer, offset, position, badStart)); !read.ok()) {
                        return read;
                    }
                }
                zeroFill(piece(buffer, offset, badStart, badEnd));
                appendUnreadable(unreadable_, badStart, badEnd - badStart, bad.errorCode);
                position = badEnd;
            }
        }
        if (position < end) {
            return readRange(position, piece(buffer, offset, position, end));
        }
        return success();
    }

private:
    static std::span<std::byte> piece(std::span<std::byte> buffer, std::uint64_t bufferOffset, std::uint64_t from,
                                      std::uint64_t to) {
        return buffer.subspan(static_cast<std::size_t>(from - bufferOffset), static_cast<std::size_t>(to - from));
    }

    Status readRange(std::uint64_t offset, std::span<std::byte> bytes) {
        if (options_.cancellation.isCancellationRequested()) {
            return cancelledError();
        }
        const storage::ReadResult read = source_.read(ByteOffset{offset}, bytes);
        if (read.ok()) {
            return success();
        }
        if (!isRetryable(read.status)) {
            return storage::toError(read);
        }
        return readSectors(offset, bytes);
    }

    // Re-reads a failed range one sector at a time; sectors that keep failing
    // are zero-filled and recorded.
    Status readSectors(std::uint64_t offset, std::span<std::byte> bytes) {
        const std::uint64_t sectorSize = storage::isValidSectorSize(source_.sectorSize()) ? source_.sectorSize() : 512;
        const std::uint64_t end = offset + bytes.size();
        std::uint64_t position = offset;
        while (position < end) {
            if (options_.cancellation.isCancellationRequested()) {
                return cancelledError();
            }
            const std::uint64_t next = std::min(end, (position / sectorSize + 1) * sectorSize);
            const std::span<std::byte> sector = piece(bytes, offset, position, next);
            storage::ReadResult read;
            for (std::uint32_t attempt = 0; attempt <= options_.sectorRetryCount; ++attempt) {
                read = source_.read(ByteOffset{position}, sector);
                if (read.ok() || !isRetryable(read.status)) {
                    break;
                }
            }
            if (!read.ok()) {
                if (!isRetryable(read.status)) {
                    return storage::toError(read);
                }
                zeroFill(sector);
                appendUnreadable(unreadable_, position, next - position, read.systemErrorCode);
            }
            position = next;
        }
        return success();
    }

    storage::IStorageSource& source_;
    const SourceReadOptions& options_;
    std::vector<storage::BadRegion>& unreadable_;
};

}  // namespace

Error cancelledError() {
    return makeError(ErrorCode::Cancelled, "carving was cancelled");
}

Status fillFromSource(storage::IStorageSource& source, std::uint64_t offset, std::span<std::byte> buffer,
                      const SourceReadOptions& options, std::vector<storage::BadRegion>& unreadable) {
    if (buffer.empty()) {
        return success();
    }
    return Filler(source, options, unreadable).fill(offset, buffer);
}

}  // namespace recovery::carving::detail
