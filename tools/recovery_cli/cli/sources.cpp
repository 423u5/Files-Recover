#include "cli/sources.hpp"

#include "cli/format.hpp"
#include "recovery/text.hpp"
#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"

#include <cwctype>
#include <system_error>
#include <utility>

namespace recovery::cli {

namespace {

// Win32 error codes the messages explain.
constexpr std::uint32_t kFileNotFound = 2;
constexpr std::uint32_t kPathNotFound = 3;
constexpr std::uint32_t kAccessDenied = 5;
constexpr std::uint32_t kSharingViolation = 32;

std::shared_ptr<storage::IDeviceOpener> diskOpener(Context& context) {
    return context.environment().diskOpener ? context.environment().diskOpener : storage::makePlatformDeviceOpener();
}

std::string withHint(std::string message, const Error& error, bool disk) {
    message += ": " + describe(error);
    switch (error.systemErrorCode) {
    case kAccessDenied:
        message += disk ? ". Reading a physical disk needs administrator rights: run the command from an elevated "
                          "prompt"
                        : ". Check that you may read the file";
        break;
    case kSharingViolation:
        message += ". Another program has it open for writing; close it and try again";
        break;
    case kFileNotFound:
    case kPathNotFound:
        message += disk ? ". There is no such disk: see Disk Management for the disk numbers" : "";
        break;
    default:
        break;
    }
    return message;
}

// "\\.\PhysicalDrive", in any case, at the start of `path`.
bool isPhysicalDrivePrefix(std::wstring_view path) {
    constexpr std::wstring_view kPrefix = L"\\\\.\\physicaldrive";
    if (path.size() < kPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
        if (std::towlower(path[i]) != kPrefix[i]) {
            return false;
        }
    }
    return true;
}

// A drive ("E:", "E:\") rather than a file.
bool isDrive(const std::filesystem::path& path) {
    return path.has_root_name() && path.relative_path().empty();
}

// "F:", "F:\" or "F:/", an ASCII letter: a drive as --source spells it.
bool isDriveSpelling(std::string_view text) {
    if ((text.size() != 2 && text.size() != 3) || text[1] != ':') {
        return false;
    }
    const char letter = text[0];
    const bool ascii = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
    return ascii && (text.size() == 2 || text[2] == '\\' || text[2] == '/');
}

Result<OpenedSource> openDisk(std::uint32_t disk, Context& context) {
    OpenedSource opened;
    opened.source = std::make_unique<storage::PhysicalDiskSource>(disk, diskOpener(context));
    if (Status open = opened.source->open(); !open.ok()) {
        const Error& error = open.error();
        const std::string device = toUtf8(storage::PhysicalDiskSource::devicePathFor(disk));
        return makeError(error.code, withHint("cannot open " + device, error, true), error.systemErrorCode);
    }
    opened.info = opened.source->getInfo();
    return opened;
}

// The disk `drive` ("F:\") is on, whole (L177): the engine reads whole disks.
Result<OpenedSource> openDriveDisk(const std::filesystem::path& drive, Context& context) {
    const std::string shown = displayPath(drive.root_name());
    const std::string giveDisk =
        "give the disk itself, --source \\\\.\\PhysicalDriveN (recovery disks lists the disks)";
    const Result<std::vector<std::uint32_t>> disks = context.diskResolver()(drive);
    if (!disks.ok()) {
        return makeError(ErrorCode::InvalidInput,
                         "cannot tell which disk " + shown + " is on (" + disks.error().message + "): " + giveDisk,
                         disks.error().systemErrorCode);
    }
    if (disks->empty()) {
        return makeError(ErrorCode::InvalidInput, shown + " is on no physical disk: " + giveDisk);
    }
    if (disks->size() > 1) {
        std::string numbers;
        for (const std::uint32_t disk : *disks) {
            numbers += (numbers.empty() ? "" : ", ") + toUtf8(storage::PhysicalDiskSource::devicePathFor(disk));
        }
        return makeError(ErrorCode::InvalidInput,
                         shown + " spans several disks (" + numbers + "): give the one to read with --source");
    }
    context.note(shown + " is on " + toUtf8(storage::PhysicalDiskSource::devicePathFor(disks->front())) +
                 ": the whole disk is read, every volume on it");
    return openDisk(disks->front(), context);
}

Result<OpenedSource> openImage(const std::filesystem::path& path, std::optional<std::uint32_t> sectorSize,
                               bool readMetadata, Context& context) {
    const std::string shown = "'" + displayPath(path) + "'";
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return makeError(ErrorCode::InvalidInput, "the image file " + shown + " does not exist");
    }
    if (status.type() == std::filesystem::file_type::directory) {
        if (isDrive(path)) {
            std::string message = shown + " is a drive, not an image file: the engine reads whole disks";
            const Result<std::vector<std::uint32_t>> disks = context.diskResolver()(path);
            if (disks.ok() && disks->size() == 1) {
                message += "; use --source " + toUtf8(storage::PhysicalDiskSource::devicePathFor(disks->front())) +
                           " for the disk it is on (as an administrator)";
            } else {
                message += "; use --source \\\\.\\PhysicalDriveN for the disk it is on (Disk Management shows N)";
            }
            return makeError(ErrorCode::InvalidInput, std::move(message));
        }
        return makeError(ErrorCode::InvalidInput, shown + " is a folder, not an image file");
    }

