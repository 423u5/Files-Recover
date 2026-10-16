#pragma once

// The vocabulary of recovery sessions (P16): the states of a scan and of a
// recovery job, the times a session records, and the source a session
// belongs to.

#include "recovery/cancellation.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "recovery/sha256.hpp"
#include "storage/disk_list.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// the session was created, and its fingerprint. A physical disk's path,
// number and description are as the session last found it (L147: Windows
// numbers disks as they are attached).
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
    // Physical disks, from journal format 3 on: the device's serial number;
    // empty when it reports none, and in older sessions until one finds its
    // disk again.
    std::string serialNumber;
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

// What differs between the device a session records and a disk's (L147, the
// user's choice of 2026-10-16: the device must be the same, not only its
// content): when the session recorded a serial number, the disk must report
// it; when it recorded none (sessions before journal format 3, devices that
// report none), the disk's vendor and product must be the recorded ones.
// Empty when nothing does. A card reader or a USB enclosure reports its own
// serial number, so the card or the disk in another one is another device.
[[nodiscard]] std::string deviceDifference(const SessionSource& recorded, std::string_view vendor,
                                           std::string_view product, std::string_view serialNumber);

// Whether `source` is the one `recorded` describes, unchanged: the same type,
// size and sector size, and the same fingerprint; an image file under the
// same path; a physical disk the same device (deviceDifference), under any
// number. Fails with InvalidInput saying what differs, and like
// fingerprintSource().
[[nodiscard]] Status checkSameSource(const SessionSource& recorded, storage::IStorageSource& source,
                                     const CancellationToken& cancel = {});

// Opens physical disk `number` read-only: the caller's opener, whose errors
// say what to do (administrator rights).
using SessionDiskOpener = std::function<Result<std::unique_ptr<storage::IStorageSource>>(std::uint32_t number)>;

struct FoundSessionDisk {
    std::unique_ptr<storage::IStorageSource> source;
    // Its number now; whether the session recorded another.
    std::uint32_t number = 0;
    bool renumbered = false;
};

// Finds the physical disk `recorded` describes among the disks attached
// (L147): after a restart, or once unplugged and plugged in again, the
// session's disk may have another number, and its number another disk. The
// disk under the recorded number is tried first; then every disk `list`
// gives of the recorded size and sector size (or of a size it does not
// tell) that is the same device (deviceDifference), by its content
// (checkSameSource). Fails with
// InvalidInput when no disk is the session's, saying why each disk tried is
// not, or when several are; at once with the recorded disk's error when it
// cannot be opened for want of rights (no other disk could be either).
[[nodiscard]] Result<FoundSessionDisk> findSessionDisk(const SessionSource& recorded, const SessionDiskOpener& open,
                                                       const storage::DiskLister& list,
                                                       const CancellationToken& cancel = {});

// The disks attached that may be the session's physical disk (of its size,
// sector size and device) and the disk under its recorded number: what a
// file written outside an operation (a report, a log) must not be on, when
// the disk is not opened to tell which it is. Nothing is read from a disk.
// Image files: the recorded source alone.
[[nodiscard]] std::vector<storage::SourceInfo> possibleSessionSources(const SessionSource& recorded,
                                                                     const storage::DiskLister& list);

}  // namespace recovery::session
