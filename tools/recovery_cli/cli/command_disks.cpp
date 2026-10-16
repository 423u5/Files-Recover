// recovery disks: the physical disks attached, as --source names them, with
// their size, bus, model, serial number and drive letters (L177). Nothing is
// read from a disk: Windows is asked what it knows of each one, through
// handles that can neither read nor change anything, so listing them needs
// no administrator rights (reading one does).

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "cli/sessions.hpp"
#include "recovery/text.hpp"
#include "storage/physical_disk_source.hpp"

#include <stdlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

namespace recovery::cli {

namespace {

constexpr OptionSpec kSessionsDirOption{kSessionsDirName, "", "FOLDER",
                                        "The sessions folder, to tell its disk (default "
                                        "%LOCALAPPDATA%\\RecoveryEngine\\Sessions)"};

constexpr std::array kOptions = {kSessionsDirOption, kQuietOption, kHelpOption};

// The nearest part of `path` that exists (itself or a parent), to tell the
// disk of a folder not created yet; empty when none does.
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

// The disks `path` is on (none when that cannot be told).
std::vector<std::uint32_t> disksOf(const std::filesystem::path& path, Context& context) {
    if (const std::filesystem::path existing = existingPart(path); !existing.empty()) {
        if (Result<std::vector<std::uint32_t>> on = context.diskResolver()(existing); on.ok()) {
            return std::move(on).value();
        }
    }
    return {};
}

// %SystemRoot%, when it is set.
std::optional<std::filesystem::path> windowsFolder() {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"SystemRoot") != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path folder(value);
    free(value);
    return folder.empty() ? std::nullopt : std::optional<std::filesystem::path>(folder);
}

ExitCode runDisks(const ParsedOptions& options, Context& context) {
    Result<std::vector<storage::AttachedDisk>> listed = context.diskLister()();
    if (!listed.ok()) {
        return context.fail("cannot list the disks", listed.error());
    }
    std::vector<storage::AttachedDisk> disks = std::move(listed).value();
    std::sort(disks.begin(), disks.end(),
              [](const storage::AttachedDisk& a, const storage::AttachedDisk& b) { return a.number < b.number; });

    const std::optional<std::filesystem::path> windows = windowsFolder();
    const std::vector<std::uint32_t> system = windows.has_value() ? disksOf(*windows, context) : std::vector<std::uint32_t>{};
    std::vector<std::uint32_t> sessions;
    if (Result<std::filesystem::path> root = sessionsRoot(options, context.environment()); root.ok()) {
        sessions = disksOf(*root, context);
    }
    const auto holds = [](const std::vector<std::uint32_t>& numbers, std::uint32_t number) {
        return std::find(numbers.begin(), numbers.end(), number) != numbers.end();
    };

    std::ostream& out = context.out();
    if (disks.empty()) {
        out << "No physical disks are attached (or Windows lists none).\n";
        return ExitCode::Success;
    }
    report::Table table({{"Disk"}, {"Size", true}, {"Bus"}, {"Model"}, {"Letters"}, {"Serial number"}, {"Notes"}});
    for (const storage::AttachedDisk& disk : disks) {
        std::string model = disk.vendor;
        if (!disk.product.empty()) {
            model += model.empty() ? "" : " ";
            model += disk.product;
        }
        std::string letters;
        for (const std::filesystem::path& root : disk.volumes) {
            std::string letter = toUtf8(root.root_name());
            letters += (letters.empty() ? "" : " ") + letter;
        }
        std::string notes;
        const auto note = [&notes](std::string_view text) {
            notes += (notes.empty() ? "" : ", ") + std::string(text);
        };
        if (disk.removable.value_or(false)) {
            note("removable");
        }
        if (holds(system, disk.number)) {
            note("holds Windows");
        }
        if (holds(sessions, disk.number)) {
            note("holds the sessions folder");
        }
        table.addRow({toUtf8(storage::PhysicalDiskSource::devicePathFor(disk.number)),
                      disk.sizeBytes != 0 ? formatSize(disk.sizeBytes) : std::string("no medium"), printable(disk.bus),
                      printable(model), letters, printable(disk.serialNumber), notes});
    }
    table.print(out, "");
    out.flush();
    context.note("Give a disk to --source as it is listed, or as a drive letter on it (the whole disk is read). "
                 "Reading a disk needs administrator rights; listing them does not.");
    return ExitCode::Success;
}

}  // namespace

const CommandSpec& disksCommand() {
    static const CommandSpec command{
        "disks",
        "List the physical disks a scan can read",
        "recovery disks [options]",
        "Lists the physical disks attached: the name --source takes (\\\\.\\PhysicalDriveN), size, bus, model,\n"
        "serial number and drive letters, and which disk holds Windows and which the sessions folder (a\n"
        "session is never kept on the disk it scans: scanning that disk needs --sessions-dir on another).\n"
        "Nothing is read from a disk, so no administrator rights are needed. A memory card's serial\n"
        "number is its card reader's.",
        kOptions,
        &runDisks,
    };
    return command;
}

}  // namespace recovery::cli
