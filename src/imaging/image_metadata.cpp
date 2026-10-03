#include "imaging/image_metadata.hpp"

#include "recovery/checked_math.hpp"
#include "storage/destination_file.hpp"

#include <charconv>
#include <concepts>
#include <format>
#include <fstream>
#include <set>

namespace recovery::imaging {

namespace {

constexpr std::string_view kHeader = "# RecoveryEngine disk image metadata. Generated file; do not edit.";

template <std::unsigned_integral T>
std::optional<T> parseUnsigned(std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

Error formatError(std::size_t line, std::string message) {
    return makeError(ErrorCode::InvalidFormat, "image metadata line " + std::to_string(line) + ": " + message);
}

bool hasLineBreakOrNul(std::string_view value) {
    return value.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
}

Result<storage::BadRegion> parseBadRegion(std::string_view value) {
    const std::size_t first = value.find(',');
    const std::size_t second = first == std::string_view::npos ? first : value.find(',', first + 1);
    if (second == std::string_view::npos) {
        return makeError(ErrorCode::InvalidFormat, "bad_region must be offset,length,code");
    }
    const auto offset = parseUnsigned<std::uint64_t>(value.substr(0, first));
    const auto length = parseUnsigned<std::uint64_t>(value.substr(first + 1, second - first - 1));
    const auto code = parseUnsigned<std::uint32_t>(value.substr(second + 1));
    if (!offset || !length || !code) {
        return makeError(ErrorCode::InvalidFormat, "bad_region contains an invalid number");
    }
    return storage::BadRegion{*offset, *length, *code};
}

}  // namespace

std::string_view toString(ImageState state) noexcept {
    switch (state) {
    case ImageState::InProgress:
        return "in_progress";
    case ImageState::Completed:
        return "completed";
    case ImageState::Cancelled:
        return "cancelled";
    case ImageState::Failed:
        return "failed";
    }
    return "unknown";
}

std::optional<ImageState> parseImageState(std::string_view text) noexcept {
    for (const ImageState state :
         {ImageState::InProgress, ImageState::Completed, ImageState::Cancelled, ImageState::Failed}) {
        if (toString(state) == text) {
            return state;
        }
    }
    return std::nullopt;
}

std::filesystem::path metadataPathFor(const std::filesystem::path& imagePath) {
    std::filesystem::path path = imagePath;
    path += ".imgmeta";
    return path;
}

Result<std::string> serializeImageMetadata(const ImageMetadata& m) {
    for (const std::string* value :
         {&m.engineVersion, &m.sourcePath, &m.sourceVendor, &m.sourceProduct, &m.startedUtc, &m.updatedUtc}) {
        if (hasLineBreakOrNul(*value)) {
            return makeError(ErrorCode::InvalidInput, "metadata values must not contain line breaks");
        }
    }

    std::string out;
    out.reserve(512 + m.badRegions.size() * 48);
    const auto put = [&out](std::string_view key, std::string_view value) {
        out += key;
        out += '=';
        out += value;
        out += '\n';
    };
    out += kHeader;
    out += '\n';
    put("format_version", std::to_string(m.formatVersion));
    put("engine_version", m.engineVersion);
    put("state", toString(m.state));
    put("source_type", storage::toString(m.sourceType));
    put("source_path", m.sourcePath);
    put("source_vendor", m.sourceVendor);
    put("source_product", m.sourceProduct);
    put("source_size", std::to_string(m.sourceSize));
    put("sector_size", std::to_string(m.sectorSize));
    put("block_size", std::to_string(m.blockSize));
    put("bytes_completed", std::to_string(m.bytesCompleted));
    put("started_utc", m.startedUtc);
    put("updated_utc", m.updatedUtc);
    for (const storage::BadRegion& region : m.badRegions) {
        out += std::format("bad_region={},{},{}\n", region.offset, region.length, region.errorCode);
    }
    return out;
}

Result<ImageMetadata> parseImageMetadata(std::string_view text) {
    if (text.size() > kMaxMetadataBytes) {
        return makeError(ErrorCode::InvalidFormat, "image metadata exceeds the size limit");
    }

    ImageMetadata m;
    std::set<std::string, std::less<>> seen;
    storage::BadRegionMap regions;
    std::size_t regionLines = 0;
    std::size_t lineNumber = 0;
    std::size_t position = 0;

    while (position < text.size()) {
        const std::size_t newline = text.find('\n', position);
        std::string_view line = text.substr(position, newline == std::string_view::npos ? text.size() - position
                                                                                         : newline - position);
        position = newline == std::string_view::npos ? text.size() : newline + 1;
        ++lineNumber;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.size() > kMaxMetadataLineLength) {
            return formatError(lineNumber, "line too long");
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            return formatError(lineNumber, "expected key=value");
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);

        if (key == "bad_region") {
            if (++regionLines > kMaxMetadataBadRegions) {
                return formatError(lineNumber, "too many bad regions");
            }
            const Result<storage::BadRegion> region = parseBadRegion(value);
            if (!region.ok()) {
                return formatError(lineNumber, region.error().message);
            }
            if (const Status added = regions.add(region.value()); !added.ok()) {
                return formatError(lineNumber, added.error().message);
            }
            continue;
        }
        if (!seen.emplace(key).second) {
            return formatError(lineNumber, "duplicate key '" + std::string(key) + "'");
        }

        const auto requireNumber = [&](auto& target) -> Status {
            using Target = std::remove_reference_t<decltype(target)>;
            const std::optional<Target> number = parseUnsigned<Target>(value);
            if (!number.has_value()) {
                return formatError(lineNumber, "invalid number for '" + std::string(key) + "'");
            }
            target = *number;
            return success();
        };

        Status parsed = success();
        if (key == "format_version") {
            parsed = requireNumber(m.formatVersion);
        } else if (key == "engine_version") {
            m.engineVersion = value;
        } else if (key == "state") {
            const auto state = parseImageState(value);
            if (!state) {
                return formatError(lineNumber, "unknown state");
            }
            m.state = *state;
        } else if (key == "source_type") {
            const auto type = storage::parseSourceType(value);
            if (!type) {
                return formatError(lineNumber, "unknown source type");
            }
            m.sourceType = *type;
        } else if (key == "source_path") {
            m.sourcePath = value;
        } else if (key == "source_vendor") {
            m.sourceVendor = value;
        } else if (key == "source_product") {
            m.sourceProduct = value;
        } else if (key == "source_size") {
            parsed = requireNumber(m.sourceSize);
        } else if (key == "sector_size") {
            parsed = requireNumber(m.sectorSize);
        } else if (key == "block_size") {
            parsed = requireNumber(m.blockSize);
        } else if (key == "bytes_completed") {
            parsed = requireNumber(m.bytesCompleted);
        } else if (key == "started_utc") {
            m.startedUtc = value;
        } else if (key == "updated_utc") {
            m.updatedUtc = value;
        }
        // Unknown keys are ignored so newer engines can add optional fields.
        if (!parsed.ok()) {
            return parsed.error();
        }
    }

