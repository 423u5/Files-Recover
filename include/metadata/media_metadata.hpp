#pragma once

// Media metadata (P17): what a recovered file's own content says about it,
// for a user interface that lists, sorts and previews what a scan found:
//
//   images  size, colour model, bit depth, alpha, frames and loop count,
//           interlacing; Exif orientation, date taken, camera make and model
//   audio   codec and profile, sample rate, channels, bits per sample,
//           bitrate, duration; title, artist, album, date, track, genre
//           (ID3v1, ID3v2.2-2.4, RIFF INFO, MP4 ilst and QuickTime text)
//   video   brands, duration, creation and modification time, and per track
//           the codec, profile and level, size, frame rate, rotation,
//           channels, language; the same tags as audio
//
// and where its previews are: the content itself (what a platform decoder
// shows or plays), an Exif thumbnail, cover art. A preview is described
// (format, media type, place in the content, size, orientation) and its
// bytes are handed out on request (readPreview, openPreview); nothing here
// decodes or renders a picture, and nothing depends on a user interface
// library.
//
// Metadata is read on demand, from one candidate's content at a time: a
// bounded number of headers, tags and frame headers, never the whole file
// (MetadataOptions bounds the work and the memory). The content is untrusted
// like everything the engine reads: every field is checked against the bytes
// that hold it, and problems are reported as issues, never as errors. Tag
// and Exif values are file content: they are returned to the caller and
// never logged.

#include "carving/content_reader.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::metadata {

enum class MediaKind : std::uint8_t {
    // No format is known, or the format's files are not media (an MP4 whose
    // brand is an image format: HEIF, AVIF).
    Unknown,
    Image,
    Audio,
    Video,
};

[[nodiscard]] std::string_view toString(MediaKind kind) noexcept;

// The kind of a carving format's files: "jpeg", "png", "gif", "bmp" and
// "webp" are images, "mp3", "wav", "aac" and "m4a" audio, "mp4" video.
// Unknown for any other id (and the empty one).
[[nodiscard]] MediaKind kindOfFormat(std::string_view formatId) noexcept;

// The media type (MIME) of a carving format's files ("image/jpeg",
// "audio/mpeg", "video/mp4", ...); empty for an unknown id. MP4 files get a
// more precise one from their brands once their content is read
// (MediaMetadata::mediaType: "video/quicktime", "video/3gpp").
[[nodiscard]] std::string_view mediaTypeOfFormat(std::string_view formatId) noexcept;

using MediaDuration = std::chrono::microseconds;

// A date and time as a file records it: only the parts it records
// (precision), in the zone it says (or none).
struct MediaDateTime {
    enum class Precision : std::uint8_t { Year, Month, Day, Hour, Minute, Second };
    enum class Zone : std::uint8_t {
        // No zone recorded: a local time of an unknown place (Exif without an
        // offset, ID3, RIFF INFO).
        Unknown,
        // UTC (MP4 header times; text that ends in "Z").
        Utc,
        // A fixed offset from UTC (offsetMinutes).
        Offset,
    };

    // 1 to 9999.
    std::int32_t year = 1;
    // Parts beyond the precision are their first value (1 for month and day,
    // 0 for the time).
    std::uint8_t month = 1;
    std::uint8_t day = 1;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    Precision precision = Precision::Year;
    Zone zone = Zone::Unknown;
    // Zone::Offset: minutes east of UTC (-1439 to 1439).
    std::int16_t offsetMinutes = 0;

    // ISO 8601 to the precision: "2024", "2024-05", "2024-05-17",
    // "2024-05-17T14", "2024-05-17T14:23", "2024-05-17T14:23:05", with "Z"
    // or "+02:00" after a time whose zone is known.
    [[nodiscard]] std::string iso8601() const;
    // The instant, when the zone is known; the parts beyond the precision
    // count as their first value.
    [[nodiscard]] std::optional<std::chrono::sys_seconds> utc() const;

    friend bool operator==(const MediaDateTime&, const MediaDateTime&) = default;
};

