#pragma once

// The dedicated ISO base media file format parser (P11): MP4, MOV, M4V, M4A
// and 3GP files. It reads the boxes of mp4_box.hpp into a model of the movie:
//
//   ftyp                   brands
//   moov / mvhd            the movie's time scale and duration
//   trak / tkhd            track id, duration, display size
//        / mdia / mdhd     the media's time scale, duration and language
//               / hdlr     the handler: video ('vide'), audio ('soun') or other
//               / minf / stbl / stsd        sample descriptions (codec, size, channels, rate)
//                             / stts        decoding times
//                             / stsc        samples per chunk
//                             / stsz, stz2  sample sizes
//                             / stco, co64  chunk offsets
//        / mvex / trex     the tracks' defaults for movie fragments
//   moof / mfhd            a movie fragment's sequence number (parseFile())
//        / traf / tfhd     a track fragment: its track, base data offset, defaults
//               / trun     a run of samples: where they are and how long
//   mdat, free, skip, wide, ... at the top level
//
// From stsc, stsz and stco/co64 it derives each track's chunks: where they
// are and how long; each run of a movie fragment is one more such piece of
// the track. parseFile() then checks every chunk and run against the media
// data boxes of the file.
//
// It assumes nothing about the order of the boxes (moov before or after
// mdat, children in any order), about the media data (any number of mdat
// boxes, chunks anywhere in them, with gaps), about the file being in one
// piece (it reads only through IContentReader, so the content can be
// assembled from fragments, and parseMovie() works on a moov found on its
// own), or about what a later ftyp means (it is reported, not taken as the
// end of the file).
//
// Everything read is untrusted. Every box is checked against its parent,
// every count against the bytes that hold it before anything is allocated,
// every offset and sum for overflow, and the work of one parse is bounded by
// ParseLimits, which also bounds its memory. A malformed structure is
// reported as issues (the parse still returns what it could read); a Result
// fails only for invalid arguments or a failing read (the reader's error,
// such as Cancelled). Edit lists and codec configurations are not parsed,
// apart from the NAL unit length size of AVC and HEVC (docs/formats/mp4.md).

#include "carving/content_reader.hpp"
#include "formats/mp4_box.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recovery::formats::mp4 {

// ---------------------------------------------------------------------------
// Issues and limits
// ---------------------------------------------------------------------------

enum class IssueKind : std::uint8_t {
    // The structure breaks the format's rules, or its parts disagree.
    Malformed,
    // Within the rules, but larger than ParseLimits allows to be parsed.
    LimitExceeded,
};

struct Issue {
    IssueKind kind = IssueKind::Malformed;
    // Content offset of the box at fault.
    std::uint64_t offset = 0;
    // Where it is in the box tree: "moov/trak[2]/mdia/minf/stbl/stsc" (tracks count from 1).
    std::string path;
    // What is wrong (never file content).
    std::string detail;

    // "moov/trak[2]/mdia/minf/stbl/stsc at 1234: <detail>"
    [[nodiscard]] std::string describe() const;
};

// The issues of a parse. Only the first kMaxRecorded are kept; all are counted.
class IssueList {
public:
    static constexpr std::size_t kMaxRecorded = 32;

    void add(IssueKind kind, std::uint64_t offset, std::string path, std::string detail);
    // Adds the other list's recorded issues and counts its unrecorded ones.
    void append(const IssueList& other);

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] const std::vector<Issue>& recorded() const noexcept { return recorded_; }
    [[nodiscard]] bool contains(IssueKind kind) const noexcept;

private:
    std::vector<Issue> recorded_;
    std::uint64_t count_ = 0;
};

// Bounds on the work and memory of one parse. Reaching one is an issue of
// kind LimitExceeded; what lies beyond it is not parsed.
struct ParseLimits {
    // Box headers read, at every level.
    std::uint64_t maxBoxes = 1'000'000;
    // Tracks in a movie.
    std::uint32_t maxTracks = 256;
    // Sample descriptions (stsd entries) of a track.
    std::uint32_t maxSampleDescriptions = 64;
    // Sample table entries held in memory for all tracks together: the
    // entries of stts, stsc, stsz/stz2 and stco/co64, and the chunks derived
    // from them. The default takes a movie of several hours; at most 32
    // bytes of memory per entry, 128 MiB.
    std::uint64_t maxTableEntries = std::uint64_t{1} << 22;
};

// InvalidInput when a limit is 0.
[[nodiscard]] Status validate(const ParseLimits& limits);

// ---------------------------------------------------------------------------
// The movie
// ---------------------------------------------------------------------------

struct FileType {
    FourCc majorBrand;
    std::uint32_t minorVersion = 0;
    std::vector<FourCc> compatibleBrands;
};

struct MovieHeader {
    std::uint8_t version = 0;
    std::uint32_t timescale = 0;
    std::uint64_t duration = 0;
    std::uint32_t nextTrackId = 0;
};

