#pragma once

// What the MP4 formats and MP4 recovery (P12) build on top of the parser of
// mp4_parser.hpp:
//
//  * which files are audio (M4A) and which video (MP4): one rule for the
//    ftyp brands and the tracks, shared by both carving formats, so every
//    ISO base media file goes to exactly one of them (or to neither: HEIF,
//    AVIF, CR3 and JPEG 2000 images);
//  * where a movie's media data lies according to its sample tables;
//  * moov discovery: a moov box found by searching the content, for files
//    whose top-level boxes do not lead to it;
//  * sample framing: every AVC or HEVC video sample is a sequence of NAL
//    units, each after a length field, that fills the sample exactly
//    (ISO/IEC 14496-15). A sample that other data has replaced, or that is
//    not where the tables say, almost never passes. This reads the samples'
//    NAL unit headers, not the coded video.
//
// Everything read is untrusted, as in the parser: offsets and sums are
// checked, and the work is bounded by the content and ParseLimits.

#include "carving/content_reader.hpp"
#include "formats/mp4_box.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recovery::formats::mp4 {

// ---------------------------------------------------------------------------
// Audio, video or neither
// ---------------------------------------------------------------------------

enum class BrandClass : std::uint8_t {
    // Major brands of audio files: M4A, M4B, M4P, F4A, F4B.
    Audio,
    // Major brands of video files: M4V, M4VH, M4VP, F4V, F4P, and QuickTime's qt.
    Video,
    // Major brands of still images, which are not MP4 or M4A files: HEIF
    // (heic, heix, heim, heis, hevc, hevx, mif1, msf1), AVIF (avif, avis),
    // Canon's CR3 (crx), JPEG 2000 (jp2, mjp2).
    Image,
    // Any other brand (isom, iso2-iso9, mp41, mp42, avc1, 3gp4-3gp9, 3g2a,
    // dash, ...): the tracks decide.
    Generic,
};

[[nodiscard]] BrandClass classifyBrand(FourCc majorBrand) noexcept;

enum class MediaKind : std::uint8_t {
    // An M4A file.
    Audio,
    // An MP4 file (or MOV, M4V, 3GP).
    Video,
    // Neither: an image brand, or a major brand that is not printable.
    Neither,
};

struct Classification {
    MediaKind kind = MediaKind::Neither;
    // Why (for details; never file content).
    std::string reason;
};

// An audio brand makes the file audio and a video brand video, whatever the
// tracks. A generic brand (or no ftyp at all) makes it audio when `movie`
// has a sound track and no video track, and video otherwise, including when
// there is no movie to tell by (moov lost, overwritten or beyond the data).
[[nodiscard]] Classification classify(const std::optional<FileType>& fileType, const Movie* movie);

// ---------------------------------------------------------------------------
// Media data according to the sample tables
// ---------------------------------------------------------------------------

struct MediaExtent {
    // The smallest offset and the largest end of any chunk or fragment run.
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t samples = 0;
    // The sum of the chunks' and runs' sizes.
    std::uint64_t bytes = 0;
};

// Over every track (chunks of valid sample tables and fragment runs); empty
// when no track has a chunk or run of at least one byte.
[[nodiscard]] std::optional<MediaExtent> mediaExtent(const Movie& movie);

// ---------------------------------------------------------------------------
// moov discovery
// ---------------------------------------------------------------------------

struct FoundMovie {
    BoxHeader box;
    Movie movie;
};

// Searches the content in [from, to) for a moov box that lies entirely in
// it and whose movie has at least one track with a valid sample table, and
// returns the first. Every "moov" in the range is tried (a box header that
// fits, then a parse), at most `maxAttempts` of them. Fails with
// InvalidInput for invalid limits, and with the reader's errors.
[[nodiscard]] Result<std::optional<FoundMovie>> findMovie(carving::IContentReader& content, std::uint64_t from,
                                                          std::uint64_t to, const ParseLimits& limits = {},
                                                          std::size_t maxAttempts = 64);

// ---------------------------------------------------------------------------
// Sample framing
// ---------------------------------------------------------------------------

struct TrackFraming {
    // 1-based track number in moov.
    std::uint32_t track = 0;
    // Samples whose NAL units were walked, and those they do not fill exactly.
    std::uint64_t checked = 0;
    std::uint64_t bad = 0;
    // The first bad sample: its index in the track (0-based), its file
    // offset, and what is wrong (never file content).
    std::optional<std::uint64_t> firstBadSample;
    std::uint64_t firstBadOffset = 0;
    std::string firstBadDetail;
};

struct FramingCheck {
    // One entry per video track whose sample descriptions give a NAL unit
    // length size (the others have no framing to check).
    std::vector<TrackFraming> tracks;

    [[nodiscard]] std::uint64_t checked() const noexcept;
    [[nodiscard]] std::uint64_t bad() const noexcept;
    // "sample 12 of track 1 at offset 4096: ..." for the first bad sample.
    [[nodiscard]] std::string describeFirstBad() const;
};

// Walks the NAL units of every sample of the file's AVC and HEVC tracks that
// lies inside the content (chunks and runs whose placement is MediaData, or
// Unchecked and inside the content): each NAL unit is a big-endian length of
// the description's nalLengthSize bytes, then that many bytes whose first
// has the forbidden zero bit clear, and together they fill the sample. Fails
// with the reader's errors.
[[nodiscard]] Result<FramingCheck> checkSampleFraming(carving::IContentReader& content, const Movie& movie);

}  // namespace recovery::formats::mp4