// Exif orientation (TIFF tag 0x0112): how the stored pixels are turned to
// show the picture. The values are Exif's.
enum class Orientation : std::uint8_t {
    Normal = 1,
    Mirror = 2,
    Rotate180 = 3,
    MirrorRotate180 = 4,
    MirrorRotate270 = 5,
    Rotate90 = 6,
    MirrorRotate90 = 7,
    Rotate270 = 8,
};

[[nodiscard]] std::string_view toString(Orientation orientation) noexcept;
// Degrees the stored picture is turned clockwise to show it (0, 90, 180 or 270).
[[nodiscard]] std::uint16_t rotationOf(Orientation orientation) noexcept;
// The stored picture is mirrored left to right before it is turned.
[[nodiscard]] bool mirrored(Orientation orientation) noexcept;
// The orientation that turns a picture `degrees` clockwise (0, 90, 180, 270), unmirrored.
[[nodiscard]] std::optional<Orientation> orientationForRotation(std::uint32_t degrees) noexcept;

enum class ColorModel : std::uint8_t {
    Unknown,
    Grayscale,
    Rgb,
    // Palette indexes (GIF, PNG colour type 3, BMP of 8 bits or fewer).
    Indexed,
    // JPEG and lossy WebP.
    YCbCr,
    Cmyk,
    Ycck,
};

[[nodiscard]] std::string_view toString(ColorModel model) noexcept;

struct ImageMetadata {
    // Pixels (WebP and GIF: the canvas).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ColorModel color = ColorModel::Unknown;
    // Samples per pixel, alpha included (indexed: 1, the index).
    std::uint8_t channels = 0;
    // Bits of one sample (indexed: of an index); 0 when the channels differ
    // (BMP bit fields of 5, 6 and 5 bits).
    std::uint8_t bitsPerChannel = 0;
    // Bits per pixel, as viewers show a "bit depth": every channel together.
    std::uint16_t bitsPerPixel = 0;
    // An alpha channel or a transparent colour.
    bool alpha = false;
    // Animations: their frames (GIF images, APNG frames, WebP ANMF frames);
    // 1 for a still image.
    std::uint32_t frames = 1;
    // Animations: how many times they play (0: forever), when recorded.
    std::optional<std::uint32_t> loopCount;
    // Progressive JPEG, interlaced PNG and GIF.
    bool progressive = false;
    // Exif.
    std::optional<Orientation> orientation;
    // Exif DateTimeOriginal, else DateTimeDigitized, else DateTime; with the
    // matching OffsetTime* tag as its zone when there is one.
    std::optional<MediaDateTime> dateTaken;
    std::string cameraMake;
    std::string cameraModel;
};

struct AudioStreamMetadata {
    // How the samples are coded: "mp3", "aac", "alac", "pcm" (integer),
    // "pcm_float", "alaw", "mulaw", "adpcm_ima", "adpcm_ms", "ac3", "eac3",
    // "opus", "flac", "amr_nb", "amr_wb"; a WAV format tag the engine does
    // not name is "wav_0x<tag>", an MP4 sample entry it does not name its
    // four characters.
    std::string codec;
    // "MPEG-1 Layer III", "AAC LC", "HE-AAC", ...; empty when not known.
    std::string profile;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    // The bits of one sample when the format records them (WAV, PCM in
    // MP4); 0 otherwise.
    std::uint32_t bitsPerSample = 0;
    // Bits per second of the coded audio (the frames, the data chunk, the
    // track's samples); 0 when not known.
    std::uint64_t bitrate = 0;
    // MP3: the bitrate changes from frame to frame.
    bool variableBitrate = false;
};

// Frames per second as a reduced fraction: 30000/1001, 25/1.
struct FrameRate {
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 1;

