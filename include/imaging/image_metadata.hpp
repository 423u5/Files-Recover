#pragma once

#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::imaging {

enum class ImageState : std::uint8_t {
    InProgress,
    Completed,
    Cancelled,
    Failed,
};

[[nodiscard]] std::string_view toString(ImageState state) noexcept;
[[nodiscard]] std::optional<ImageState> parseImageState(std::string_view text) noexcept;

// Sidecar describing a disk image: where it came from, how far imaging got,
// and which regions of the source could not be read (and are zero-filled in
// the image). Stored next to the image as "<image>.imgmeta" in a versioned
// line-based key=value format (see docs/architecture/imaging.md).
struct ImageMetadata {
    static constexpr std::uint32_t kFormatVersion = 1;

    std::uint32_t formatVersion = kFormatVersion;
    std::string engineVersion;
    ImageState state = ImageState::InProgress;
    storage::SourceType sourceType = storage::SourceType::Synthetic;
    std::string sourcePath;  // UTF-8
    std::string sourceVendor;
    std::string sourceProduct;
    std::uint64_t sourceSize = 0;
    std::uint32_t sectorSize = 0;
    std::uint64_t blockSize = 0;
    // Bytes [0, bytesCompleted) of the image are final.
    std::uint64_t bytesCompleted = 0;
    std::string startedUtc;
    std::string updatedUtc;
    std::vector<storage::BadRegion> badRegions;
};

// Limits applied when parsing (metadata files are untrusted input).
inline constexpr std::size_t kMaxMetadataBytes = 64 * kMiB;
inline constexpr std::size_t kMaxMetadataLineLength = 4096;
inline constexpr std::size_t kMaxMetadataBadRegions = 1'000'000;

[[nodiscard]] std::filesystem::path metadataPathFor(const std::filesystem::path& imagePath);

[[nodiscard]] Result<std::string> serializeImageMetadata(const ImageMetadata& metadata);
[[nodiscard]] Result<ImageMetadata> parseImageMetadata(std::string_view text);

// Reads and validates a metadata file.
[[nodiscard]] Result<ImageMetadata> readImageMetadata(const std::filesystem::path& path);
// Writes to "<path>.tmp", flushes, then atomically replaces `path`, so a
// crash leaves either the old or the new metadata, never a torn file.
[[nodiscard]] Status writeImageMetadata(const std::filesystem::path& path, const ImageMetadata& metadata);

}  // namespace recovery::imaging
