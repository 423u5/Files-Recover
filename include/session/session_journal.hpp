#pragma once

// The session journal (P16): the one file a recovery session is kept in. It
// is append-only: a header, then records, each one thing that happened (the
// session was created, a scan's update, a state change, a recovery job's
// update, a file about to be written). A session is what its records say,
// applied in order, so a session read back after a crash is the session as
// it was at its last record, and a scan or a job resumes from there.
//
// File layout (integers little-endian):
//
//   header  "RCVSESSN"  format version (u32)  CRC-32 of the 12 bytes before (u32)        16 bytes
//   record  "SREC"  type (u16)  flags (u16)  time (i64, ms since 1970, UTC)
//           payload length (u64)  CRC-32 of the payload (u32)  CRC-32 of the 28 bytes before (u32)
//           payload                                                                      32 bytes + payload
//
// Flag bit 0 marks a record an older reader may skip when it does not know
// its type ("optional"); a record of an unknown type without it, or with
// another flag bit set, belongs to a newer format: the journal is refused.
//
// Crash safety: a record is complete once its bytes are written and flushed
// (FlushFileBuffers), and only then does the scan or job that made it go on.
// A crash can leave one incomplete record at the end (a torn tail): reading
// stops before it, and the session drops it. A record that does not check
// with intact records after it means damage: reading stops before it too,
// and the session keeps a copy of the damaged journal before dropping the
// rest. Every record's checks (both CRCs, a payload that decodes, an update
// that follows the ones before) are made before anything of it is used.
//
// Payloads use a compact binary encoding of the engine's values (unsigned
// integers as LEB128, signed ones zigzag-encoded, strings and lists with
// their length first, optional values with a presence byte). Decoding treats
// the bytes as untrusted: every length is checked against the bytes left,
// every enumerator against its type's range, candidates against their
// invariants (validateCandidate, validateFileCandidate), and a payload must
// be used up exactly.

#include "evaluation/evaluated_candidate.hpp"
#include "recovery/result.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "scan/scan_state.hpp"
#include "session/session_types.hpp"
#include "storage/destination_file.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace recovery::session {

// The journal format this engine writes, and the oldest it reads.
inline constexpr std::uint32_t kJournalFormatVersion = 1;
inline constexpr std::uint32_t kOldestJournalFormatVersion = 1;

inline constexpr std::size_t kJournalHeaderSize = 16;
inline constexpr std::size_t kRecordHeaderSize = 32;
inline constexpr std::uint16_t kRecordOptional = 1;

enum class RecordType : std::uint16_t {
    SessionCreated = 1,
    ScanState = 2,
    ScanUpdate = 3,
    JobCreated = 4,
    JobState = 5,
    JobUpdate = 6,
    FileStarted = 7,
    // Optional: the session found its journal damaged and dropped a part of it.
    Damage = 8,
};

[[nodiscard]] std::string_view toString(RecordType type) noexcept;

// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------

// The first record of every session.
struct SessionCreatedRecord {
    std::string id;
    // The engine that created the session.
    std::string engineVersion;
    SessionSource source;
    // The scan's settings. The playability checker is not stored (it is
    // given to each run): `playability` says whether the scan runs that level.
    scan::ScanConfiguration configuration;
    bool playability = false;
};

// A state change of the scan.
struct ScanStateRecord {
    SessionState state = SessionState::Started;
    // The engine of the run that started, or ended.
    std::string engineVersion;
    // Failed: why.
    std::optional<Error> error;
    // Where the scan was when its state changed.
    scan::ScanStage stage = scan::ScanStage::Volumes;
    scan::ScanMetrics metrics;
};

// A recovery job: what it writes and where.
struct JobCreatedRecord {
    // 1 for a session's first job, then one more each time.
    std::uint32_t job = 0;
    // Absolute and normalised, UTF-8 (as RecoveryJob names it).
    std::string destination;
    // The candidates to write, in the order given.
    std::vector<evaluation::EvaluatedCandidateId> candidates;
};

// A state change of a recovery job.
struct JobStateRecord {
    std::uint32_t job = 0;
    SessionState state = SessionState::Started;
    std::string engineVersion;
    std::optional<Error> error;
    scan::RecoveryJobMetrics metrics;
};