    [[nodiscard]] double value() const noexcept {
        return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    friend bool operator==(const FrameRate&, const FrameRate&) = default;
};

struct VideoStreamMetadata {
    // "h264", "hevc", "mpeg4", "h263", "mjpeg", "av1", "vp9"; an MP4 sample
    // entry the engine does not name is its four characters.
    std::string codec;
    // AVC and HEVC: "High", "Main 10", ... (empty when not known), and the
    // level: "4.0", "5.1" ("1b"; HEVC ", High tier").
    std::string profile;
    std::string level;
    // The coded size (the sample description).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The size to show it at (the track header, before rotation).
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    // Average: the samples over the track's duration.
    std::optional<FrameRate> frameRate;
    // Degrees the decoded picture is turned clockwise to show it (0, 90,
    // 180, 270), from the track header's matrix; 0 when the matrix is not a
    // turn by a multiple of 90 degrees (an issue says so).
    std::uint16_t rotation = 0;
    // Bits per second of the track's samples; 0 when not known.
    std::uint64_t bitrate = 0;
};

struct TrackMetadata {
    // The track header's id.
    std::uint32_t id = 0;
    // Video, Audio, or Unknown for other handlers (text, hints, metadata).
    MediaKind kind = MediaKind::Unknown;
    // The handler type ("vide", "soun", "text", ...) and the first sample
    // description's code ("avc1", "mp4a", ...): four characters, or
    // "0x<hex>" when they are not printable.
    std::string handler;
    std::string sampleEntry;
    // "eng", "und", ...; empty for a Macintosh language code.
    std::string language;
    bool enabled = true;
    std::optional<MediaDuration> duration;
    // The samples of the sample tables and of the movie fragments, and their bytes.
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::optional<VideoStreamMetadata> video;
    std::optional<AudioStreamMetadata> audio;
};

struct MovieMetadata {
    // ftyp: "isom", "M4A ", ...; empty without an ftyp box.
    std::string majorBrand;
    std::uint32_t minorVersion = 0;
    std::vector<std::string> compatibleBrands;
    // The movie header's times (UTC); none when they are 0 (not set).
    std::optional<MediaDateTime> created;
    std::optional<MediaDateTime> modified;
    // Samples in movie fragments (moof).
    bool fragmented = false;
    std::vector<TrackMetadata> tracks;
};

// Common tags: ID3v2 frames (ID3v1 fills what they leave empty), RIFF INFO
// chunks, MP4 ilst items and QuickTime text atoms. Text is UTF-8, without
// control characters, at most MetadataOptions::maxTextLength bytes.
struct MediaTags {
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    // The recording or release date: ID3v2.4 TDRC, ID3v2.3 TYER (with TDAT
    // and TIME), ID3v1's year, ICRD, MP4 "(c)day".
    std::optional<MediaDateTime> date;
    std::optional<std::uint32_t> track;
    std::optional<std::uint32_t> trackTotal;

    [[nodiscard]] bool empty() const noexcept;
};

enum class PreviewKind : std::uint8_t {
    // The file itself: an image to show, audio or video to play.
    Content,
    // A smaller picture of the image stored with it (an Exif thumbnail).
    Thumbnail,
    // A picture stored with audio or video (ID3 APIC, MP4 covr).
    CoverArt,
};

[[nodiscard]] std::string_view toString(PreviewKind kind) noexcept;

// A preview: bytes of the content that a platform decoder can show or play
// as they are. A picture's format is taken from its own first bytes, never
// from what a tag claims it is.
struct PreviewSource {
    PreviewKind kind = PreviewKind::Content;
    // The carving format of the bytes ("jpeg", "png", ...; for Content the
    // file's own) and their media type.
    std::string formatId;
    std::string mediaType;
    // Where the bytes are, in the content.
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    // Pictures (and video), from their own header; 0 when not known.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // How to turn it to show it: Exif (a thumbnail shares its image's), or
    // a video track's rotation.
    std::optional<Orientation> orientation;
    // CoverArt from ID3: the picture type (3: front cover).
    std::optional<std::uint8_t> pictureType;
};

struct MetadataIssue {
    // Content offset of the structure at fault, when there is one.
    std::optional<std::uint64_t> offset;
    // What is wrong; never file content.
    std::string detail;
};

struct MediaMetadata {
    static constexpr std::size_t kMaxIssues = 32;

