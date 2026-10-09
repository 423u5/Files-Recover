#include "api_sources.hpp"

#include "conversions.hpp"
#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/text.hpp"
#include "report/text_format.hpp"
#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"

#include <stdlib.h>

#include <algorithm>
#include <cwctype>
#include <system_error>
#include <utility>

namespace recovery::api::detail {

namespace {

// Win32 error codes the messages explain.
constexpr std::uint32_t kFileNotFound = 2;
constexpr std::uint32_t kPathNotFound = 3;
constexpr std::uint32_t kAccessDenied = 5;
constexpr std::uint32_t kSharingViolation = 32;

}  // namespace

std::optional<std::filesystem::path> environmentFolder(const wchar_t* name) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path folder(value);
    free(value);
    if (folder.empty()) {
        return std::nullopt;
    }
    return folder;
}

namespace {

std::string withHint(std::string message, const Error& error, bool disk) {
    message += ": " + describe(error);
    switch (error.systemErrorCode) {
    case kAccessDenied:
        message += disk ? ". Reading a physical disk needs administrator rights: run the program as an administrator"
                        : ". Check that the file may be read";
        break;
    case kSharingViolation:
        message += ". Another program has it open for writing; close it and try again";
        break;
    case kFileNotFound:
    case kPathNotFound:
        message += disk ? ". There is no such disk (listSources() lists them)" : "";
        break;
    default:
        break;
    }
    return message;
}

// A drive ("E:", "E:\") rather than a file.
bool isDrive(const std::filesystem::path& path) {
    return path.has_root_name() && path.relative_path().empty();
}

Result<OpenedSource> openDisk(std::uint32_t disk, const Platform& platform) {
    if (disk > storage::PhysicalDiskSource::kMaxDiskNumber) {
        return makeError(ErrorCode::InvalidInput,
                         "there is no physical disk " + std::to_string(disk) + ": disks are numbered from 0 to " +
                             std::to_string(storage::PhysicalDiskSource::kMaxDiskNumber));
    }
    OpenedSource opened;
    opened.source = std::make_unique<storage::PhysicalDiskSource>(disk, platform.diskOpener);
    if (Status open = opened.source->open(); !open.ok()) {
        const Error& error = open.error();
        const std::string device = toUtf8(storage::PhysicalDiskSource::devicePathFor(disk));
        return makeError(error.code, withHint("cannot open " + device, error, true), error.systemErrorCode);
    }
    opened.info = opened.source->getInfo();
    return opened;
}

Result<OpenedSource> openImage(const std::filesystem::path& path, std::uint32_t sectorSize, bool readMetadata,
                               const Platform& platform) {
    if (path.empty()) {
        return makeError(ErrorCode::InvalidInput, "no image file was given");
    }
    const std::string shown = "'" + report::displayPath(path) + "'";
    if (storage::isDeviceNamespacePath(path)) {
        return makeError(ErrorCode::InvalidInput, shown +
                                                      " is a device, not an image file: give a physical disk by its "
                                                      "number (SourceRef::physicalDisk)");
    }
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return makeError(ErrorCode::InvalidInput, "the image file " + shown + " does not exist");
    }
    if (status.type() == std::filesystem::file_type::directory) {
        if (isDrive(path)) {
            std::string message = shown + " is a drive, not an image file: the engine reads whole disks";
            const Result<std::vector<std::uint32_t>> disks = platform.diskResolver(path);
            if (disks.ok() && disks->size() == 1) {
                message += "; scan physical disk " + std::to_string(disks->front()) + ", the disk it is on";
            }
            return makeError(ErrorCode::InvalidInput, std::move(message));
        }
        return makeError(ErrorCode::InvalidInput, shown + " is a folder, not an image file");
    }

    OpenedSource opened;
    std::uint32_t sector = sectorSize;
    if (readMetadata) {
        opened.metadataFile = imaging::metadataPathFor(path);
        if (std::filesystem::exists(opened.metadataFile, ec)) {
            Result<imaging::ImageMetadata> metadata = imaging::readImageMetadata(opened.metadataFile);
            if (!metadata.ok()) {
                opened.metadataError = metadata.error();
            } else {
                if (sector == 0) {
                    sector = metadata->sectorSize;
                }
                opened.imageMetadata = std::move(metadata).value();
            }
        }
    }
    storage::DiskImageOptions options;
    if (sector != 0) {
        options.logicalSectorSize = sector;
    }
    opened.source = std::make_unique<storage::DiskImageSource>(path, options);
    if (Status open = opened.source->open(); !open.ok()) {
        const Error& error = open.error();
        return makeError(error.code, withHint("cannot open the image file " + shown, error, false),
                         error.systemErrorCode);
    }
    opened.info = opened.source->getInfo();
    return opened;
}