    OpenedSource opened;
    std::optional<std::uint32_t> sector = sectorSize;
    if (readMetadata) {
        const std::filesystem::path metadataPath = imaging::metadataPathFor(path);
        if (std::filesystem::exists(metadataPath, ec)) {
            Result<imaging::ImageMetadata> metadata = imaging::readImageMetadata(metadataPath);
            if (!metadata.ok()) {
                context.warning("cannot read the image's metadata '" + displayPath(metadataPath) +
                                "': " + printable(describe(metadata.error())) +
                                ". The regions its source could not be read in are not known: they read as zeros");
            } else {
                if (metadata->state != imaging::ImageState::Completed) {
                    context.warning("the image is not complete (" + std::string(imaging::toString(metadata->state)) +
                                    "): " + formatBytes(metadata->bytesCompleted) + " of " +
                                    formatBytes(metadata->sourceSize) + " were imaged");
                }
                if (sector.has_value() && *sector != metadata->sectorSize) {
                    context.warning("--sector-size " + std::to_string(*sector) + " differs from the image's (" +
                                    std::to_string(metadata->sectorSize) + ")");
                }
                if (!sector.has_value()) {
                    sector = metadata->sectorSize;
                }
                opened.imageMetadata = std::move(metadata).value();
            }
        }
    }

    storage::DiskImageOptions options;
    options.logicalSectorSize = sector.value_or(options.logicalSectorSize);
    opened.source = std::make_unique<storage::DiskImageSource>(path, options);
    if (Status open = opened.source->open(); !open.ok()) {
        const Error& error = open.error();
        return makeError(error.code, withHint("cannot open the image file " + shown, error, false),
                         error.systemErrorCode);
    }
    opened.info = opened.source->getInfo();
    return opened;
}

}  // namespace

Result<SourceSpec> parseSource(std::string_view option, std::string_view text) {
    if (text.empty()) {
        return makeError(ErrorCode::InvalidInput, std::string(option) + " needs a disk image file or a physical disk");
    }
    Result<std::filesystem::path> path = pathFromUtf8(text);
    if (!path.ok()) {
        return path.error();
    }
    SourceSpec spec;
    if (isDriveSpelling(text)) {
        spec.drive = std::filesystem::path(
            std::wstring{static_cast<wchar_t>(std::towupper(static_cast<wint_t>(text[0]))), L':', L'\\'});
        return spec;
    }
    if (const std::optional<std::uint32_t> disk = storage::PhysicalDiskSource::parseDevicePath(path->native());
        disk.has_value()) {
        spec.disk = disk;
        return spec;
    }
    if (isPhysicalDrivePrefix(path->native())) {
        return makeError(ErrorCode::InvalidInput,
                         std::string(option) + " '" + printable(text) + "' is not a physical disk: give \\\\.\\" +
                             "PhysicalDriveN, N from 0 to " +
                             std::to_string(storage::PhysicalDiskSource::kMaxDiskNumber));
    }
    if (storage::isDeviceNamespacePath(*path)) {
        return makeError(ErrorCode::InvalidInput,
                         std::string(option) + " '" + printable(text) +
                             "' is a device the engine does not read: give a disk image file or a whole physical "
                             "disk (\\\\.\\PhysicalDriveN)");
    }
    spec.image = std::move(path).value();
    return spec;
}

Result<OpenedSource> openSource(const SourceSpec& spec, std::optional<std::uint32_t> sectorSize, Context& context) {
    if (spec.disk.has_value()) {
        return openDisk(*spec.disk, context);
    }
    if (spec.drive.has_value()) {
        return openDriveDisk(*spec.drive, context);
    }
    return openImage(spec.image, sectorSize, true, context);
}

Result<OpenedSource> openRecordedSource(const session::SessionSource& recorded, Context& context) {
    switch (recorded.type) {
    case storage::SourceType::PhysicalDisk: {
        // Under the number it had, or under another (L147).
        const session::SessionDiskOpener open =
            [&context](std::uint32_t number) -> Result<std::unique_ptr<storage::IStorageSource>> {
            Result<OpenedSource> opened = openDisk(number, context);
            if (!opened.ok()) {
                return opened.error();
            }
            return std::move(opened->source);
        };
        Result<session::FoundSessionDisk> found = session::findSessionDisk(recorded, open, context.diskLister());
        if (!found.ok()) {
            return found.error();
        }
        OpenedSource opened;
        opened.source = std::move(found->source);
        opened.info = opened.source->getInfo();
        if (found->renumbered) {
            context.note("The session's disk is " + toUtf8(storage::PhysicalDiskSource::devicePathFor(found->number)) +
                         " now (" + printable(recorded.path) + " when last used): the same device" +
                         (opened.info.serialNumber.empty() ? std::string()
                                                           : " (serial " + printable(opened.info.serialNumber) + ")") +
                         ", unchanged.");
        }
        return opened;
    }
    case storage::SourceType::DiskImage: {
        Result<std::filesystem::path> path = pathFromUtf8(recorded.path);
        if (!path.ok()) {
            return path.error();
        }
        return openImage(*path, recorded.sectorSize, false, context);
    }
    case storage::SourceType::Synthetic:
        break;
    }
    return makeError(ErrorCode::InvalidInput, "the session's source (" + std::string(storage::toString(recorded.type)) +
                                                  ") is not a disk or an image file the CLI can open");
}

std::vector<storage::SourceInfo> protectedSourcesOf(const session::SessionSource& recorded, Context& context) {
    return session::possibleSessionSources(recorded, context.diskLister());
}

std::vector<storage::BadRegion> knownBadRegions(const OpenedSource& source) {
    return source.imageMetadata.has_value() ? source.imageMetadata->badRegions : std::vector<storage::BadRegion>{};
}

std::string describeSource(const storage::SourceInfo& info) {
    return describeSource(info.type, toUtf8(info.path), info.diskNumber, info.vendor, info.product);
}

}  // namespace recovery::cli
