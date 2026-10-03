#pragma once

// Private to the exFAT module.

#include "filesystem/exfat/exfat_boot_sector.hpp"
#include "filesystem/exfat/exfat_names.hpp"
#include "filesystem/filesystem.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::filesystem::exfat {

// Allocation Bitmap directory entry (type 0x81).
struct BitmapEntryInfo {
    std::uint8_t flags = 0;  // bit 0: which FAT/bitmap pair this is
    std::uint32_t firstCluster = 0;
    std::uint64_t length = 0;
};

// Up-case Table directory entry (type 0x82).
struct UpcaseEntryInfo {
    std::uint32_t checksum = 0;
    std::uint32_t firstCluster = 0;
    std::uint64_t length = 0;
};

struct ParseOutput {
    std::vector<DirectoryEntry> entries;  // active and deleted files and directories
    std::vector<std::string> problems;    // malformed entry sets, reported as scan issues
    std::vector<BitmapEntryInfo> bitmaps;
    std::vector<UpcaseEntryInfo> upcaseTables;
    std::optional<std::string> label;
};

// Decodes a stream of 32-byte directory entries into entry sets.
//
// Active sets (File, Stream Extension, File Name entries, then benign
// secondaries) are listed even when their checksum or name hash is wrong,
// with the problem flagged. Deleted sets keep every field (only the InUse
// bit of each entry type is cleared), so they are verified with the entry
// set checksum and dropped when it fails. When a deleted set's File entry
// was overwritten, the surviving Stream Extension and File Name entries are
// still listed if the name matches the name hash (MetadataIncomplete).
class DirectoryParser {
public:
    DirectoryParser(const BootSector& boot, const UpcaseTable& upcase) noexcept : boot_(boot), upcase_(upcase) {}

    // Processes one entry. Returns false at the end-of-directory marker.
    bool feed(std::span<const std::byte> entry, std::uint64_t volumeOffset, ParseOutput& out);
    // Some entries could not be read: an entry set cannot continue across the gap.
    void interrupt(ParseOutput& out);
    // No more entries.
    void finish(ParseOutput& out) { interrupt(out); }

private:
    enum class Pending : std::uint8_t {
        None,
        File,         // active File entry set
        DeletedFile,  // deleted File entry set
        Orphan,       // deleted Stream Extension + File Name entries without their File entry
        Skip,         // benign primary entry and its secondaries: ignored
    };
    struct Slot {
        std::array<std::byte, kDirectoryEntrySize> bytes{};
        std::uint64_t offset = 0;

        [[nodiscard]] std::uint8_t type() const noexcept { return static_cast<std::uint8_t>(bytes[0]); }
    };

    void process(const Slot& slot, ParseOutput& out);
    void start(const Slot& slot, ParseOutput& out);
    [[nodiscard]] bool accepts(std::uint8_t type) const noexcept;
    void complete(ParseOutput& out);
    void abandon(ParseOutput& out);
    // A deleted set that could not be used: its later entries may still hold
    // an orphaned Stream Extension, so they are processed again on their own.
    void salvage(const std::vector<Slot>& slots, ParseOutput& out);
    // Returns an empty string on success, otherwise why the set was rejected.
    [[nodiscard]] std::string buildFileEntry(const std::vector<Slot>& slots, bool deleted, bool complete,
                                             ParseOutput& out) const;
    [[nodiscard]] bool buildOrphanEntry(const std::vector<Slot>& slots, ParseOutput& out) const;
    void checkName(const std::u16string& units, std::uint16_t storedHash, DirectoryEntry& entry) const;

    const BootSector& boot_;
    const UpcaseTable& upcase_;
    Pending pending_ = Pending::None;
    std::vector<Slot> slots_;
    std::size_t expected_ = 0;
};

// Decodes an exFAT timestamp (FAT date in the high 16 bits, FAT time in the
// low 16), its 10 ms increment and its UTC offset byte. Returns nullopt for
// an unset (zero) timestamp; sets `invalid` when fields are out of range.
// A valid UTC offset converts the value to UTC (Timestamp::local = false).
[[nodiscard]] std::optional<Timestamp> decodeTimestamp(std::uint32_t raw, std::uint8_t tenMilliseconds,
                                                       std::uint8_t utcOffset, bool& invalid);

}  // namespace recovery::filesystem::exfat
