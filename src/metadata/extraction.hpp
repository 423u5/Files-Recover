#pragma once

// What the metadata extractors of recovery_metadata share (private): the
// state of one extraction, bounded reads of the content, text and date
// decoding, picture probing, and the extractors of each format.

#include "carving/content_reader.hpp"
#include "metadata/media_metadata.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::metadata::detail {

// The state of one extraction: the content, the options, and the result.
struct Extraction {
    carving::IContentReader& content;
    const MetadataOptions& options;
    MediaMetadata& out;
    // A picture inside the file (a thumbnail, cover art), read for its
    // header only: no Exif, no animation walk, no previews.
    bool nested = false;

    // Records a problem of the content (MediaMetadata::issues, capped).
    void issue(std::optional<std::uint64_t> offset, std::string detail);
};

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Up to `length` (at most kMaxReadLength) bytes at `offset`: fewer when the
// content ends first, none when `offset` is at or beyond its end. The span
// stays valid until the next read.
[[nodiscard]] Result<std::span<const std::byte>> readUpTo(carving::IContentReader& content, std::uint64_t offset,
                                                          std::size_t length);

// Bytes [offset, offset + length) of the content, clipped to its end, in
// memory. The caller bounds `length`.
[[nodiscard]] Result<std::vector<std::byte>> readBlock(carving::IContentReader& content, std::uint64_t offset,
                                                       std::uint64_t length);

// The content's bytes at `offset` start with `text`.
[[nodiscard]] bool startsWith(std::span<const std::byte> bytes, std::string_view text) noexcept;

