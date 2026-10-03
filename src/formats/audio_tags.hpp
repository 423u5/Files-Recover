#pragma once

// The metadata tags that frame-stream audio files carry around their frames
// (private to recovery_formats; MP3 and ADTS AAC use them):
//  * before the first frame: ID3v2 tags (id3.org, versions 2.2 to 2.4);
//  * after the last frame, in any order: an APEv2 tag that has a header, a
//    Lyrics3 tag (v1 or v2), an appended ID3v2 tag, and ID3v1 (last, 128
//    bytes, or 355 with the "TAG+" extension).
// The tags belong to the file, so the walks include them in its length. What
// a tag says is not checked, only its layout.

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"
#include "structure_walk.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace recovery::formats::detail {

inline constexpr std::size_t kId3v2HeaderSize = 10;
inline constexpr std::size_t kId3v1Size = 128;

struct Id3v2Header {
    // 2, 3 or 4.
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    // Bytes after the header: extended header, frames and padding.
    std::uint32_t size = 0;
    // A 10-byte footer follows (version 2.4 only).
    bool footer = false;

    // Header, body and footer.
    [[nodiscard]] std::uint64_t totalSize() const noexcept {
        return kId3v2HeaderSize + std::uint64_t{size} + (footer ? kId3v2HeaderSize : 0);
    }
};

// The ID3v2 header at the start of `bytes`, or nothing when there is none:
// "ID3", a major version of 2 to 4, a revision below 0xFF, no flags the
// version does not define, and a 28-bit syncsafe size.
[[nodiscard]] std::optional<Id3v2Header> parseId3v2Header(std::span<const std::byte> bytes) noexcept;

// Where a run of tags ends.
struct TagRun {
    // One past the last byte of the tags (the start when there are none).
    std::uint64_t end = 0;
    // The data ends inside a tag.
    bool truncated = false;
    // How many tags there were.
    std::uint32_t count = 0;
};

// Skips the ID3v2 tags that start at `offset`, one after another, and the
// zero bytes that may follow them (at most kMaxZeroPadding). At Depth::Full
// the frames inside each tag are walked too, and damage is noted in `walk`.
[[nodiscard]] Result<TagRun> skipLeadingTags(carving::IContentReader& content, std::uint64_t offset, Depth depth,
                                             Walk& walk);

// Skips the tags that start at `offset`, where a stream's last frame ends.
// ID3v1 is the last of them; anything that is not a tag ends the run.
[[nodiscard]] Result<TagRun> skipTrailingTags(carving::IContentReader& content, std::uint64_t offset, Depth depth,
                                              Walk& walk);

// Zero bytes after the leading tags that still count as padding.
inline constexpr std::uint64_t kMaxZeroPadding = 64 * 1024;

}  // namespace recovery::formats::detail
