#pragma once

// Content identity (P14): what a recovered file's bytes are, whatever its
// name, so that the same content found twice (a copy, a deleted copy next to
// the active file, a carve of a file the metadata also knows) is recognised.
//
//  * SHA-256 of exactly the bytes recovery writes: the final identity. Two
//    candidates are duplicates when their digests are equal.
//  * A preliminary hash, quick to compute and compare: the size and the
//    CRC-32 of the first and of the last 64 KiB. Different preliminary
//    hashes mean different content; equal ones only that it may be the same.
//
// The content is the candidate's data as CandidateContentReader (and so
// reconstructCandidate and RecoveryWriter) delivers it: bytes that read as
// zeros (known zeros, missing data inside the file, unreadable sectors) are
// hashed as zeros, and missing data at the end is not part of it.

#include "carving/content_reader.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "recovery/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace recovery::evaluation {

struct PreliminaryHash {
    static constexpr std::uint64_t kWindow = 64 * kKiB;

    std::uint64_t size = 0;
    // CRC-32 of the first and of the last min(size, kWindow) bytes.
    std::uint32_t head = 0;
    std::uint32_t tail = 0;

    friend bool operator==(const PreliminaryHash&, const PreliminaryHash&) = default;
};

struct ContentIdentity {
    // The bytes recovery delivers (the plan's recoveredSize).
    std::uint64_t size = 0;
    std::optional<Sha256Digest> sha256;
    std::optional<PreliminaryHash> preliminary;
};

struct IdentityOptions {
    // SHA-256 reads every byte of the content; duplicates need it.
    bool sha256 = true;
    // The preliminary hash reads the first and the last 64 KiB.
    bool preliminary = true;
};

// The identity of the content whose bytes are all of `content`. Fails only
// with the reader's errors (Cancelled, source failures).
[[nodiscard]] Result<ContentIdentity> computeIdentity(carving::IContentReader& content,
                                                      const IdentityOptions& options = {});

// Duplicate detection: the first candidate recorded with a SHA-256 is that
// content's original; later candidates with the same digest are duplicates
// of it. Duplicates are identified, not removed: every candidate is still
// delivered and can be written. Empty content is never a duplicate (an empty
// file says nothing about another), and neither is content without a
// SHA-256 (IdentityOptions::sha256 off).
//
// Thread safety: none; one owner at a time.
class DuplicateIndex {
public:
    // Records candidate `id`'s content: the original's id when it is a
    // duplicate, nothing otherwise (it becomes the original).
    [[nodiscard]] std::optional<std::uint64_t> add(const ContentIdentity& identity, std::uint64_t id);
    // The original of this content, if any, without recording anything.
    [[nodiscard]] std::optional<std::uint64_t> originalOf(const ContentIdentity& identity) const;
    // Distinct contents recorded.
    [[nodiscard]] std::size_t size() const noexcept { return originals_.size(); }

private:
    std::map<Sha256Digest, std::uint64_t> originals_;
};

}  // namespace recovery::evaluation