struct TrackHeader {
    std::uint8_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t trackId = 0;
    std::uint64_t duration = 0;
    // Display size, 16.16 fixed point.
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool enabled() const noexcept { return (flags & 1U) != 0; }
};

struct MediaHeader {
    std::uint8_t version = 0;
    std::uint32_t timescale = 0;
    std::uint64_t duration = 0;
    // Packed ISO 639-2/T code (or a Macintosh language code below 0x400).
    std::uint16_t language = 0;

    // "eng", "und", ...; empty for a Macintosh code or an invalid value.
    [[nodiscard]] std::string isoLanguage() const;
};

enum class TrackKind : std::uint8_t {
    // Handler 'vide'.
    Video,
    // Handler 'soun'.
    Audio,
    // Any other handler (text, subtitles, hints, metadata, ...), or none.
    Other,
};

// An entry of stsd: which codec a track's samples use.
struct SampleDescription {
    // 'avc1', 'hvc1', 'mp4v', 'mp4a', 'alac', 'samr', ...
    FourCc format;
    // Content offset and size of the entry (a box).
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint16_t dataReferenceIndex = 0;
    // Video tracks (VisualSampleEntry): the coded size in pixels.
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    // Audio tracks (AudioSampleEntry; QuickTime version 2 entries too).
    std::uint32_t channelCount = 0;
    std::uint16_t sampleSize = 0;
    // Samples per second (integer part).
    std::uint32_t sampleRate = 0;
    // Video tracks with an AVC (avcC) or HEVC (hvcC) configuration: the size
    // of the length field before each NAL unit of a sample (1, 2 or 4 bytes);
    // 0 when there is no such configuration or its value is not allowed.
    std::uint8_t nalLengthSize = 0;
};

struct TimeToSample {
    std::uint32_t sampleCount = 0;
    std::uint32_t sampleDelta = 0;
};

struct SampleToChunk {
    // 1-based.
    std::uint32_t firstChunk = 0;
    std::uint32_t samplesPerChunk = 0;
    // 1-based index into the sample descriptions.
    std::uint32_t sampleDescriptionIndex = 0;
};

// A track's sample tables as the file records them.
struct SampleTable {
    std::vector<SampleDescription> descriptions;
    std::vector<TimeToSample> timeToSample;
    std::vector<SampleToChunk> sampleToChunk;
    // stsz/stz2: the number of samples, and either one size for all of them
    // (uniformSampleSize > 0) or one size each.
    std::uint32_t sampleCount = 0;
    std::uint32_t uniformSampleSize = 0;
    std::vector<std::uint32_t> sampleSizes;
    // stco/co64: offsets from the start of the file.
    std::vector<std::uint64_t> chunkOffsets;
    // Which boxes held them.
    bool compactSampleSizes = false;  // stz2
    bool wideChunkOffsets = false;    // co64

    // The size of sample `index` (0-based, below sampleCount).
    [[nodiscard]] std::uint32_t sampleSize(std::uint32_t index) const noexcept {
        return uniformSampleSize != 0 ? uniformSampleSize : sampleSizes[index];
    }
};

// Where a chunk lies, as parseFile() finds it (parseMovie() leaves Unchecked).
enum class ChunkPlacement : std::uint8_t {
    Unchecked,
    // Inside the payload of one mdat box.
    MediaData,
    // Inside the content, but not inside one mdat payload.
    OutsideMediaData,
    // It ends beyond the content: the data it needs is not there.
    BeyondData,
};

// A piece of a track's media data whose samples follow each other: a chunk
// of the sample tables, or a run (trun) of a movie fragment.
struct Chunk {
    // From stco/co64 (or a run's data offset): an offset from the start of the file.
    std::uint64_t offset = 0;
    // The sum of its samples' sizes.
    std::uint64_t size = 0;
    // 0-based index of its first sample, and how many it holds. For a run,
    // the index is into Track::fragmentSampleSizes.
    std::uint32_t firstSample = 0;
    std::uint32_t sampleCount = 0;
    std::uint32_t sampleDescriptionIndex = 0;
    ChunkPlacement placement = ChunkPlacement::Unchecked;

    [[nodiscard]] std::uint64_t end() const noexcept { return offset + size; }
};

struct Track {
    // The trak box.
    BoxHeader box;
    TrackHeader header;
    MediaHeader media;
    // hdlr's handler type ('vide', 'soun', 'text', 'sbtl', 'hint', 'meta', ...).
    FourCc handler;
    TrackKind kind = TrackKind::Other;
    SampleTable samples;
    // True when stsd, stts, stsc, stsz (or stz2) and stco (or co64) are all
    // there, well formed and agree: the chunks then describe every sample.
    bool sampleTableValid = false;
    // Every chunk, in order; filled only when sampleTableValid.
    std::vector<Chunk> chunks;
    // Movie fragments (parseFile() only): every run (trun) of this track's
    // samples, in file order, and the size of each of their samples.
    std::vector<Chunk> fragmentRuns;
    std::vector<std::uint32_t> fragmentSampleSizes;

