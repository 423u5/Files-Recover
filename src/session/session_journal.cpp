#include "session/session_journal.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "recovery/text.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace recovery::session {

namespace {

constexpr std::array<std::byte, 8> kJournalMagic = {std::byte{'R'}, std::byte{'C'}, std::byte{'V'}, std::byte{'S'},
                                                    std::byte{'E'}, std::byte{'S'}, std::byte{'S'}, std::byte{'N'}};
constexpr std::array<std::byte, 4> kRecordMagic = {std::byte{'S'}, std::byte{'R'}, std::byte{'E'}, std::byte{'C'}};

// The part of the file read at a time when looking for intact records.
constexpr std::uint64_t kSearchChunk = 1 * kMiB;

// ERROR_SHARING_VIOLATION: another handle has the file open for writing.
constexpr std::uint32_t kSharingViolation = 32;

bool startsWithMagic(std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= kRecordMagic.size() &&
           std::equal(kRecordMagic.begin(), kRecordMagic.end(), bytes.begin());
}

}  // namespace

std::string_view toString(JournalEnd end) noexcept {
    switch (end) {
    case JournalEnd::Clean:
        return "clean";
    case JournalEnd::TornTail:
        return "torn-tail";
    case JournalEnd::Damaged:
        return "damaged";
    }
    return "unknown";
}

std::vector<std::byte> encodeJournalHeader(std::uint32_t version) {
    std::vector<std::byte> header(kJournalHeaderSize);
    std::copy(kJournalMagic.begin(), kJournalMagic.end(), header.begin());
    storeLe32(header, 8, version);
    storeLe32(header, 12, crc32(std::span(header).first(12)));
    return header;
}

std::vector<std::byte> encodeRecord(std::uint16_t type, std::uint16_t flags, SessionTime time,
                                    std::span<const std::byte> payload) {
    std::vector<std::byte> bytes(kRecordHeaderSize + payload.size());
    const std::span<std::byte> header(bytes.data(), kRecordHeaderSize);
    std::copy(kRecordMagic.begin(), kRecordMagic.end(), header.begin());
    storeLe16(header, 4, type);
    storeLe16(header, 6, flags);
    storeLe64(header, 8, static_cast<std::uint64_t>(time.time_since_epoch().count()));
    storeLe64(header, 16, payload.size());
    storeLe32(header, 24, crc32(payload));
    storeLe32(header, 28, crc32(header.first(28)));
    std::copy(payload.begin(), payload.end(), bytes.begin() + kRecordHeaderSize);
    return bytes;
}

// ---------------------------------------------------------------------------
// JournalReader
// ---------------------------------------------------------------------------

JournalReader::JournalReader(JournalReader&&) noexcept = default;
JournalReader& JournalReader::operator=(JournalReader&&) noexcept = default;
JournalReader::~JournalReader() = default;

Result<JournalReader> JournalReader::open(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return makeError(ErrorCode::InvalidInput, "session: there is no journal at '" + toUtf8(path) + "'");
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError, "session: cannot read the size of '" + toUtf8(path) + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    JournalReader reader;
    reader.path_ = path;
    reader.fileSize_ = size;
    reader.file_ = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!reader.file_->is_open()) {
        return makeError(ErrorCode::DestinationError, "session: cannot open the journal '" + toUtf8(path) + "'");
    }
    std::array<std::byte, kJournalHeaderSize> header{};
    Result<bool> read = reader.readAt(0, header);
    if (!read.ok()) {
        return read.error();
    }
    if (!*read || !std::equal(kJournalMagic.begin(), kJournalMagic.end(), header.begin())) {
        return makeError(ErrorCode::InvalidFormat, "session: '" + toUtf8(path) + "' is not a session journal");
    }
    if (crc32(std::span(header).first(12)) != loadLe32(header, 12)) {
        return makeError(ErrorCode::InvalidFormat, "session: the header of the journal '" + toUtf8(path) +
                                                       "' is damaged (its format cannot be told)");
    }
    const std::uint32_t version = loadLe32(header, 8);
    if (version > kJournalFormatVersion) {
        return makeError(ErrorCode::InvalidFormat,
                         "session: the journal '" + toUtf8(path) + "' is in session format " +
                             std::to_string(version) + ", newer than this engine reads (format " +
                             std::to_string(kJournalFormatVersion) + "): open it with a newer engine");
    }
    if (version < kOldestJournalFormatVersion) {
        return makeError(ErrorCode::InvalidFormat, "session: the journal '" + toUtf8(path) +
                                                       "' is in session format " + std::to_string(version) +
                                                       ", which this engine does not read");
    }
    reader.formatVersion_ = version;
    reader.position_ = kJournalHeaderSize;
    return reader;
}