    MediaKind kind = MediaKind::Unknown;
    // The format the content was read as (empty: none), and its media type.
    std::string formatId;
    std::string mediaType;
    // How long it plays (audio, video, animations).
    std::optional<MediaDuration> duration;
    // The duration is an estimate: frames counted in part of the file and
    // the rest extrapolated (MetadataOptions::maxScanBytes), or the data
    // size over the bitrate.
    bool durationEstimated = false;
    // Bits per second of the whole content (its size over its duration).
    std::optional<std::uint64_t> bitrate;
    std::optional<ImageMetadata> image;
    // Audio files: their stream. MP4: the first audio track's (enabled
    // tracks first).
    std::optional<AudioStreamMetadata> audio;
    // MP4: the first video track's (enabled tracks first).
    std::optional<VideoStreamMetadata> video;
    // MP4 and M4A: the movie and every track.
    std::optional<MovieMetadata> movie;
    MediaTags tags;
    // Thumbnails and cover art first, in content order, then the content.
    std::vector<PreviewSource> previews;
    // Problems of the content found while reading it: the first kMaxIssues,
    // and how many there were.
    std::vector<MetadataIssue> issues;
    std::uint64_t issueCount = 0;
};

// Bounds on the work and memory of one extraction.
struct MetadataOptions {
    // Bytes of a frame stream walked to count its frames (MP3 without an
    // info tag, ADTS) and of an animation walked to count its frames (GIF,
    // WebP); a longer file's duration is extrapolated (durationEstimated),
    // and its frames are those counted (an issue says so).
    std::uint64_t maxScanBytes = 64 * kMiB;
    // The largest tag or metadata block read into memory as a whole: an
    // unsynchronised ID3v2 tag, a RIFF INFO list, an Exif block, MP4 user
    // data. Larger ones are skipped (an issue says so). Pictures inside tags
    // are never read, only located.
    std::uint64_t maxTagBytes = 16 * kMiB;
    // The longest text kept, in bytes of UTF-8 (longer text is cut at a
    // character boundary).
    std::size_t maxTextLength = 1024;
    // Parse limits for MP4 and M4A files.
    formats::mp4::ParseLimits mp4;
};

// InvalidInput when a limit is 0.
[[nodiscard]] Status validate(const MetadataOptions& options);

// The metadata of the file whose bytes are all of `content`, read as the
// carving format `formatId` (an EvaluatedCandidate's formatId; empty or
// unknown: kind Unknown and nothing else). Problems of the content are
// issues. Fails only with the reader's errors (Cancelled, source failures)
// and with InvalidInput for invalid options.
[[nodiscard]] Result<MediaMetadata> extractMetadata(carving::IContentReader& content, std::string_view formatId,
                                                    const MetadataOptions& options = {});

// The metadata of a candidate's content, read from `source` (the scan's
// source, open) through a CandidateContentReader: the bytes recovery would
// write. Fails also with InvalidInput for a malformed candidate.
[[nodiscard]] Result<MediaMetadata> readMediaMetadata(storage::IStorageSource& source,
                                                      const evaluation::EvaluatedCandidate& candidate,
                                                      const MetadataOptions& options = {},
                                                      const carving::SourceReadOptions& readOptions = {});

// The bytes of a preview, read from the content it was found in. Fails with
// InvalidInput when the preview does not lie inside the content or is longer
// than `maxBytes` (use openPreview for large ones), and with the reader's
// errors.
[[nodiscard]] Result<std::vector<std::byte>> readPreview(carving::IContentReader& content,
                                                         const PreviewSource& preview,
                                                         std::uint64_t maxBytes = 64 * kMiB);

// The bytes of a preview as content of their own, offsets [0, length): a
// view of `content`, which must outlive it (one owner for both). Fails with
// InvalidInput when the preview does not lie inside the content.
[[nodiscard]] Result<std::unique_ptr<carving::IContentReader>> openPreview(carving::IContentReader& content,
                                                                           const PreviewSource& preview);

}  // namespace recovery::metadata
