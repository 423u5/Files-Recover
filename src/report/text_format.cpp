#include "report/text_format.hpp"

#include "report/utf8.hpp"
#include "recovery/text.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <ostream>

namespace recovery::report {

namespace {

// Characters that reorder or break the text around them.
bool isHiddenControl(char32_t value) noexcept {
    return value == 0x061C || value == 0x200E || value == 0x200F || (value >= 0x202A && value <= 0x202E) ||
           (value >= 0x2066 && value <= 0x2069) || value == 0x2028 || value == 0x2029;
}

std::size_t codePoints(std::string_view text) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        (void)decodeUtf8(text, i);
        ++count;
    }
    return count;
}

std::string grouped(std::uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        // A separator before every digit that has a multiple of three digits from it to the end.
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace

std::string printable(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const char32_t value = decodeUtf8(text, i);
        if (value < 0x20 || value == 0x7F) {
            out += std::format("\\x{:02X}", static_cast<unsigned>(value));
        } else if ((value >= 0x80 && value <= 0x9F) || isHiddenControl(value)) {
            out += std::format("\\u{{{:04X}}}", static_cast<unsigned>(value));
        } else {
            appendUtf8(out, value);
        }
    }
    return out;
}

std::string displayPath(const std::filesystem::path& path) {
    return printable(toUtf8(path));
}

Result<std::filesystem::path> pathFromUtf8(std::string_view text) {
    std::u8string units;
    units.reserve(text.size());
    for (const char c : text) {
        units.push_back(static_cast<char8_t>(c));
    }
    try {
        return std::filesystem::path(units);
    } catch (const std::exception&) {
        return makeError(ErrorCode::InvalidInput, "the path '" + printable(text) + "' is not valid UTF-8");
    }
}

std::string formatSize(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    constexpr std::array<std::string_view, 6> kUnits = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double value = static_cast<double>(bytes);
    std::string_view unit;
    // The largest unit the value is at least one of (EiB at most: 2^64 is 16 EiB).
    for (const std::string_view next : kUnits) {
        value /= 1024.0;
        unit = next;
        if (value < 1024.0) {
            break;
        }
    }
    return std::format("{:.1f} {}", value, unit);
}

std::string formatBytes(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
    }
    return grouped(bytes) + " bytes (" + formatSize(bytes) + ")";
}

std::string formatCount(std::uint64_t count) {
    return grouped(count);
}

std::string formatRate(std::uint64_t bytesPerSecond) {
    return formatSize(bytesPerSecond) + "/s";
}

std::string formatDuration(std::chrono::milliseconds duration) {
    const auto total = std::chrono::duration_cast<std::chrono::seconds>(std::max(duration, {})).count();
    return std::format("{}:{:02}:{:02}", total / 3600, (total / 60) % 60, total % 60);
}

std::string formatPercent(std::uint64_t done, std::uint64_t total) {
    if (total == 0) {
        return {};
    }
    const double share = static_cast<double>(std::min(done, total)) * 100.0 / static_cast<double>(total);
    return std::format("{:.1f}%", share);
}

std::string formatTime(session::SessionTime time) {
    return std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(time));
}

std::string formatIsoTime(session::SessionTime time) {
    return formatUtcTimestamp(time);
}

std::string formatTimestamp(const filesystem::Timestamp& timestamp) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp.time);
    return timestamp.local ? std::format("{:%Y-%m-%d %H:%M:%S} (local time)", seconds)
                           : std::format("{:%Y-%m-%d %H:%M:%S} UTC", seconds);
}

std::string formatIsoTimestamp(const filesystem::Timestamp& timestamp) {
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(timestamp.time);
    const auto seconds = std::chrono::floor<std::chrono::seconds>(milliseconds);
    const auto fraction = (milliseconds - seconds).count();
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}{}", seconds, fraction, timestamp.local ? "" : "Z");
}

std::string describeSource(storage::SourceType type, std::string_view path, std::optional<std::uint32_t> disk,
                           std::string_view vendor, std::string_view product) {
    switch (type) {
    case storage::SourceType::PhysicalDisk: {
        std::string text = "physical disk";
        if (disk.has_value()) {
            text += " " + std::to_string(*disk);
        }
        std::string model = printable(vendor);
        if (!product.empty()) {
            model += model.empty() ? "" : " ";
            model += printable(product);
        }
        if (!model.empty()) {
            text += " (" + model + ")";
        }
        return text;
    }
    case storage::SourceType::DiskImage:
        return "disk image " + printable(path);
    case storage::SourceType::Synthetic:
        break;
    }
    return std::string(storage::toString(type)) + " " + printable(path);
}

std::string stageName(scan::ScanStage stage) {
    std::string name(scan::toString(stage));
    for (char& c : name) {
        if (c == '-') {
            c = ' ';
        }
    }
    return name;
}

Table::Table(std::vector<Column> columns) : columns_(std::move(columns)) {}

void Table::addRow(std::vector<std::string> cells) {
    cells.resize(columns_.size());
    rows_.push_back(std::move(cells));
}

void Table::print(std::ostream& out, std::string_view indent) const {
    std::vector<std::size_t> widths(columns_.size(), 0);
    for (std::size_t c = 0; c < columns_.size(); ++c) {
        widths[c] = codePoints(columns_[c].header);
        for (const std::vector<std::string>& row : rows_) {
            widths[c] = std::max(widths[c], codePoints(row[c]));
        }
    }
    const auto line = [&](const auto& cell) {
        std::string text(indent);
        for (std::size_t c = 0; c < columns_.size(); ++c) {
            const std::string& value = cell(c);
            const std::size_t pad = widths[c] - codePoints(value);
            const bool last = c + 1 == columns_.size();
            if (columns_[c].alignRight) {
                text.append(pad, ' ');
                text += value;
            } else {
                text += value;
                if (!last) {
                    text.append(pad, ' ');
                }
            }
            if (!last) {
                text += "  ";
            }
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        out << text << '\n';
    };
    line([&](std::size_t c) -> const std::string& { return columns_[c].header; });
    for (const std::vector<std::string>& row : rows_) {
        line([&](std::size_t c) -> const std::string& { return row[c]; });
    }
}

}  // namespace recovery::report
