#pragma once

// Read access to the bytes of one file (or would-be file), by offset within
// the file. Formats parse and validate files only through this interface, so
// they never see the source itself and can never read beyond the bytes they
// were given: the data window of a carve, or a file held in memory.

#include "recovery/cancellation.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace recovery::carving {

class IContentReader {
public:
    // Largest single read.
    static constexpr std::size_t kMaxReadLength = 1 * kMiB;

    virtual ~IContentReader() = default;
    IContentReader(const IContentReader&) = delete;
    IContentReader& operator=(const IContentReader&) = delete;
    IContentReader(IContentReader&&) = delete;
    IContentReader& operator=(IContentReader&&) = delete;

    // Bytes that may be read: offsets [0, size()).
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    // Returns exactly `length` bytes at `offset`. The span stays valid until
    // the next call. Fails with InvalidInput when the range is not inside
    // [0, size()) or longer than kMaxReadLength, with Cancelled, and with the
    // source's error when a read fails for a reason other than an I/O error.
    // Unreadable source sectors are delivered as zeros (see SourceContentReader).
    [[nodiscard]] virtual Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) = 0;

protected:
    IContentReader() = default;
};

// Content held in memory (data already read, and tests). Does not own the data.
class MemoryContentReader final : public IContentReader {
public:
    explicit MemoryContentReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return data_.size(); }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override;

private:
    std::span<const std::byte> data_;
};

// How the scanner and the carver read a source.
struct SourceReadOptions {
    static constexpr std::uint32_t kMaxSectorRetries = 16;

    // After a failed read, each of its sectors is read again individually, up
    // to 1 + sectorRetryCount times, before it is counted as unreadable.
    std::uint32_t sectorRetryCount = 1;
    // Unreadable regions already known, in source offsets (for example from
    // the metadata of an image whose bad sectors were zero-filled). They are
    // not read and count as unreadable. Not owned: it must outlive every scan
    // and carve that uses these options.
    const storage::BadRegionMap* knownBadRegions = nullptr;
    CancellationToken cancellation;
};

[[nodiscard]] Status validate(const SourceReadOptions& options);

// The bytes [start, start + size) of a source, as a file's content.
//
// Reads are served from a cache of `cacheSize` bytes (larger reads load as
// much as they need, up to kMaxReadLength), so formats can parse small
// structures without a source read each. The first load is only
// kMinCacheSize bytes, since most carves end at the header check. A read
// that fails with an I/O error
// is repeated sector by sector; sectors that still fail, and bytes inside
// known bad regions, are delivered as zeros and recorded in unreadable().
//
// Thread safety: none; one owner at a time. The source must stay open and
// outlive the reader.
class SourceContentReader final : public IContentReader {
public:
    static constexpr std::size_t kMinCacheSize = 4 * kKiB;
    static constexpr std::size_t kDefaultCacheSize = 256 * kKiB;

    // Fails with InvalidInput when the source is not open, the range is not
    // inside the source, cacheSize is outside [kMinCacheSize, kMaxReadLength]
    // or the options are invalid.
    [[nodiscard]] static Result<std::unique_ptr<SourceContentReader>> open(storage::IStorageSource& source,
                                                                           std::uint64_t start, std::uint64_t size,
                                                                           SourceReadOptions options = {},
                                                                           std::size_t cacheSize = kDefaultCacheSize);

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override;

    // Source offset of content offset 0.
    [[nodiscard]] std::uint64_t start() const noexcept { return start_; }
    // Shrinks the content to its first `size` bytes (never grows it), e.g. to
    // the length end detection found. Cached data is kept.
    void truncate(std::uint64_t size) noexcept;

    // Source ranges delivered as zeros so far.
    [[nodiscard]] const storage::BadRegionMap& unreadable() const noexcept { return unreadable_; }
    // Source bytes read so far (including re-reads after a cache miss).
    [[nodiscard]] std::uint64_t bytesRead() const noexcept { return bytesRead_; }
    // The error of the last read that failed because of the source (not
    // because of the request or cancellation), if any. A format that passes
    // such an error on lets the caller tell it from the format's own errors.
    [[nodiscard]] const std::optional<Error>& sourceFailure() const noexcept { return sourceFailure_; }

private:
    SourceContentReader(storage::IStorageSource& source, std::uint64_t start, std::uint64_t size,
                        SourceReadOptions options, std::size_t cacheSize);

    storage::IStorageSource* source_;
    std::uint64_t start_;
    std::uint64_t size_;
    SourceReadOptions options_;
    std::size_t cacheSize_;
    std::vector<std::byte> cache_;
    std::uint64_t cacheOffset_ = 0;  // content offset of cache_[0]
    std::size_t cacheLength_ = 0;    // valid bytes in cache_
    storage::BadRegionMap unreadable_;
    std::uint64_t bytesRead_ = 0;
    std::optional<Error> sourceFailure_;
};

// Offset of the first occurrence of `pattern` that lies entirely inside
// [from, to) (to is clipped to content.size()), or nullopt. Reads in chunks,
// so memory stays bounded whatever the range. Fails with InvalidInput for an
// empty pattern or one longer than a chunk, and with the reader's errors.
[[nodiscard]] Result<std::optional<std::uint64_t>> findPattern(IContentReader& content,
                                                               std::span<const std::byte> pattern, std::uint64_t from,
                                                               std::uint64_t to);

}  // namespace recovery::carving
