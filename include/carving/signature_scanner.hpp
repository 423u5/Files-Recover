#pragma once

// Signature scanning: one sequential pass over a source range that reports
// every position where a registered format's signature matches.

#include "carving/content_reader.hpp"
#include "carving/file_format.hpp"
#include "carving/format_registry.hpp"
#include "diagnostics/logger.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace recovery::carving {

struct ScanProgress {
    // Every file start before this offset has been examined.
    std::uint64_t position = 0;
    std::uint64_t startOffset = 0;
    std::uint64_t endOffset = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t hits = 0;
    std::uint64_t unreadableBytes = 0;
    std::chrono::milliseconds elapsed{0};
};

// Invoked on the scanning thread. Must not throw (exceptions are swallowed).
using ScanProgressCallback = std::function<void(const ScanProgress&)>;

struct ScanOptions {
    static constexpr std::uint32_t kMaxAlignment = 1 * static_cast<std::uint32_t>(kMiB);

    // File starts examined: [startOffset, endOffset). No endOffset means the
    // end of the source. A signature may extend beyond endOffset (up to the
    // end of the source), so scans of adjacent ranges together report every
    // hit exactly once.
    std::uint64_t startOffset = 0;
    std::optional<std::uint64_t> endOffset;
    // Size of the sequential reads: a multiple of the source's sector size,
    // at most storage::kMaxReadSize. Memory use is about one block.
    std::size_t blockSize = 1 * kMiB;
    // Only file starts at multiples of this (in source offsets) are examined:
    // 1 examines every byte, 512 only sector starts (files on FAT, exFAT and
    // NTFS start at cluster boundaries). A power of two up to kMaxAlignment.
    // Data a larger alignment makes irrelevant is not read.
    std::uint32_t alignment = 1;
    // The scan stops (HitLimitReached) after the position at which the
    // number of hits reaches this. At least 1.
    std::uint64_t maxHits = 10'000'000;
    SourceReadOptions reads;
    std::chrono::milliseconds progressInterval{250};
    ScanProgressCallback onProgress;
    diagnostics::Logger* logger = nullptr;
};

struct SignatureHit {
    // The format whose signature matched. Owned by the scanner (and the
    // registry it was created from): valid as long as either exists.
    const IFileFormat* format = nullptr;
    // Position of the format in the registry.
    std::size_t formatIndex = 0;
    // Index into format->descriptor().signatures.
    std::size_t signatureIndex = 0;
    // Source offset where the file would start. The pattern lies at
    // fileOffset + signature.offset.
    std::uint64_t fileOffset = 0;
};

// Receives hits in increasing fileOffset order; hits at the same offset come
// in registration order, one per format (its first matching signature). An
// error stops the scan: Cancelled ends it as cancelled, any other error is
// returned by scan().
using HitSink = std::function<Status(const SignatureHit&)>;

enum class ScanOutcome : std::uint8_t {
    Completed,
    Cancelled,
    HitLimitReached,
};

[[nodiscard]] std::string_view toString(ScanOutcome outcome) noexcept;

struct ScanReport {
    ScanOutcome outcome = ScanOutcome::Completed;
    std::uint64_t startOffset = 0;
    std::uint64_t endOffset = 0;
    // Every file start before this offset has been examined and its hits
    // delivered: endOffset when completed, and where a later scan of the
    // rest continues otherwise. When the sink cancelled, hits at nextOffset
    // may have been delivered already.
    std::uint64_t nextOffset = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t hits = 0;
    // Hits per format, by registry position.
    std::vector<std::uint64_t> hitsPerFormat;
    // Bytes that could not be read (read errors and known bad regions); no
    // signature is matched across them.
    std::uint64_t unreadableBytes = 0;
    std::vector<storage::BadRegion> unreadableRegions;
    std::chrono::milliseconds elapsed{0};
};

// Finds signature matches in a source.
//
// The source is read strictly sequentially, in blocks of options.blockSize,
// and never written. Memory use is one block plus the longest signature,
// whatever the size of the source; hits are streamed to the sink as they are
// found. A read that fails with an I/O error is repeated sector by sector;
// sectors that still fail are recorded and skipped, so one bad sector costs
// only the matches that would touch it.
//
// Signatures are indexed by their offset and first byte, so the cost per
// examined position hardly depends on the number of formats.
//
// Thread safety: scan() is const; concurrent scans (of different ranges,
// with different sinks) are safe on sources that allow concurrent reads.
class SignatureScanner {
public:
    // Snapshot of the signatures of every format in `registry`. Fails with
    // InvalidInput when the registry is empty.
    [[nodiscard]] static Result<SignatureScanner> create(const FormatRegistry& registry);

    // Scans `source`, which must be open. Fails with InvalidInput for invalid
    // options (range outside the source, block size, alignment, ...), and
    // with the error of a read that fails for a reason other than an I/O
    // error, or the sink's error (other than Cancelled).
    [[nodiscard]] Result<ScanReport> scan(storage::IStorageSource& source, const HitSink& sink,
                                          const ScanOptions& options = {}) const;

    [[nodiscard]] const std::vector<std::shared_ptr<const IFileFormat>>& formats() const noexcept {
        return formats_;
    }
    // Longest distance from a file start to the end of one of its patterns:
    // how far a scan reads beyond its range.
    [[nodiscard]] std::size_t maxReach() const noexcept { return maxReach_; }

private:
    class Run;

    struct Entry {
        std::size_t formatIndex = 0;
        std::size_t signatureIndex = 0;
        FileSignature signature;
    };
    // Signatures with the same offset, indexed by their first byte.
    struct Group {
        std::uint32_t offset = 0;
        std::array<bool, 256> firstByte{};
        std::array<std::vector<Entry>, 256> entries;
    };

    SignatureScanner() = default;

    std::vector<std::shared_ptr<const IFileFormat>> formats_;
    std::vector<Group> groups_;
    std::size_t maxReach_ = 0;
};

}  // namespace recovery::carving