// A recovery job's update (RecoveryJob's sink).
struct JobUpdateRecord {
    std::uint32_t job = 0;
    scan::RecoveryJobUpdate update;
};

// A file a recovery job has created and is about to write: if the job is
// interrupted before an update says the candidate is done, the file may be
// incomplete, and is removed before the job resumes.
struct FileStartedRecord {
    std::uint32_t job = 0;
    evaluation::EvaluatedCandidateId candidate{0};
    // UTF-8, as RecoveredFile::path.
    std::string path;
};

// Damage the session found in its journal, and what it dropped.
struct DamageRecord {
    // Where the journal stopped being readable, and the bytes from there on.
    std::uint64_t offset = 0;
    std::uint64_t bytesDropped = 0;
    // Intact records found after it, dropped with it.
    std::uint64_t recordsDropped = 0;
    std::string reason;
    // The copy of the damaged journal kept in the session's folder.
    std::string backup;
};

using RecordPayload = std::variant<SessionCreatedRecord, ScanStateRecord, scan::ScanUpdate, JobCreatedRecord,
                                   JobStateRecord, JobUpdateRecord, FileStartedRecord, DamageRecord>;

[[nodiscard]] RecordType typeOf(const RecordPayload& payload) noexcept;
// The payload's bytes. The overloads take each kind of payload as it is,
// without copying it into a RecordPayload.
[[nodiscard]] std::vector<std::byte> encodePayload(const RecordPayload& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const SessionCreatedRecord& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const ScanStateRecord& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const scan::ScanUpdate& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const JobCreatedRecord& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const JobStateRecord& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const JobUpdateRecord& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const FileStartedRecord& payload);
[[nodiscard]] std::vector<std::byte> encodePayload(const DamageRecord& payload);
// Decodes a payload of a known type. Fails with InvalidFormat for bytes that
// do not decode, and with InvalidInput for an unknown type.
[[nodiscard]] Result<RecordPayload> decodePayload(RecordType type, std::span<const std::byte> bytes);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

struct JournalRecord {
    // RecordType's value (or a type this engine does not know).
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    SessionTime time{};
    // Empty when the reader was told not to read it.
    std::vector<std::byte> payload;
    bool payloadRead = false;
    // Where the record starts in the file, and its size with its header.
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] bool optional() const noexcept { return (flags & kRecordOptional) != 0; }
};

enum class JournalEnd : std::uint8_t {
    // Every byte after the header belongs to an intact record.
    Clean,
    // The last record is incomplete or does not check, and nothing intact
    // follows: a write a crash interrupted.
    TornTail,
    // A record does not check, and intact records follow it.
    Damaged,
};

[[nodiscard]] std::string_view toString(JournalEnd end) noexcept;

// Reads a journal record by record, checking each. Reading does not lock the
// file: a journal another process is writing can be read (its last record
// may then look torn).
//
// Thread safety: none; one owner at a time.
class JournalReader {
public:
    // Opens the journal and checks its header. Fails with InvalidFormat for a
    // file that is not a journal, whose header is damaged, or whose format
    // is newer than kJournalFormatVersion (or older than
    // kOldestJournalFormatVersion); with DestinationError when it cannot be
    // opened.
    [[nodiscard]] static Result<JournalReader> open(const std::filesystem::path& path);

    JournalReader(JournalReader&&) noexcept;
    JournalReader& operator=(JournalReader&&) noexcept;
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;
    ~JournalReader();

    [[nodiscard]] std::uint32_t formatVersion() const noexcept { return formatVersion_; }
    [[nodiscard]] std::uint64_t fileSize() const noexcept { return fileSize_; }

    // The next intact record; nullopt once the intact records end (end()
    // says how). `wantPayload` (when set) chooses the types whose payload is
    // read and checked; other payloads are skipped unread. Fails with
    // InvalidFormat for a record of a newer format (unknown flag bits), and
    // with DestinationError when the file cannot be read.
    [[nodiscard]] Result<std::optional<JournalRecord>> next(
        const std::function<bool(std::uint16_t type)>& wantPayload = {});

