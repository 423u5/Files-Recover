#pragma once

// A recovery candidate's data as a file's content, for format parsers and
// validators (carving::IContentReader): the bytes reconstructCandidate()
// would deliver, read from the source region by region, without being
// written anywhere. MP4 recovery (P12) validates filesystem candidates
// through it, so a file of several gigabytes needs no copy.

#include "carving/content_reader.hpp"
#include "recovery/config.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace recovery {

// Offsets [0, size()) of the file: size() is the end of the candidate's last
// region that is not Missing, as for reconstructCandidate(). Stored regions
// are read from the source through a carving::SourceContentReader over the
// whole source (a cache, sector-by-sector retries after an I/O error, known
// bad regions); Zeros and Missing regions inside the file read as zeros, and
// Embedded ones come from the candidate. Stored bytes that cannot be read, or
// that lie beyond the end of the source (a truncated image), read as zeros
// and are recorded.
//
// Thread safety: none; one owner at a time. The source must stay open and
// outlive the reader. The reader keeps its own copy of the candidate's
// regions and embedded data.
class CandidateContentReader final : public carving::IContentReader {
public:
    // Fails with InvalidInput for a malformed candidate (validateCandidate),
    // a source that is not open, or invalid options.
    [[nodiscard]] static Result<std::unique_ptr<CandidateContentReader>> open(
        storage::IStorageSource& source, const RecoveryCandidate& candidate, carving::SourceReadOptions options = {},
        std::size_t cacheSize = carving::SourceContentReader::kDefaultCacheSize);

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override;

    // Source ranges delivered as zeros so far (read errors and known bad regions).
    [[nodiscard]] const storage::BadRegionMap& unreadable() const noexcept { return source_->unreadable(); }
    // File offsets of stored bytes read so far that lie beyond the end of the source.
    [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::uint64_t>>& outsideSource() const noexcept {
        return outside_;
    }
    [[nodiscard]] std::uint64_t bytesRead() const noexcept { return source_->bytesRead(); }
    [[nodiscard]] const std::optional<Error>& sourceFailure() const noexcept { return source_->sourceFailure(); }

private:
    CandidateContentReader(std::unique_ptr<carving::SourceContentReader> source, std::uint64_t sourceSize,
                           std::vector<SourceRegion> regions, std::vector<std::byte> embedded, std::uint64_t size);

    // The index of the region holding file offset `offset` (< size_).
    [[nodiscard]] std::size_t regionAt(std::uint64_t offset) const noexcept;
    void noteOutside(std::uint64_t fileOffset, std::uint64_t length);

    std::unique_ptr<carving::SourceContentReader> source_;
    std::uint64_t sourceSize_;
    std::vector<SourceRegion> regions_;
    std::vector<std::byte> embedded_;
    std::uint64_t size_;
    std::vector<std::byte> buffer_;
    // Merged [begin, end) file ranges.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> outside_;
};

}  // namespace recovery