Result<bool> JournalReader::readAt(std::uint64_t offset, std::span<std::byte> buffer) {
    if (offset > fileSize_ || buffer.size() > fileSize_ - offset) {
        return false;
    }
    file_->clear();
    file_->seekg(static_cast<std::streamoff>(offset));
    file_->read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (file_->gcount() != static_cast<std::streamsize>(buffer.size())) {
        return makeError(ErrorCode::DestinationError, "session: cannot read the journal '" + toUtf8(path_) +
                                                          "' at offset " + std::to_string(offset));
    }
    return true;
}

Result<std::optional<std::uint64_t>> JournalReader::intactAt(std::uint64_t offset) {
    if (offset > fileSize_ || fileSize_ - offset < kRecordHeaderSize) {
        return std::optional<std::uint64_t>{};
    }
    std::array<std::byte, kRecordHeaderSize> header{};
    Result<bool> read = readAt(offset, header);
    if (!read.ok()) {
        return read.error();
    }
    if (!startsWithMagic(header) || crc32(std::span(header).first(28)) != loadLe32(header, 28)) {
        return std::optional<std::uint64_t>{};
    }
    const std::uint64_t length = loadLe64(header, 16);
    if (length > fileSize_ - offset - kRecordHeaderSize) {
        return std::optional<std::uint64_t>{};
    }
    std::vector<std::byte> payload(static_cast<std::size_t>(length));
    read = readAt(offset + kRecordHeaderSize, payload);
    if (!read.ok()) {
        return read.error();
    }
    if (crc32(payload) != loadLe32(header, 24)) {
        return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>{kRecordHeaderSize + length};
}

Result<std::optional<std::uint64_t>> JournalReader::findIntact(std::uint64_t from) {
    std::vector<std::byte> chunk;
    std::uint64_t at = from;
    while (at < fileSize_ && fileSize_ - at >= kRecordHeaderSize) {
        const std::uint64_t length = std::min(kSearchChunk, fileSize_ - at);
        chunk.resize(static_cast<std::size_t>(length));
        Result<bool> read = readAt(at, chunk);
        if (!read.ok()) {
            return read.error();
        }
        for (std::size_t i = 0; i + kRecordMagic.size() <= chunk.size(); ++i) {
            if (!startsWithMagic(std::span(chunk).subspan(i))) {
                continue;
            }
            Result<std::optional<std::uint64_t>> intact = intactAt(at + i);
            if (!intact.ok()) {
                return intact.error();
            }
            if (intact->has_value()) {
                return std::optional<std::uint64_t>{at + i};
            }
        }
        // The next chunk starts where a magic cut by this one's end begins.
        at += length - (kRecordMagic.size() - 1);
    }
    return std::optional<std::uint64_t>{};
}

Status JournalReader::stop(std::uint64_t searchFrom, std::string reason) {
    ended_ = true;
    endReason_ = std::move(reason);
    Result<std::optional<std::uint64_t>> found = findIntact(searchFrom);
    if (!found.ok()) {
        return found.error();
    }
    end_ = found->has_value() ? JournalEnd::Damaged : JournalEnd::TornTail;
    return success();
}

Result<std::optional<JournalRecord>> JournalReader::next(const std::function<bool(std::uint16_t type)>& wantPayload) {
    using Next = std::optional<JournalRecord>;
    if (ended_) {
        return Next{};
    }
    const std::uint64_t at = position_;
    const auto stopHere = [&](std::string reason) -> Result<Next> {
        if (Status stopped = stop(at + 1, std::move(reason) + " at offset " + std::to_string(at)); !stopped.ok()) {
            return stopped.error();
        }
        return Next{};
    };
    if (at == fileSize_) {
        ended_ = true;
        end_ = JournalEnd::Clean;
        return Next{};
    }
    if (fileSize_ - at < kRecordHeaderSize) {
        return stopHere("the journal ends inside a record's header");
    }
    std::array<std::byte, kRecordHeaderSize> header{};
    Result<bool> read = readAt(at, header);
    if (!read.ok()) {
        return read.error();
    }
    if (!startsWithMagic(header) || crc32(std::span(header).first(28)) != loadLe32(header, 28)) {
        return stopHere("a record header that does not check");
    }
    JournalRecord record;
    record.type = loadLe16(header, 4);
    record.flags = loadLe16(header, 6);
    record.time = SessionTime{std::chrono::milliseconds{static_cast<std::int64_t>(loadLe64(header, 8))}};
    const std::uint64_t length = loadLe64(header, 16);
    if ((record.flags & ~kRecordOptional) != 0) {
        return makeError(ErrorCode::InvalidFormat,
                         "session: the journal '" + toUtf8(path_) + "' has a record with flags " +
                             std::to_string(record.flags) + " at offset " + std::to_string(at) +
                             ": it is of a newer session format than this engine reads");
    }
    if (length > fileSize_ - at - kRecordHeaderSize) {
        return stopHere("a record that runs past the end of the journal");
    }
    record.offset = at;
    record.size = kRecordHeaderSize + length;
    if (!wantPayload || wantPayload(record.type)) {
        record.payload.resize(static_cast<std::size_t>(length));
        read = readAt(at + kRecordHeaderSize, record.payload);
        if (!read.ok()) {
            return read.error();
        }
        if (crc32(record.payload) != loadLe32(header, 24)) {
            record.payload.clear();
            return stopHere("a record whose payload does not check");
        }
        record.payloadRead = true;
    }
    position_ = at + record.size;
    return Next{std::move(record)};
}

Result<std::uint64_t> JournalReader::countIntactRecords(std::uint64_t from) {
    std::uint64_t count = 0;
    std::uint64_t at = from;
    for (;;) {
        // Records follow each other; look for one only where the chain breaks.
        Result<std::optional<std::uint64_t>> size = intactAt(at);
        if (!size.ok()) {
            return size.error();
        }
        if (!size->has_value()) {
            Result<std::optional<std::uint64_t>> found = findIntact(at);
            if (!found.ok()) {
                return found.error();
            }
            if (!found->has_value()) {
                return count;
            }
            at = **found;
            size = intactAt(at);
            if (!size.ok()) {
                return size.error();
            }
            if (!size->has_value()) {
                return count;
            }
        }
        ++count;
        at += **size;
    }
}

// ---------------------------------------------------------------------------
// JournalFile
// ---------------------------------------------------------------------------

JournalFile::JournalFile(storage::DestinationFile file, std::filesystem::path path, std::uint64_t end)
    : file_(std::move(file)), path_(std::move(path)), end_(end) {}

JournalFile::~JournalFile() = default;

Result<std::unique_ptr<JournalFile>> JournalFile::create(const std::filesystem::path& path) {
    Result<storage::DestinationFile> opened =
        storage::DestinationFile::open(path, storage::DestinationFile::OpenMode::CreateNew);
    if (!opened.ok()) {
        return opened.error();
    }
    const std::vector<std::byte> header = encodeJournalHeader();
    if (Status written = opened->writeAt(0, header); !written.ok()) {
        return written.error();
    }
    if (Status flushed = opened->flush(); !flushed.ok()) {
        return flushed.error();
    }
    return std::unique_ptr<JournalFile>(new JournalFile(std::move(opened).value(), path, header.size()));
}

Result<std::unique_ptr<JournalFile>> JournalFile::open(const std::filesystem::path& path) {
    Result<storage::DestinationFile> opened =
        storage::DestinationFile::open(path, storage::DestinationFile::OpenMode::OpenExisting);
    if (!opened.ok()) {
        if (opened.error().systemErrorCode == kSharingViolation) {
            return makeError(ErrorCode::DestinationError,
                             "session: the session is in use: its journal '" + toUtf8(path) +
                                 "' is open in another process or session",
                             kSharingViolation);
        }
        return opened.error();
    }
    Result<std::uint64_t> size = opened->size();
    if (!size.ok()) {
        return size.error();
    }
    return std::unique_ptr<JournalFile>(new JournalFile(std::move(opened).value(), path, *size));
}

Status JournalFile::append(RecordType type, std::uint16_t flags, SessionTime time,
                           std::span<const std::byte> payload) {
    const std::vector<std::byte> bytes = encodeRecord(static_cast<std::uint16_t>(type), flags, time, payload);
    std::uint64_t mine = 0;
    {
        const std::lock_guard lock(writeMutex_);
        if (broken_.has_value()) {
            return *broken_;
        }
        const std::uint64_t start = end_;
        if (Status written = file_.writeAt(start, bytes); !written.ok()) {
            // Whatever part of the record reached the file goes again, so the
            // journal ends with the records written before.
            if (Status cut = file_.truncate(start); !cut.ok()) {
                broken_ = makeError(ErrorCode::DestinationError,
                                    "session: the journal '" + toUtf8(path_) +
                                        "' is broken: a record could not be written, nor removed again: " +
                                        written.error().message,
                                    written.error().systemErrorCode);
            }
            return written;
        }
        end_ = start + bytes.size();
        mine = ++written_;
    }
    // Group commit: a flush covers every record written before it began, so
    // a thread whose record an earlier flush covered does not flush again.
    const std::lock_guard flushLock(flushMutex_);
    if (flushed_ >= mine) {
        return success();
    }
    std::uint64_t target = 0;
    {
        const std::lock_guard lock(writeMutex_);
        target = written_;
    }
    if (Status flushed = file_.flush(); !flushed.ok()) {
        // The record may reach the device or not: what the session holds and
        // what the file holds may differ, so nothing more is written.
        const std::lock_guard lock(writeMutex_);
        broken_ = makeError(ErrorCode::DestinationError,
                            "session: the journal '" + toUtf8(path_) + "' is broken: it could not be flushed: " +
                                flushed.error().message,
                            flushed.error().systemErrorCode);
        return *broken_;
    }
    flushed_ = target;
    return success();
}

Status JournalFile::append(const RecordPayload& payload, SessionTime time) {
    const RecordType type = typeOf(payload);
    const std::uint16_t flags = type == RecordType::Damage ? kRecordOptional : 0;
    return append(type, flags, time, encodePayload(payload));
}

Status JournalFile::truncate(std::uint64_t end) {
    const std::lock_guard lock(writeMutex_);
    if (broken_.has_value()) {
        return *broken_;
    }
    if (Status cut = file_.truncate(end); !cut.ok()) {
        return cut;
    }
    if (Status flushed = file_.flush(); !flushed.ok()) {
        return flushed;
    }
    end_ = end;
    return success();
}

void JournalFile::markBroken(Error error) {
    const std::lock_guard lock(writeMutex_);
    if (!broken_.has_value()) {
        broken_ = std::move(error);
    }
}

std::uint64_t JournalFile::end() const {
    const std::lock_guard lock(writeMutex_);
    return end_;
}

bool JournalFile::broken() const {
    const std::lock_guard lock(writeMutex_);
    return broken_.has_value();
}

}  // namespace recovery::session