    // After next() returned nullopt: how the journal ends, where its intact
    // records end, and why the record there does not check.
    [[nodiscard]] JournalEnd end() const noexcept { return end_; }
    [[nodiscard]] std::uint64_t validEnd() const noexcept { return position_; }
    [[nodiscard]] const std::string& endReason() const noexcept { return endReason_; }

    // Intact records that start at `from` or later (looking for them past
    // damage), for reports of what damage costs.
    [[nodiscard]] Result<std::uint64_t> countIntactRecords(std::uint64_t from);

private:
    JournalReader() = default;

    // Reads `buffer.size()` bytes at `offset`; false when the file ends first.
    [[nodiscard]] Result<bool> readAt(std::uint64_t offset, std::span<std::byte> buffer);
    // Whether an intact record starts at `offset`; its size when it does.
    [[nodiscard]] Result<std::optional<std::uint64_t>> intactAt(std::uint64_t offset);
    // The first offset at or after `from` where an intact record starts.
    [[nodiscard]] Result<std::optional<std::uint64_t>> findIntact(std::uint64_t from);
    [[nodiscard]] Status stop(std::uint64_t searchFrom, std::string reason);

    std::unique_ptr<std::ifstream> file_;
    std::filesystem::path path_;
    std::uint32_t formatVersion_ = 0;
    std::uint64_t fileSize_ = 0;
    std::uint64_t position_ = 0;
    bool ended_ = false;
    JournalEnd end_ = JournalEnd::Clean;
    std::string endReason_;
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// The journal opened for appending. It is the session's lock too: while one
// JournalFile has the journal open, no other can open it (in any process).
//
// A record is appended and flushed as a whole. If writing it fails, the file
// is cut back to where the record began; if even that fails, the journal is
// broken: every later append fails (the session must be opened again, which
// reads what reached the file).
//
// Thread safety: append() may be called from several threads at once; the
// records are written one after the other, and threads waiting for a flush
// share one (group commit). The other members need one owner.
class JournalFile {
public:
    // Creates a journal (the file must not exist) and writes its header.
    // Fails with DestinationError.
    [[nodiscard]] static Result<std::unique_ptr<JournalFile>> create(const std::filesystem::path& path);
    // Opens an existing journal to append at its end. Fails with
    // DestinationError, saying when another JournalFile holds it.
    [[nodiscard]] static Result<std::unique_ptr<JournalFile>> open(const std::filesystem::path& path);

    ~JournalFile();
    JournalFile(const JournalFile&) = delete;
    JournalFile& operator=(const JournalFile&) = delete;
    JournalFile(JournalFile&&) = delete;
    JournalFile& operator=(JournalFile&&) = delete;

    // Appends a record; once it returns, the record is on the device
    // (flushed). Fails with DestinationError, or with the error that broke
    // the journal.
    [[nodiscard]] Status append(RecordType type, std::uint16_t flags, SessionTime time,
                                std::span<const std::byte> payload);
    // The same, for a payload to encode.
    [[nodiscard]] Status append(const RecordPayload& payload, SessionTime time);
    // Drops everything from `end` on (a torn tail, damage), flushed.
    [[nodiscard]] Status truncate(std::uint64_t end);
    // Refuses every later append with `error`: the journal holds something
    // its owner could not take in, so the two no longer agree.
    void markBroken(Error error);

    [[nodiscard]] std::uint64_t end() const;
    [[nodiscard]] bool broken() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    JournalFile(storage::DestinationFile file, std::filesystem::path path, std::uint64_t end);

    storage::DestinationFile file_;
    std::filesystem::path path_;
    mutable std::mutex writeMutex_;
    std::uint64_t end_ = 0;
    std::uint64_t written_ = 0;
    std::optional<Error> broken_;
    std::mutex flushMutex_;
    std::uint64_t flushed_ = 0;
};

// The bytes of a record as the journal stores it (header and payload).
[[nodiscard]] std::vector<std::byte> encodeRecord(std::uint16_t type, std::uint16_t flags, SessionTime time,
                                                  std::span<const std::byte> payload);
// The bytes of a journal's header for `version`.
[[nodiscard]] std::vector<std::byte> encodeJournalHeader(std::uint32_t version = kJournalFormatVersion);

}  // namespace recovery::session
