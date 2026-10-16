#include "session/session_types.hpp"

#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/text.hpp"
#include "storage/physical_disk_source.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace recovery::session {

namespace {

// ERROR_ACCESS_DENIED: opening a disk needs administrator rights.
constexpr std::uint32_t kAccessDenied = 5;

bool unreadable(storage::ReadStatus status) noexcept {
    return status == storage::ReadStatus::IoError || status == storage::ReadStatus::PartialRead;
}

// "SanDisk Ultra", or what stands for no model.
std::string modelName(std::string_view vendor, std::string_view product) {
    std::string model(vendor);
    if (!product.empty()) {
        model += model.empty() ? "" : " ";
        model += product;
    }
    return model.empty() ? std::string("device of no model name") : "'" + model + "'";
}

// The number of the disk a session records: its own, or its path's.
std::optional<std::uint32_t> recordedDiskNumber(const SessionSource& recorded) {
    if (recorded.diskNumber.has_value()) {
        return recorded.diskNumber;
    }
    try {
        const std::filesystem::path path(std::u8string(recorded.path.begin(), recorded.path.end()));
        return storage::PhysicalDiskSource::parseDevicePath(path.native());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// "disk 2 when last used (SanDisk Ultra, serial 4C53..., 30752636928 bytes)".
std::string recordedDiskName(const SessionSource& recorded) {
    const std::optional<std::uint32_t> number = recordedDiskNumber(recorded);
    std::string text = number.has_value() ? "disk " + std::to_string(*number) + " when last used" : "the disk";
    std::string details = recorded.vendor;
    if (!recorded.product.empty()) {
        details += details.empty() ? "" : " ";
        details += recorded.product;
    }
    if (!recorded.serialNumber.empty()) {
        details += details.empty() ? "" : ", ";
        details += "serial " + recorded.serialNumber;
    }
    details += details.empty() ? "" : ", ";
    details += std::to_string(recorded.size) + " bytes";
    return text + " (" + details + ")";
}

// An engine message without its "session: " prefix, to quote it.
std::string withoutPrefix(const std::string& message) {
    constexpr std::string_view kPrefix = "session: ";
    return message.starts_with(kPrefix) ? message.substr(kPrefix.size()) : message;
}

// Attached disk `disk` as a source the destination guard knows.
storage::SourceInfo attachedSource(const storage::AttachedDisk& disk) {
    storage::SourceInfo info;
    info.type = storage::SourceType::PhysicalDisk;
    info.path = storage::PhysicalDiskSource::devicePathFor(disk.number);
    info.sizeBytes = disk.sizeBytes;
    info.logicalSectorSize = disk.logicalSectorSize;
    info.diskNumber = disk.number;
    info.vendor = disk.vendor;
    info.product = disk.product;
    info.removable = disk.removable;
    info.serialNumber = disk.serialNumber;
    return info;
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
    described.serialNumber = info.serialNumber;
    described.fingerprint = *fingerprint;
    return described;
}

std::string deviceDifference(const SessionSource& recorded, std::string_view vendor, std::string_view product,
                             std::string_view serialNumber) {
    if (!recorded.serialNumber.empty()) {
        if (serialNumber == recorded.serialNumber) {
            return {};
        }
        if (serialNumber.empty()) {
            return "it reports no serial number, the session's disk '" + recorded.serialNumber + "'";
        }
        return "its serial number is '" + std::string(serialNumber) + "', the session's disk's '" +
               recorded.serialNumber + "'";
    }
    if (vendor == recorded.vendor && product == recorded.product) {
        return {};
    }
    return "it is a " + modelName(vendor, product) + ", the session's disk a " +
           modelName(recorded.vendor, recorded.product);
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
    if (info.type == storage::SourceType::PhysicalDisk) {
        // Under any number (L147): the same device.
        if (const std::string different = deviceDifference(recorded, info.vendor, info.product, info.serialNumber);
            !different.empty()) {
            return makeError(ErrorCode::InvalidInput,
                             "session: the disk is another device than the session's: " + different);
        }
    } else if (const std::string path = toUtf8(info.path); path != recorded.path) {
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

Result<FoundSessionDisk> findSessionDisk(const SessionSource& recorded, const SessionDiskOpener& open,
                                         const storage::DiskLister& list, const CancellationToken& cancel) {
    if (recorded.type != storage::SourceType::PhysicalDisk) {
        return makeError(ErrorCode::InvalidInput, "session: the session's source is not a physical disk");
    }
    if (!open) {
        return makeError(ErrorCode::InvalidInput, "session: no way to open a disk was given");
    }
    // Why each disk tried is not the session's.
    std::vector<std::string> tried;
    // Disk `number` when it is the session's; null when it is not (why is in
    // `tried`); an error that ends the search.
    const auto check = [&](std::uint32_t number) -> Result<std::unique_ptr<storage::IStorageSource>> {
        const std::string name = "disk " + std::to_string(number) + ": ";
        Result<std::unique_ptr<storage::IStorageSource>> opened = open(number);
        if (!opened.ok() || *opened == nullptr) {
            const Error error = opened.ok() ? makeError(ErrorCode::InternalError, "not opened") : opened.error();
            if (error.systemErrorCode == kAccessDenied) {
                return error;
            }
            tried.push_back(name + error.message);
            return std::unique_ptr<storage::IStorageSource>();
        }
        const Status same = checkSameSource(recorded, **opened, cancel);
        if (same.ok()) {
            return std::move(opened).value();
        }
        if (same.error().code == ErrorCode::Cancelled) {
            return same.error();
        }
        tried.push_back(name + withoutPrefix(same.error().message));
        return std::unique_ptr<storage::IStorageSource>();
    };
    const auto joined = [&tried] {
        std::string text;
        for (const std::string& reason : tried) {
            text += (text.empty() ? "" : "; ") + reason;
        }
        return text;
    };

    const std::optional<std::uint32_t> recordedNumber = recordedDiskNumber(recorded);
    if (recordedNumber.has_value()) {
        Result<std::unique_ptr<storage::IStorageSource>> found = check(*recordedNumber);
        if (!found.ok()) {
            return found.error();
        }
        if (*found != nullptr) {
            return FoundSessionDisk{std::move(found).value(), *recordedNumber, false};
        }
    }
    const std::string sought = "session: the session's disk, " + recordedDiskName(recorded) + ",";
    Result<std::vector<storage::AttachedDisk>> attached =
        list ? list()
             : Result<std::vector<storage::AttachedDisk>>(makeError(ErrorCode::InvalidInput, "no disk list was given"));
    if (!attached.ok()) {
        return makeError(ErrorCode::InvalidInput, sought + " is not found: " + joined() +
                                                      "; the disks attached cannot be listed to look for it under "
                                                      "another number: " +
                                                      attached.error().message);
    }
    std::vector<FoundSessionDisk> matches;
    for (const storage::AttachedDisk& disk : *attached) {
        if (cancel.isCancellationRequested()) {
            return makeError(ErrorCode::Cancelled, "session: looking for the session's disk was cancelled");
        }
        // A disk whose size the system does not tell is tried too: opening it tells.
        const bool otherSize = disk.sizeBytes != 0 &&
                               (disk.sizeBytes != recorded.size || disk.logicalSectorSize != recorded.sectorSize);
        if ((recordedNumber.has_value() && disk.number == *recordedNumber) || otherSize) {
            continue;
        }
        if (const std::string different = deviceDifference(recorded, disk.vendor, disk.product, disk.serialNumber);
            !different.empty()) {
            tried.push_back("disk " + std::to_string(disk.number) + ": another device: " + different);
            continue;
        }
        Result<std::unique_ptr<storage::IStorageSource>> found = check(disk.number);
        if (!found.ok()) {
            return found.error();
        }
        if (*found != nullptr) {
            matches.push_back(FoundSessionDisk{std::move(found).value(), disk.number, true});
        }
    }
    if (matches.size() == 1) {
        return std::move(matches.front());
    }
    if (matches.size() > 1) {
        std::string numbers;
        for (const FoundSessionDisk& match : matches) {
            numbers += (numbers.empty() ? "" : ", ") + std::to_string(match.number);
        }
        return makeError(ErrorCode::InvalidInput,
                         sought + " is attached " + std::to_string(matches.size()) + " times: disks " + numbers +
                             " are the same device with the same content (copies in readers of one model?): "
                             "detach all but one");
    }
    return makeError(ErrorCode::InvalidInput,
                     sought + " is not attached, or it has changed" +
                         (tried.empty() ? std::string(": no disk attached has its size and sector size")
                                        : ": " + joined()));
}

std::vector<storage::SourceInfo> possibleSessionSources(const SessionSource& recorded,
                                                        const storage::DiskLister& list) {
    storage::SourceInfo own;
    own.type = recorded.type;
    try {
        own.path = std::filesystem::path(std::u8string(recorded.path.begin(), recorded.path.end()));
    } catch (const std::exception&) {
        own.path.clear();
    }
    own.sizeBytes = recorded.size;
    own.logicalSectorSize = recorded.sectorSize;
    own.physicalSectorSize = recorded.physicalSectorSize;
    own.vendor = recorded.vendor;
    own.product = recorded.product;
    own.removable = recorded.removable;
    own.serialNumber = recorded.serialNumber;
    if (recorded.type != storage::SourceType::PhysicalDisk) {
        return {own};
    }
    own.diskNumber = recordedDiskNumber(recorded);
    Result<std::vector<storage::AttachedDisk>> attached =
        list ? list()
             : Result<std::vector<storage::AttachedDisk>>(makeError(ErrorCode::InvalidInput, "no disk list was given"));
    if (!attached.ok() || !own.diskNumber.has_value()) {
        // Which disk it is now is not known: the one it was (the guard
        // refuses everything for a disk of no number).
        return {own};
    }
    std::vector<storage::SourceInfo> sources;
    bool recordedListed = false;
    for (const storage::AttachedDisk& disk : *attached) {
        recordedListed = recordedListed || disk.number == *own.diskNumber;
        // A disk whose size the system does not tell may be the session's too.
        const bool sameSize = disk.sizeBytes == 0 ||
                              (disk.sizeBytes == recorded.size && disk.logicalSectorSize == recorded.sectorSize);
        if (sameSize && deviceDifference(recorded, disk.vendor, disk.product, disk.serialNumber).empty()) {
            sources.push_back(attachedSource(disk));
        }
    }
    // The list leaves out a disk it cannot ask about: the recorded one may be there.
    if (!recordedListed) {
        sources.push_back(own);
    }
    return sources;
}

}  // namespace recovery::session
