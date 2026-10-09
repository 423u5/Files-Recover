#pragma once

// Text for people, as the reports and the CLI write it: sizes, counts, times
// and durations, sources and scan stages, tables, and strings from untrusted
// disks made safe to print.

#include "filesystem/filesystem.hpp"
#include "recovery/result.hpp"
#include "scan/scan_state.hpp"
#include "session/session_types.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::report {

// `text` made safe to print on a terminal. Names, labels and tags come from
// untrusted disks: printed as they are, a name could move the cursor, rewrite
// earlier output (escape sequences) or disguise its extension (bidirectional
// overrides). Invalid UTF-8 becomes U+FFFD; C0 controls and DEL become \xNN;
// C1 controls, the bidirectional formatting characters (U+061C, U+200E,
// U+200F, U+202A-U+202E, U+2066-U+2069) and the line and paragraph
// separators (U+2028, U+2029) become \u{XXXX}.
[[nodiscard]] std::string printable(std::string_view text);

// The path as UTF-8, printable.
[[nodiscard]] std::string displayPath(const std::filesystem::path& path);

// A path from UTF-8 (a session records its paths so). InvalidInput when the
// text is not valid UTF-8.
[[nodiscard]] Result<std::filesystem::path> pathFromUtf8(std::string_view text);

// "512 B", "1.5 KiB", "2.0 MiB", ... (powers of 1024, one decimal).
[[nodiscard]] std::string formatSize(std::uint64_t bytes);
// "2,097,152 bytes (2.0 MiB)"; "512 bytes" below 1 KiB.
[[nodiscard]] std::string formatBytes(std::uint64_t bytes);
// "1,234,567"
[[nodiscard]] std::string formatCount(std::uint64_t count);
// "12.5 MiB/s"
[[nodiscard]] std::string formatRate(std::uint64_t bytesPerSecond);
// "1:02:03" (hours, minutes, seconds).
[[nodiscard]] std::string formatDuration(std::chrono::milliseconds duration);
// "45.2%" of `done` out of `total` ("" when the total is unknown).
[[nodiscard]] std::string formatPercent(std::uint64_t done, std::uint64_t total);
// "2026-10-08 10:15:30 UTC"
[[nodiscard]] std::string formatTime(session::SessionTime time);
// "2026-10-08T10:15:30.125Z"
[[nodiscard]] std::string formatIsoTime(session::SessionTime time);
// A filesystem time: "2024-05-01 12:00:00 UTC", or "2024-05-01 12:00:00
// (local time)" for FAT's local times, which have no zone.
[[nodiscard]] std::string formatTimestamp(const filesystem::Timestamp& timestamp);
// ISO 8601: with "Z" for UTC, without a zone for local times.
[[nodiscard]] std::string formatIsoTimestamp(const filesystem::Timestamp& timestamp);

// "disk image E:\card.img" or "physical disk 2 (Vendor Product)".
[[nodiscard]] std::string describeSource(storage::SourceType type, std::string_view path,
                                         std::optional<std::uint32_t> disk, std::string_view vendor,
                                         std::string_view product);

// "source pass": a scan stage's name for people.
[[nodiscard]] std::string stageName(scan::ScanStage stage);

// Columns of text, aligned. Widths count code points (wide East Asian
// characters take one column too).
class Table {
public:
    struct Column {
        std::string header;
        bool alignRight = false;
    };

    explicit Table(std::vector<Column> columns);

    void addRow(std::vector<std::string> cells);
    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }
    // Prints the header and the rows, each line after `indent`. The last
    // column is not padded.
    void print(std::ostream& out, std::string_view indent = "  ") const;

private:
    std::vector<Column> columns_;
    std::vector<std::vector<std::string>> rows_;
};

}  // namespace recovery::report