    // The samples of the sample tables and of the movie fragments.
    [[nodiscard]] std::uint64_t totalSamples() const noexcept {
        return std::uint64_t{samples.sampleCount} + fragmentSampleSizes.size();
    }
};

// A track's defaults for its movie fragments (mvex/trex).
struct TrackExtends {
    std::uint32_t trackId = 0;
    // 1-based index into the track's sample descriptions.
    std::uint32_t sampleDescriptionIndex = 0;
    std::uint32_t sampleDuration = 0;
    std::uint32_t sampleSize = 0;
    std::uint32_t sampleFlags = 0;
};

struct Movie {
    // The moov box.
    BoxHeader box;
    MovieHeader header;
    std::vector<Track> tracks;
    // moov holds mvex: samples may be in movie fragments (moof), which
    // parseFile() reads with the defaults in trackExtends.
    bool fragmented = false;
    std::vector<TrackExtends> trackExtends;
    IssueList issues;

    [[nodiscard]] std::size_t count(TrackKind kind) const noexcept;
};

// A movie fragment (moof), as parseFile() reads it.
struct MovieFragment {
    BoxHeader box;
    // mfhd's sequence number (0 when mfhd is missing).
    std::uint32_t sequenceNumber = 0;
    // Its track fragments (traf) and the runs (trun) in them.
    std::uint32_t trackFragments = 0;
    std::uint32_t runs = 0;
};

// Calls visit(sampleIndex, offset, size) for every sample of a track, in
// order: the samples of its chunks (when the sample table is valid), then
// those of its movie fragment runs. Offsets are from the start of the file.
template <typename Visit>
void forEachSample(const Track& track, Visit&& visit) {
    for (const Chunk& chunk : track.chunks) {
        std::uint64_t offset = chunk.offset;
        for (std::uint32_t i = 0; i < chunk.sampleCount; ++i) {
            const std::uint32_t index = chunk.firstSample + i;
            const std::uint32_t size = track.samples.sampleSize(index);
            visit(index, offset, size);
            offset += size;
        }
    }
    // The parser keeps sampleCount + fragment samples below 2^32.
    const std::uint32_t base = track.samples.sampleCount;
    for (const Chunk& run : track.fragmentRuns) {
        std::uint64_t offset = run.offset;
        for (std::uint32_t i = 0; i < run.sampleCount; ++i) {
            const std::uint32_t index = run.firstSample + i;
            const std::uint32_t size = track.fragmentSampleSizes[index];
            visit(base + index, offset, size);
            offset += size;
        }
    }
}

// Parses a moov box and everything in it. `moov` is a Valid box header read
// from this content (from scanTopLevel, or found elsewhere: no ftyp or mdat
// is needed). Chunk offsets are reported as the file records them; their
// placement stays Unchecked. Fails with InvalidInput when `moov` is not a
// moov box inside the content or the limits are invalid, and with the
// reader's errors.
[[nodiscard]] Result<Movie> parseMovie(carving::IContentReader& content, const BoxHeader& moov,
                                       const ParseLimits& limits = {});

// Parses an ftyp box. Issues (a size that is not a whole number of brands)
// are added to `issues`.
[[nodiscard]] Result<FileType> parseFileType(carving::IContentReader& content, const BoxHeader& ftyp,
                                             IssueList& issues);

// ---------------------------------------------------------------------------
// A whole file
// ---------------------------------------------------------------------------

enum class FileStatus : std::uint8_t {
    // The boxes fill the content; ftyp, if there is one, comes first; there
    // is one moov, with at least one track; every track's sample table is
    // valid; every movie fragment is consistent with moov; every chunk and
    // run lies inside an mdat; no two of them overlap.
    Valid,
    // The data ends inside a box, before a moov box, or before the media data
    // that the chunks need, and nothing before that is malformed.
    Truncated,
    // Anything else: see the issues.
    Invalid,
};

struct Mp4File {
    Layout layout;
    std::optional<FileType> fileType;
    // The first moov box's movie; its tracks hold the runs of the fragments.
    std::optional<Movie> movie;
    // The movie fragments (moof), in content order (read only with a movie).
    std::vector<MovieFragment> fragments;
    // Every issue: the file's (layout, ftyp, boxes, fragments, chunk
    // placement and overlap) and the movie's.
    IssueList issues;
    FileStatus status = FileStatus::Invalid;
    // Valid: a summary; otherwise why (the first issue, or where the data ends).
    std::string detail;
};

// Parses the content as one file: the top-level boxes, the ftyp, the first
// moov and the movie fragments (moof), then checks each chunk and run against
// the file's mdat boxes (including one that is cut short by the end of the
// data). Content offset 0 is the start of the file. Fails with InvalidInput
// for invalid limits, and with the reader's errors.
[[nodiscard]] Result<Mp4File> parseFile(carving::IContentReader& content, const ParseLimits& limits = {});

}  // namespace recovery::formats::mp4