// [offset, offset + length) of another reader as content of its own. The
// other reader must outlive it.
class WindowReader final : public carving::IContentReader {
public:
    // The window must lie inside `content`.
    WindowReader(carving::IContentReader& content, std::uint64_t offset, std::uint64_t length) noexcept
        : content_(content), offset_(offset), length_(length) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return length_; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override;

private:
    carving::IContentReader& content_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

enum class TextEncoding : std::uint8_t {
    Latin1,
    Utf8,
    // UTF-8 when the bytes are valid UTF-8, ISO 8859-1 otherwise (Exif
    // ASCII, RIFF INFO, ID3v1: fields that should be ASCII and are not always).
    Utf8OrLatin1,
    // UTF-16 with a byte order mark (big-endian without one).
    Utf16,
    Utf16Be,
    Utf16Le,
};

// The text in `bytes` up to its first terminator (a zero byte; a zero code
// unit for UTF-16), as UTF-8: invalid sequences become U+FFFD, control
// characters spaces, spaces at both ends are trimmed, and it is cut at a
// character boundary to at most `maxLength` bytes.
[[nodiscard]] std::string decodeText(std::span<const std::byte> bytes, TextEncoding encoding, std::size_t maxLength);

// Where the text that starts at `bytes[0]` ends, its terminator included
// (one zero byte, or a zero code unit at an even position for wide text);
// bytes.size() when it has none.
[[nodiscard]] std::size_t textEnd(std::span<const std::byte> bytes, bool wide) noexcept;

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

// A whole date and time that names an existing day and time (year 1-9999).
[[nodiscard]] bool validDateTime(const MediaDateTime& value) noexcept;

// ISO 8601 as tags write it: "2024", "2024-05", "2024-05-17",
// "2024-05-17T14", "2024-05-17T14:23", "2024-05-17T14:23:05" (a space for
// the "T", fractions of a second ignored), then "Z", "+02:00" or "+0200".
[[nodiscard]] std::optional<MediaDateTime> parseIsoDateTime(std::string_view text);

// Exif: "2024:05:17 14:23:05" (local time, no zone).
[[nodiscard]] std::optional<MediaDateTime> parseExifDateTime(std::string_view text);

// "+02:00", "-05:30", "+0200", "Z": minutes east of UTC.
[[nodiscard]] std::optional<std::int16_t> parseUtcOffset(std::string_view text);

// Seconds since 1970-01-01 UTC as a UTC date (precision Second); none
// outside years 1-9999.
[[nodiscard]] std::optional<MediaDateTime> utcDateTime(std::int64_t unixSeconds);

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

// The carving format of a picture from its first bytes: "jpeg", "png",
// "gif", "bmp" or "webp"; empty when it is none of them.
[[nodiscard]] std::string_view sniffPicture(std::span<const std::byte> head) noexcept;

// The picture at [offset, offset + length) of the content as a preview of
// kind `kind`: its format from its first bytes, its size from its header.
// None (and an issue) when it is not a picture whose header the engine
// reads. `what` names it in issues ("the Exif thumbnail").
[[nodiscard]] Result<std::optional<PreviewSource>> probePicture(Extraction& x, PreviewKind kind,
                                                                std::uint64_t offset, std::uint64_t length,
                                                                std::string_view what);

// ---------------------------------------------------------------------------
// Exif
// ---------------------------------------------------------------------------

struct ExifData {
    std::optional<Orientation> orientation;
    std::optional<MediaDateTime> dateTaken;
    std::string make;
    std::string model;
    // IFD1's JPEG thumbnail, in content offsets.
    std::optional<std::uint64_t> thumbnailOffset;
    std::uint64_t thumbnailLength = 0;
    std::optional<Orientation> thumbnailOrientation;
};

// Reads the Exif block at [offset, offset + length) of the content: a TIFF
// structure, after an "Exif\0\0" prefix when there is one (JPEG APP1 has
// it; PNG eXIf and WebP EXIF should not, and some writers add it anyway).
// Problems are issues. Fails only with the reader's errors.
[[nodiscard]] Result<ExifData> readExif(Extraction& x, std::uint64_t offset, std::uint64_t length);

// Applies Exif to an image, and adds its thumbnail as a preview.
[[nodiscard]] Status applyExif(Extraction& x, const ExifData& exif, ImageMetadata& image);

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

// ID3v2 genre references ("(13)", "13", "(13)Pop", "(RX)") and ID3v1 genre
// numbers as names; other text as it is.
[[nodiscard]] std::string genreName(std::string_view text);
// The name of ID3v1 genre `index` (0-191), empty beyond.
[[nodiscard]] std::string_view id3v1Genre(std::uint32_t index) noexcept;

// "5", "5/12": the track number and the total.
void parseTrackNumber(std::string_view text, MediaTags& tags);

// Sets each field of `into` that is empty from `from`.
void fillTags(MediaTags& into, const MediaTags& from);

struct Id3v2Result {
    // One past the tag (footer included); the offset when there is none.
    std::uint64_t end = 0;
    bool found = false;
};

// Reads the ID3v2 tag at `offset`, if there is one: its text frames into
// `tags` (fields already set are kept) and its pictures as cover art.
[[nodiscard]] Result<Id3v2Result> readId3v2(Extraction& x, std::uint64_t offset, MediaTags& tags);

// The ID3v1 tag in the last 128 bytes of [0, end), if there is one: its
// fields fill those `tags` leaves empty. Returns where the tag starts (`end`
// when there is none).
[[nodiscard]] Result<std::uint64_t> readId3v1(Extraction& x, std::uint64_t end, MediaTags& tags);

// ---------------------------------------------------------------------------
// The formats
// ---------------------------------------------------------------------------

[[nodiscard]] Status extractJpeg(Extraction& x);
[[nodiscard]] Status extractPng(Extraction& x);
[[nodiscard]] Status extractGif(Extraction& x);
[[nodiscard]] Status extractBmp(Extraction& x);
[[nodiscard]] Status extractWebp(Extraction& x);
[[nodiscard]] Status extractMp3(Extraction& x);
[[nodiscard]] Status extractAdts(Extraction& x);
[[nodiscard]] Status extractWav(Extraction& x);
// MP4 and M4A.
[[nodiscard]] Status extractMp4(Extraction& x);

// Duration helpers: `units` of 1/`scale` seconds as microseconds (none when
// the scale is 0 or the result does not fit).
[[nodiscard]] std::optional<MediaDuration> durationOf(std::uint64_t units, std::uint64_t scale) noexcept;
// Bits per second of `bytes` over `duration` (0 for an empty duration).
[[nodiscard]] std::uint64_t bitrateOf(std::uint64_t bytes, MediaDuration duration) noexcept;

}  // namespace recovery::metadata::detail
