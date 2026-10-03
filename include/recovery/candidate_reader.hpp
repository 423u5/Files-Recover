#pragma once

// Reconstruction: reads a candidate's data from the source, region by
// region, in file order.

#include "recovery/cancellation.hpp"
#include "recovery/config.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace recovery {

// Receives a candidate's data: `data` belongs at `fileOffset`. Calls come in
// increasing file order and never overlap. Bytes that are never delivered
// are zeros (known zeros, missing data inside the file, unreadable sectors).
// An error stops the reconstruction and is returned by it.
using CandidateSink = std::function<Status(std::uint64_t fileOffset, std::span<const std::byte> data)>;

struct ReconstructionOptions {
    static constexpr std::size_t kMinChunkSize = 4 * kKiB;
    static constexpr std::uint32_t kMaxSectorRetries = 16;

    // Largest single read. At least kMinChunkSize and at most storage::kMaxReadSize.
    std::size_t chunkSize = 1 * kMiB;
    // After a failed read, each sector of it is read again individually, up
    // to 1 + sectorRetryCount times, before it is counted as unreadable.
    std::uint32_t sectorRetryCount = 1;
    // Unreadable regions already known, in source offsets (for example from
    // the metadata of an image whose bad sectors were zero-filled). Stored
    // bytes inside them are not read and are counted as unreadable. Not
    // owned: it must outlive every reconstruction that uses these options.
    const storage::BadRegionMap* knownBadRegions = nullptr;
    CancellationToken cancellation;
};

struct ReconstructionReport {
    std::uint64_t expectedSize = 0;
    // Size of the reconstructed file: the file up to the end of its last
    // region that is not Missing. Missing data at the end is left out rather
    // than zero-filled; missing data inside the file is zero-filled.
    std::uint64_t outputSize = 0;
    // Read from the source and delivered.
    std::uint64_t storedBytes = 0;
    // Copied from the metadata (resident data) and delivered.
    std::uint64_t embeddedBytes = 0;
    // Known zeros (sparse holes, beyond the valid data length).
    std::uint64_t zeroBytes = 0;
    // Not located by the metadata.
    std::uint64_t missingBytes = 0;
    // Stored bytes that could not be read (zero-filled): read errors, known
    // bad regions, and bytes beyond the end of the source.
    std::uint64_t unreadableBytes = 0;
    // The part of unreadableBytes that lies beyond the end of the source (a truncated image).
    std::uint64_t outsideSourceBytes = 0;
    // Stored bytes read from reallocated clusters: they may belong to other data now.
    std::uint64_t reallocatedBytes = 0;
    // Unreadable source ranges that affected this file (read errors and known
    // bad regions), merged.
    std::vector<storage::BadRegion> unreadableRegions;

    // Every byte of the file was located and read (it may still have been
    // overwritten: see reallocatedBytes, and the candidate's layout evidence).
    [[nodiscard]] bool allBytesRead() const noexcept { return missingBytes == 0 && unreadableBytes == 0; }
};

// Reconstructs `candidate` from `source` (the whole disk or image its offsets
// refer to) and delivers its data to `sink`.
//
// The source is only read. Reads are split into chunks of at most
// options.chunkSize; a chunk that fails with an I/O error is re-read sector
// by sector, and the sectors that still fail are zero-filled and reported,
// so one bad sector costs one sector of the file, not the whole file.
//
// Fails with InvalidInput for a malformed candidate (see validateCandidate)
// or options, Cancelled when cancellation is requested, and with the error
// of a read that fails for a reason other than an I/O error (the source was
// closed, an invalid request), or the error the sink returns.
[[nodiscard]] Result<ReconstructionReport> reconstructCandidate(storage::IStorageSource& source,
                                                                const RecoveryCandidate& candidate,
                                                                const CandidateSink& sink,
                                                                const ReconstructionOptions& options = {});

}  // namespace recovery
