#pragma once

// Shared by the metadata tests (P17): extraction from bytes in memory, and
// files put together from parts the builders do not write (ID3 tags of any
// frames and encodings, MP4 user data, patched MP4 headers).

#include "metadata/media_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::metadata::test {

using Bytes = std::vector<std::byte>;

// extractMetadata over the bytes (ADD_FAILURE when it fails).
[[nodiscard]] MediaMetadata extract(std::span<const std::byte> bytes, std::string_view formatId,
                                    const MetadataOptions& options = {});

// The issues' details, one per line (for failure messages).
[[nodiscard]] std::string issuesText(const MediaMetadata& metadata);

[[nodiscard]] Bytes text(std::string_view value);
// `value` as UTF-16 with a byte order mark (little-endian) or big-endian
// without one; each character below U+10000.
[[nodiscard]] Bytes utf16(std::u16string_view value, bool bigEndian);

// An ID3v2 frame: id and body (flags 0).
struct Id3Frame {
    std::string id;
    Bytes body;
};

// An ID3v2 tag of `version` (2, 3 or 4) holding the frames; `unsync`
// unsynchronises the whole tag (its flag set, every 0xFF followed by a 0x00).
[[nodiscard]] Bytes id3Tag(std::uint8_t version, const std::vector<Id3Frame>& frames, std::size_t padding = 16,
                           bool unsync = false);
// A text frame body: the encoding byte, then the text.
[[nodiscard]] Bytes id3Text(std::uint8_t encoding, std::span<const std::byte> value);

// An MP4 box, and a full box.
[[nodiscard]] Bytes box(std::string_view type, std::span<const std::byte> payload);
[[nodiscard]] Bytes fullBox(std::string_view type, std::uint8_t version, std::uint32_t flags,
                            std::span<const std::byte> payload);
// An ilst item holding one data box of the given type indicator.
[[nodiscard]] Bytes ilstItem(std::string_view type, std::uint32_t dataType, std::span<const std::byte> value);
// udta / meta (a full box, with an mdir handler) / ilst around the items.
[[nodiscard]] Bytes userData(const std::vector<Bytes>& items);

// The file with `child` appended as the last child of its moov box, which
// must be the file's last top-level box (as the builders write it).
[[nodiscard]] Bytes withMoovChild(Bytes file, std::span<const std::byte> child);

// Offset of the first occurrence of `needle` in `haystack` (npos: none).
[[nodiscard]] std::size_t find(std::span<const std::byte> haystack, std::span<const std::byte> needle);

}  // namespace recovery::metadata::test
