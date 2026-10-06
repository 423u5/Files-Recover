#pragma once

// The vocabulary of recovery sessions (P16): the states of a scan and of a
// recovery job, the times a session records, and the source a session
// belongs to.

#include "recovery/cancellation.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "recovery/sha256.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace recovery::session {

// The state of a session's scan, and of each of its recovery jobs. Every
// state but Completed can be resumed.
enum class SessionState : std::uint8_t {
    // Running; or it was when its process ended, without recording an end (a
    // crash, a kill, a power loss): the session then reports it interrupted.
    Started,
    // Paused through the session: waiting at a safe point, or stopped with
    // its process while paused.
    Paused,
    // Stopped through the session. What it did is kept.
    Cancelled,
    // Done.
    Completed,
    // Stopped by an error, recorded with the state.
    Failed,
};

[[nodiscard]] std::string_view toString(SessionState state) noexcept;

// A point in time, UTC, to the millisecond.
using SessionTime = std::chrono::sys_time<std::chrono::milliseconds>;

[[nodiscard]] SessionTime sessionNow() noexcept;

// What a source holds where its partition table and volumes begin: SHA-256 of
// the first kBlock bytes of the source and the first kBlock bytes of each
// partition its partition table lists (each preceded by its offset and
// length), unreadable sectors read as zeros. Two cards of one model differ
// there (their volume serial numbers, at least), and so do most writes to a
// card (FAT32's free count, exFAT's percentage in use), so a session refuses
// to go on with a source whose fingerprint has changed.
struct SourceFingerprint {
    static constexpr std::uint64_t kBlock = 64 * kKiB;

    Sha256Digest digest;
    // Bytes hashed, and of them the bytes that could not be read.
    std::uint64_t bytes = 0;
    std::uint64_t unreadableBytes = 0;

    friend bool operator==(const SourceFingerprint&, const SourceFingerprint&) = default;
};

// The source a session belongs to: what IStorageSource::getInfo() said when
// the session was created, and its fingerprint.
struct SessionSource {
    storage::SourceType type = storage::SourceType::Synthetic;
    // UTF-8.
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t sectorSize = 0;
    std::uint32_t physicalSectorSize = 0;
    // Physical disks.
    std::optional<std::uint32_t> diskNumber;
    std::string vendor;
    std::string product;
    std::optional<bool> removable;
    SourceFingerprint fingerprint;
};

// The fingerprint of an open source. Fails with the error of a read that
// fails for a reason other than an I/O error (unreadable sectors are hashed
// as zeros and counted), and with Cancelled.
[[nodiscard]] Result<SourceFingerprint> fingerprintSource(storage::IStorageSource& source,
                                                          const CancellationToken& cancel = {});

// What a session records of an open source: its information and its
// fingerprint. Fails like fingerprintSource().
[[nodiscard]] Result<SessionSource> describeSource(storage::IStorageSource& source,
                                                   const CancellationToken& cancel = {});

// Whether `source` is the one `recorded` describes, unchanged: the same type,
// path, size and sector size, and the same fingerprint. Fails with
// InvalidInput saying what differs, and like fingerprintSource().
[[nodiscard]] Status checkSameSource(const SessionSource& recorded, storage::IStorageSource& source,
                                     const CancellationToken& cancel = {});

}  // namespace recovery::session
