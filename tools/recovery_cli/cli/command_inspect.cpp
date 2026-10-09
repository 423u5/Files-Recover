// recovery inspect: what a source is (its size, sectors, model), what an
// image's metadata says, its partition table, and the filesystem of each
// volume, as a scan would plan them. Only boot records and partition tables
// are read: nothing is listed or scanned.

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "cli/sources.hpp"
#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/text.hpp"

#include <array>
#include <format>
#include <ostream>

namespace recovery::cli {

namespace {

constexpr OptionSpec kSourceOption{"--source", "", "SOURCE",
                                   "A disk image file, or a physical disk (\\\\.\\PhysicalDriveN)"};
constexpr OptionSpec kSectorSizeOption{"--sector-size", "", "BYTES",
                                       "An image's logical sector size (default: its metadata's, else 512)"};

constexpr std::array kOptions = {kSourceOption, kSectorSizeOption, kLogOption, kQuietOption, kHelpOption};

std::string yesNo(bool value) {
    return value ? "yes" : "no";
}

std::string serialText(filesystem::FilesystemType type, std::uint64_t serial) {
    if (type == filesystem::FilesystemType::Ntfs) {
        return std::format("{:016X}", serial);
    }
    return std::format("{:04X}-{:04X}", (serial >> 16) & 0xFFFFu, serial & 0xFFFFu);
}

void printSource(std::ostream& out, const OpenedSource& opened) {
    const storage::SourceInfo& info = opened.info;
    out << "Source:          " << describeSource(info) << '\n';
    if (info.type == storage::SourceType::PhysicalDisk) {
        out << "Device:          " << displayPath(info.path) << '\n';
    }
    out << "Size:            " << formatBytes(info.sizeBytes) << '\n';
    out << "Sector size:     " << info.logicalSectorSize << " bytes";
    if (info.physicalSectorSize != 0 && info.physicalSectorSize != info.logicalSectorSize) {
        out << " (physical " << info.physicalSectorSize << ")";
    }
    out << '\n';
    if (info.removable.has_value()) {
        out << "Removable:       " << yesNo(*info.removable) << '\n';
    }
    out << "Access:          read-only\n";
    if (opened.imageMetadata.has_value()) {
        const imaging::ImageMetadata& image = *opened.imageMetadata;
        std::uint64_t unreadable = 0;
        for (const storage::BadRegion& region : image.badRegions) {
            unreadable += region.length;
        }
        out << "Image metadata:  " << imaging::toString(image.state) << ", " << formatBytes(image.bytesCompleted)
            << " of " << formatBytes(image.sourceSize) << " imaged\n";
        out << "  Imaged from:   "
            << describeSource(image.sourceType, image.sourcePath, std::nullopt, image.sourceVendor,
                              image.sourceProduct)
            << '\n';
        out << "  Started:       " << printable(image.startedUtc) << "; updated " << printable(image.updatedUtc)
            << '\n';
        out << "  Unreadable:    " << formatBytes(unreadable) << " in " << formatCount(image.badRegions.size())
            << (image.badRegions.size() == 1 ? " region" : " regions") << " (zeros in the image)\n";
    }
}

void printPartitionTable(std::ostream& out, const partition::PartitionTable& table) {
    out << "\nPartition table: ";
    switch (table.scheme) {
    case partition::PartitionScheme::Unknown:
        out << "none recognised (the source is read as one volume)\n";
        break;
    case partition::PartitionScheme::Unpartitioned:
        out << "none: sector 0 holds a volume boot record (the source is one volume)\n";
        break;
    case partition::PartitionScheme::Mbr:
    case partition::PartitionScheme::Gpt:
        out << partition::toString(table.scheme) << ", " << table.partitions.size()
            << (table.partitions.size() == 1 ? " partition\n" : " partitions\n");
        break;
    }
    if (table.gpt.has_value()) {
        const partition::GptInfo& gpt = *table.gpt;
        out << "  Disk GUID:     " << gpt.diskGuid.toString() << '\n';
        out << "  Headers:       primary " << (gpt.primaryValid ? "valid" : "invalid") << ", backup "
            << (gpt.backupValid ? "valid" : "invalid") << '\n';
    }
    if (!table.partitions.empty()) {
        Table rows({{"#", true}, {"Start", true}, {"Size", true}, {"Type"}, {"Notes"}});
        for (const partition::Partition& partition : table.partitions) {
            std::string notes;
            const auto add = [&](std::string_view note) {
                notes += notes.empty() ? "" : ", ";
                notes += note;
            };
            if (!partition.name.empty()) {
                add("\"" + printable(partition.name) + "\"");
            }
            if (partition.bootable) {
                add("bootable");
            }
            if (partition.logical) {
                add("logical");
            }
            if (partition.truncated) {
                add("truncated to the source");
            }
            rows.addRow({std::to_string(partition.index), formatCount(partition.offset), formatSize(partition.size),
                         printable(partition.typeName), notes});
        }
        rows.print(out);
    }
    for (const partition::PartitionIssue& issue : table.issues) {
        out << "  Issue:         " << partition::toString(issue.kind);
        if (issue.partitionIndex.has_value()) {
            out << " (partition " << *issue.partitionIndex << ")";
        }
        if (!issue.detail.empty()) {
            out << ": " << printable(issue.detail);
        }
        out << '\n';
    }
}

struct PlannedVolume {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::optional<std::uint32_t> partition;
};

// The volumes a scan reads: the partitions of an MBR or a GPT, else the whole source.
std::vector<PlannedVolume> planVolumes(const partition::PartitionTable& table, std::uint64_t sourceSize) {
    std::vector<PlannedVolume> volumes;
    if (table.scheme == partition::PartitionScheme::Mbr || table.scheme == partition::PartitionScheme::Gpt) {
        for (const partition::Partition& partition : table.partitions) {
            volumes.push_back(PlannedVolume{partition.offset, partition.size, partition.index});
        }
        return volumes;
    }
    volumes.push_back(PlannedVolume{0, sourceSize, std::nullopt});
    return volumes;
}

void printVolume(std::ostream& out, std::size_t number, const PlannedVolume& volume,
                 storage::IStorageSource& source, Context& context) {
    out << "\nVolume " << number << ": " << formatSize(volume.size) << " at offset " << formatCount(volume.offset);
    if (volume.partition.has_value()) {
        out << " (partition " << *volume.partition << ")";
    }
    out << '\n';
    partition::PartitionSource view(source, volume.offset, volume.size);
    if (Status opened = view.open(); !opened.ok()) {
        out << "  Filesystem:    cannot be read: " << printable(describe(opened.error())) << '\n';
        return;
    }
    Result<std::unique_ptr<FilesystemRecovery>> recovery =
        openFilesystemRecovery(view, volume.offset, {}, context.logger());
    if (!recovery.ok()) {
        out << "  Filesystem:    none the engine reads (" << printable(describe(recovery.error())) << ")\n";
        out << "                 Its files can only be found by carving (recovery scan --mode deep).\n";
        return;
    }
    const filesystem::FilesystemInfo& info = (*recovery)->filesystem().info();
    out << "  Filesystem:    " << filesystem::toString(info.type) << '\n';
    out << "  Label:         " << (info.label.empty() ? "(none)" : printable(info.label)) << '\n';
    out << "  Serial number: " << serialText(info.type, info.serialNumber) << '\n';
    out << "  Cluster size:  " << formatCount(info.clusterSize) << " bytes (" << formatCount(info.clusterCount)
        << " clusters, " << info.bytesPerSector << "-byte sectors)\n";
    out << "  Volume size:   " << formatBytes(info.volumeSize) << '\n';
    for (const std::string& warning : info.warnings) {
        out << "  Warning:       " << printable(warning) << '\n';
    }
}

ExitCode runInspect(const ParsedOptions& options, Context& context) {
    const std::optional<std::string_view> sourceText = options.value(kSourceOption.name);
    if (!sourceText.has_value()) {
        return usageError(context, makeError(ErrorCode::InvalidInput, "--source is required"));
    }
    Result<SourceSpec> spec = parseSource(kSourceOption.name, *sourceText);
    if (!spec.ok()) {
        return usageError(context, spec.error());
    }
    std::optional<std::uint32_t> sectorSize;
    if (const std::optional<std::string_view> text = options.value(kSectorSizeOption.name); text.has_value()) {
        Result<std::uint32_t> parsed = parseSectorSize(kSectorSizeOption.name, *text);
        if (!parsed.ok()) {
            return usageError(context, parsed.error());
        }
        sectorSize = *parsed;
    }

    Result<OpenedSource> opened = openSource(*spec, sectorSize, context);
    if (!opened.ok()) {
        return context.fail("cannot read the source", opened.error());
    }
    if (!openLogOption(options, &opened->info, context)) {
        return ExitCode::Error;
    }
    std::ostream& out = context.out();
    printSource(out, *opened);

    Result<partition::PartitionTable> table = partition::readPartitionTable(*opened->source);
    if (!table.ok()) {
        return context.fail("cannot read the partition table", table.error());
    }
    printPartitionTable(out, *table);
    const std::vector<PlannedVolume> volumes = planVolumes(*table, opened->info.sizeBytes);
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (context.interrupted()) {
            out.flush();
            context.note("Stopped.");
            return ExitCode::Cancelled;
        }
        printVolume(out, i + 1, volumes[i], *opened->source, context);
    }
    out.flush();
    return ExitCode::Success;
}

}  // namespace

const CommandSpec& inspectCommand() {
    static const CommandSpec command{
        "inspect",
        "Show a source: its size, partitions and filesystems",
        "recovery inspect --source SOURCE [options]",
        "Shows what a source is: its size and sectors (and model, for a physical disk), what an image's\n"
        "metadata says, its partition table, and the filesystem of each volume. Only partition tables and\n"
        "boot records are read; the source is never written.",
        kOptions,
        &runInspect,
    };
    return command;
}

}  // namespace recovery::cli