struct PlannedVolume {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::optional<std::uint32_t> partition;
};

VolumeInfo inspectVolume(const PlannedVolume& planned, storage::IStorageSource& source, diagnostics::Logger* logger) {
    VolumeInfo volume;
    volume.offset = planned.offset;
    volume.size = planned.size;
    volume.partition = planned.partition;
    partition::PartitionSource view(source, planned.offset, planned.size);
    if (Status opened = view.open(); !opened.ok()) {
        volume.error = opened.error();
        return volume;
    }
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(view, planned.offset, {}, logger);
    if (!recovery.ok()) {
        volume.error = recovery.error();
        return volume;
    }
    const filesystem::FilesystemInfo& info = (*recovery)->filesystem().info();
    volume.filesystem = toApi(info.type);
    volume.label = info.label;
    volume.serialNumber = serialText(info.type, info.serialNumber);
    volume.clusterSize = info.clusterSize;
    volume.warnings = info.warnings;
    return volume;
}

std::string diskDescription(const DiskInfo& disk) {
    std::string text = "Disk " + std::to_string(disk.number);
    std::string model = disk.vendor;
    if (!disk.product.empty()) {
        model += model.empty() ? "" : " ";
        model += disk.product;
    }
    std::string details = model;
    const auto add = [&details](const std::string& part) {
        if (!part.empty()) {
            details += details.empty() ? "" : ", ";
            details += part;
        }
    };
    add(disk.size != 0 ? report::formatSize(disk.size) : std::string("no medium"));
    add(disk.bus);
    text += ": " + details;
    if (!disk.driveLetters.empty()) {
        std::string letters;
        for (const std::string& letter : disk.driveLetters) {
            letters += letters.empty() ? "" : " ";
            letters += letter;
        }
        text += " (" + letters + ")";
    }
    return report::printable(text);
}

}  // namespace

Platform platformOf(PlatformHooks hooks) {
    Platform platform;
    platform.diskOpener = hooks.diskOpener ? std::move(hooks.diskOpener) : storage::makePlatformDeviceOpener();
    platform.diskResolver =
        hooks.diskResolver ? std::move(hooks.diskResolver) : storage::makePlatformDiskResolver();
    platform.diskLister = hooks.diskLister ? std::move(hooks.diskLister) : storage::makePlatformDiskLister();
    platform.systemFolder = hooks.systemFolder;
    if (platform.systemFolder.empty()) {
        platform.systemFolder = environmentFolder(L"SystemRoot").value_or(std::filesystem::path());
    }
    platform.playability = std::move(hooks.playability);
    platform.onOperationProgress = std::move(hooks.onOperationProgress);
    platform.maxQueuedFiles = hooks.maxQueuedFiles;
    return platform;
}

Result<OpenedSource> openSource(const SourceRef& source, const Platform& platform) {
    switch (source.kind) {
    case SourceKind::PhysicalDisk:
        return openDisk(source.disk, platform);
    case SourceKind::DiskImage:
        return openImage(source.image, source.sectorSize, true, platform);
    }
    return makeError(ErrorCode::InvalidInput, "unknown kind of source");
}

Result<OpenedSource> openRecordedSource(const session::SessionSource& recorded, const Platform& platform) {
    switch (recorded.type) {
    case storage::SourceType::PhysicalDisk: {
        std::optional<std::uint32_t> disk = recorded.diskNumber;
        if (!disk.has_value()) {
            if (Result<std::filesystem::path> path = report::pathFromUtf8(recorded.path); path.ok()) {
                disk = storage::PhysicalDiskSource::parseDevicePath(path->native());
            }
        }
        if (!disk.has_value()) {
            return makeError(ErrorCode::InvalidInput,
                             "the session's disk '" + report::printable(recorded.path) + "' is not a physical disk");
        }
        return openDisk(*disk, platform);
    }
    case storage::SourceType::DiskImage: {
        Result<std::filesystem::path> path = report::pathFromUtf8(recorded.path);
        if (!path.ok()) {
            return path.error();
        }
        return openImage(*path, recorded.sectorSize, false, platform);
    }
    case storage::SourceType::Synthetic:
        break;
    }
    return makeError(ErrorCode::InvalidInput, "the session's source (" + std::string(storage::toString(recorded.type)) +
                                                  ") is not a disk or an image file");
}

storage::SourceInfo sourceInfoOf(const session::SessionSource& recorded) {
    storage::SourceInfo info;
    info.type = recorded.type;
    if (Result<std::filesystem::path> path = report::pathFromUtf8(recorded.path); path.ok()) {
        info.path = std::move(path).value();
    }
    info.sizeBytes = recorded.size;
    info.logicalSectorSize = recorded.sectorSize;
    info.physicalSectorSize = recorded.physicalSectorSize;
    info.diskNumber = recorded.diskNumber;
    if (info.type == storage::SourceType::PhysicalDisk && !info.diskNumber.has_value()) {
        info.diskNumber = storage::PhysicalDiskSource::parseDevicePath(info.path.native());
    }
    info.vendor = recorded.vendor;
    info.product = recorded.product;
    info.removable = recorded.removable;
    return info;
}