    for (const std::string_view required :
         {"format_version", "state", "source_type", "source_path", "source_size", "sector_size", "bytes_completed"}) {
        if (!seen.contains(required)) {
            return makeError(ErrorCode::InvalidFormat, "image metadata is missing '" + std::string(required) + "'");
        }
    }
    if (m.formatVersion != ImageMetadata::kFormatVersion) {
        return makeError(ErrorCode::InvalidFormat,
                         "unsupported image metadata version " + std::to_string(m.formatVersion));
    }
    if (!storage::isValidSectorSize(m.sectorSize)) {
        return makeError(ErrorCode::InvalidFormat, "image metadata has an invalid sector size");
    }
    if (m.sourceSize > storage::kMaxAddressableOffset || m.bytesCompleted > m.sourceSize) {
        return makeError(ErrorCode::InvalidFormat, "image metadata progress exceeds the source size");
    }
    m.badRegions = regions.regions();
    for (const storage::BadRegion& region : m.badRegions) {
        if (region.end() > m.sourceSize) {
            return makeError(ErrorCode::InvalidFormat, "image metadata bad region lies outside the source");
        }
    }
    return m;
}

Result<ImageMetadata> readImageMetadata(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return makeError(ErrorCode::InvalidInput, "cannot read image metadata: " + ec.message(),
                         static_cast<std::uint32_t>(ec.value()));
    }
    if (size > kMaxMetadataBytes) {
        return makeError(ErrorCode::InvalidFormat, "image metadata exceeds the size limit");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::IoError, "cannot open image metadata");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) {
        return makeError(ErrorCode::IoError, "image metadata changed while being read");
    }
    return parseImageMetadata(text);
}

Status writeImageMetadata(const std::filesystem::path& path, const ImageMetadata& metadata) {
    const Result<std::string> text = serializeImageMetadata(metadata);
    if (!text.ok()) {
        return text.error();
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);  // stale leftover of an interrupted write

    Result<storage::DestinationFile> file =
        storage::DestinationFile::open(temporary, storage::DestinationFile::OpenMode::CreateNew);
    if (!file.ok()) {
        return file.error();
    }
    const auto bytes = std::as_bytes(std::span(text.value()));
    if (Status written = file->writeAt(0, bytes); !written.ok()) {
        return written;
    }
    if (Status flushed = file->flush(); !flushed.ok()) {
        return flushed;
    }
    file->close();
    return storage::replaceFile(temporary, path);
}

}  // namespace recovery::imaging
