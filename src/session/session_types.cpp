#include "session/session_types.hpp"

#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/text.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace recovery::session {

namespace {

bool unreadable(storage::ReadStatus status) noexcept {
    return status == storage::ReadStatus::IoError || status == storage::ReadStatus::PartialRead;
}

// Feeds `length` bytes of the source at `offset` to `hash`, preceded by the
// offset and the length. A read that fails with an I/O error is repeated
// sector by sector; sectors that still fail are hashed as zeros.
Status hashBlock(storage::IStorageSource& source, std::uint64_t offset, std::uint64_t length, Sha256& hash,
                 SourceFingerprint& fingerprint) {
    std::array<std::byte, 16> position{};
    storeLe64(position, 0, offset);
    storeLe64(position, 8, length);
    hash.update(position);

    std::vector<std::byte> buffer(static_cast<std::size_t>(length));
    const storage::ReadResult whole = source.read(ByteOffset{offset}, buffer);
    if (whole.ok()) {
        hash.update(buffer);
        fingerprint.bytes += length;
        return success();
    }
    if (!unreadable(whole.status)) {
        return storage::toError(whole);
    }
    const std::uint64_t sector = source.sectorSize();
    for (std::uint64_t done = 0; done < length;) {
        const std::uint64_t piece = std::min(sector, length - done);
        const std::span<std::byte> part(buffer.data() + done, static_cast<std::size_t>(piece));
        const storage::ReadResult read = source.read(ByteOffset{offset + done}, part);
        if (read.ok()) {
            hash.update(part);
        } else if (unreadable(read.status)) {
            hash.updateZeros(piece);
            fingerprint.unreadableBytes += piece;
        } else {
            return storage::toError(read);
        }
        fingerprint.bytes += piece;
        done += piece;
    }
    return success();
}

}  // namespace

std::string_view toString(SessionState state) noexcept {
    switch (state) {
    case SessionState::Started:
        return "started";
    case SessionState::Paused:
        return "paused";
    case SessionState::Cancelled:
        return "cancelled";
    case SessionState::Completed:
        return "completed";
    case SessionState::Failed:
        return "failed";
    }
    return "unknown";
}

SessionTime sessionNow() noexcept {
    return std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

Result<SourceFingerprint> fingerprintSource(storage::IStorageSource& source, const CancellationToken& cancel) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "session: the source is not open");
    }
    const std::uint64_t size = source.size();
    SourceFingerprint fingerprint;
    Sha256 hash;
    if (Status hashed = hashBlock(source, 0, std::min(SourceFingerprint::kBlock, size), hash, fingerprint);
        !hashed.ok()) {
        return hashed.error();
    }
    Result<partition::PartitionTable> table = partition::readPartitionTable(source);
    if (!table.ok()) {
        return table.error();
    }
    for (const partition::Partition& partition : table->partitions) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "session: fingerprinting the source was cancelled");
        }
        // Every partition the table lists lies inside the device.
        const std::uint64_t length = std::min(SourceFingerprint::kBlock, partition.size);
        if (length == 0 || partition.offset > size || length > size - partition.offset) {
            continue;
        }
        if (Status hashed = hashBlock(source, partition.offset, length, hash, fingerprint); !hashed.ok()) {
            return hashed.error();
        }
    }
    fingerprint.digest = hash.finish();
    return fingerprint;
}

Result<SessionSource> describeSource(storage::IStorageSource& source, const CancellationToken& cancel) {
    Result<SourceFingerprint> fingerprint = fingerprintSource(source, cancel);
    if (!fingerprint.ok()) {
        return fingerprint.error();
    }
    const storage::SourceInfo info = source.getInfo();
    SessionSource described;
    described.type = info.type;
    described.path = toUtf8(info.path);
    described.size = source.size();
    described.sectorSize = source.sectorSize();
    described.physicalSectorSize = info.physicalSectorSize;
    described.diskNumber = info.diskNumber;
    described.vendor = info.vendor;
    described.product = info.product;
    described.removable = info.removable;
    described.fingerprint = *fingerprint;
    return described;
}

Status checkSameSource(const SessionSource& recorded, storage::IStorageSource& source,
                       const CancellationToken& cancel) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "session: the source is not open");
    }
    const storage::SourceInfo info = source.getInfo();
    if (info.type != recorded.type) {
        return makeError(ErrorCode::InvalidInput, "session: the source is a " +
                                                      std::string(storage::toString(info.type)) + ", the session's a " +
                                                      std::string(storage::toString(recorded.type)));
    }
    const std::string path = toUtf8(info.path);
    if (path != recorded.path) {
        return makeError(ErrorCode::InvalidInput,
                         "session: the source is '" + path + "', the session's is '" + recorded.path + "'");
    }
    if (source.size() != recorded.size || source.sectorSize() != recorded.sectorSize) {
        return makeError(ErrorCode::InvalidInput,
                         "session: the source holds " + std::to_string(source.size()) + " bytes in sectors of " +
                             std::to_string(source.sectorSize()) + ", the session's " + std::to_string(recorded.size) +
                             " in sectors of " + std::to_string(recorded.sectorSize));
    }
    Result<SourceFingerprint> fingerprint = fingerprintSource(source, cancel);
    if (!fingerprint.ok()) {
        return fingerprint.error();
    }
    if (*fingerprint != recorded.fingerprint) {
        return makeError(ErrorCode::InvalidInput,
                         "session: the source is not the one the session was made for, or it has changed since: "
                         "its partition table or the start of its volumes differ (fingerprint " +
                             fingerprint->digest.hex().substr(0, 16) + ", the session's " +
                             recorded.fingerprint.digest.hex().substr(0, 16) + ")");
    }
    return success();
}

}  // namespace recovery::session