std::vector<storage::BadRegion> knownBadRegions(const OpenedSource& source) {
    return source.imageMetadata.has_value() ? source.imageMetadata->badRegions : std::vector<storage::BadRegion>{};
}

Result<SourceInspection> inspect(OpenedSource& source, diagnostics::Logger* logger) {
    SourceInspection inspection;
    inspection.source = sourceDetailsOf(source.info);
    if (source.imageMetadata.has_value()) {
        inspection.image = imageFileInfoOf(*source.imageMetadata, source.metadataFile);
    }
    Result<partition::PartitionTable> table = partition::readPartitionTable(*source.source);
    if (!table.ok()) {
        return table.error();
    }
    inspection.partitionScheme = toApi(table->scheme);
    for (const partition::Partition& entry : table->partitions) {
        inspection.partitions.push_back(partitionOf(entry));
    }
    for (const partition::PartitionIssue& issue : table->issues) {
        std::string text(partition::toString(issue.kind));
        if (issue.partitionIndex.has_value()) {
            text += " (partition " + std::to_string(*issue.partitionIndex) + ")";
        }
        if (!issue.detail.empty()) {
            text += ": " + issue.detail;
        }
        inspection.partitionIssues.push_back(std::move(text));
    }
    std::vector<PlannedVolume> planned;
    if (table->scheme == partition::PartitionScheme::Mbr || table->scheme == partition::PartitionScheme::Gpt) {
        for (const partition::Partition& entry : table->partitions) {
            planned.push_back(PlannedVolume{entry.offset, entry.size, entry.index});
        }
    } else {
        planned.push_back(PlannedVolume{0, source.info.sizeBytes, std::nullopt});
    }
    for (const PlannedVolume& volume : planned) {
        inspection.volumes.push_back(inspectVolume(volume, *source.source, logger));
    }
    return inspection;
}

std::filesystem::path existingPart(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path part = std::filesystem::absolute(path, ec);
    if (ec) {
        return {};
    }
    while (!part.empty()) {
        if (std::filesystem::exists(part, ec)) {
            return part;
        }
        if (!part.has_relative_path()) {
            return {};
        }
        part = part.parent_path();
    }
    return {};
}

Result<std::vector<DiskInfo>> listDisks(const Platform& platform, const std::filesystem::path& sessionsRoot) {
    Result<std::vector<storage::AttachedDisk>> attached = platform.diskLister();
    if (!attached.ok()) {
        return attached.error();
    }
    const auto disksOf = [&](const std::filesystem::path& path) {
        std::vector<std::uint32_t> numbers;
        if (const std::filesystem::path existing = existingPart(path); !existing.empty()) {
            if (Result<std::vector<std::uint32_t>> on = platform.diskResolver(existing); on.ok()) {
                numbers = std::move(on).value();
            }
        }
        return numbers;
    };
    const std::vector<std::uint32_t> system =
        platform.systemFolder.empty() ? std::vector<std::uint32_t>{} : disksOf(platform.systemFolder);
    const std::vector<std::uint32_t> sessions = disksOf(sessionsRoot);
    const auto holds = [](const std::vector<std::uint32_t>& numbers, std::uint32_t number) {
        return std::find(numbers.begin(), numbers.end(), number) != numbers.end();
    };

    std::vector<DiskInfo> disks;
    disks.reserve(attached->size());
    for (const storage::AttachedDisk& entry : *attached) {
        DiskInfo disk;
        disk.number = entry.number;
        disk.devicePath = toUtf8(storage::PhysicalDiskSource::devicePathFor(entry.number));
        disk.size = entry.sizeBytes;
        disk.sectorSize = entry.logicalSectorSize;
        disk.vendor = entry.vendor;
        disk.product = entry.product;
        disk.removable = entry.removable;
        disk.bus = entry.bus;
        for (const std::filesystem::path& root : entry.volumes) {
            std::string letter = toUtf8(root);
            while (!letter.empty() && (letter.back() == '\\' || letter.back() == '/')) {
                letter.pop_back();
            }
            disk.driveLetters.push_back(std::move(letter));
        }
        disk.system = holds(system, entry.number);
        disk.holdsSessions = holds(sessions, entry.number);
        disk.description = diskDescription(disk);
        disks.push_back(std::move(disk));
    }
    std::sort(disks.begin(), disks.end(), [](const DiskInfo& a, const DiskInfo& b) { return a.number < b.number; });
    return disks;
}

}  // namespace recovery::api::detail
