#pragma once

// Builders of MP4 files (ISO base media file format) for the parser tests of
// P11. makeM4a() (audio_builders.hpp) writes the one layout audio encoders
// use; makeMp4() writes any layout:
//
//  * several video, audio and text tracks, with stsz or stz2 sample sizes (a
//    table or one size for all), stco or co64 chunk offsets, version 0 or 1
//    headers, and chunks of several samples (a second stsc entry for a short
//    last chunk);
//  * moov before the media data, after it, or between two mdat boxes;
//  * the media data in one mdat or spread over several, with free boxes
//    between them, and the tracks' chunks interleaved or one track after the
//    other;
//  * 32-bit or 64-bit sizes for mdat, for moov and for every box inside moov,
//    and an mdat of size 0 (to the end of the file);
//  * ftyp or none (old QuickTime), free, wide and uuid boxes, edit lists and
//    user data the parser skips, and every container's children in reverse
//    order.
//
//  * movie fragments (P12): moov with mvex, then moof and mdat pairs, with
//    the track fragments' base data offset given in each of the three ways
//    ISO/IEC 14496-12 allows, and optionally some samples in moov's tables.
//
// Each file comes with where every sample and chunk was written, to compare
// with what the parser finds. Audio samples are AAC frames that decode to
// silence, described by an esds box, and text samples are 3GPP timed text,
// so FFmpeg can demux the files (tests/reference/check_mp4_builder_files.sh);
// video samples are AVC (or HEVC) samples: NAL units of deterministic bytes,
// each after a length field of the size the avcC (hvcC) box gives, filling
// the sample exactly, which a demuxer passes on as they are.

#include "support/audio_builders.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

enum class Mp4TrackKind : std::uint8_t { Video, Audio, Text };

struct Mp4TrackOptions {
    Mp4TrackKind kind = Mp4TrackKind::Video;
    // 0: the track's number in moov (1, 2, ...).
    std::uint32_t trackId = 0;
    std::size_t samples = 12;
    // Samples per chunk; the last chunk holds what is left.
    std::size_t samplesPerChunk = 4;
    // Every sample this many bytes (0: sizes vary). Audio samples are then
    // plain bytes, not AAC.
    std::uint32_t fixedSampleSize = 0;
    // stsz with one size for all samples instead of a table (the sizes must be equal).
    bool uniformSize = false;
    // stz2 instead of stsz, with 4-, 8- or 16-bit fields as the largest size needs.
    bool compactSizes = false;
    // tkhd and mdhd of version 1 (64-bit times).
    bool version1 = false;
    // Video: the coded size. Audio: 1 or 2 channels, and the sample rate.
    std::uint16_t width = 64;
    std::uint16_t height = 48;
    std::uint8_t channels = 2;
    std::uint32_t sampleRate = 44100;
    // Video: HEVC ('hvc1' with hvcC) instead of AVC ('avc1' with avcC), and
    // the size of the NAL unit length fields (1, 2 or 4). Samples smaller
    // than a length field and one byte are written without NAL units.
    bool hevc = false;
    std::uint8_t nalLengthSize = 4;
    // P14: these bytes as the samples instead of generated ones (`samples`,
    // `fixedSampleSize` and the NAL unit framing are then not used), and
    // these bytes as the payload of the avcC or hvcC box (video) or as the
    // AudioSpecificConfig in the esds box (audio) instead of the builder's.
    std::vector<std::vector<std::byte>> sampleData;
    std::vector<std::byte> codecConfig;
};

// How a movie fragment's track fragments give where their data is.
enum class Mp4FragmentBase : std::uint8_t {
    // tfhd's default-base-is-moof flag; each run's data offset from the moof box (CMAF, FFmpeg's default_base_moof).
    Moof,
    // tfhd's base data offset, an absolute file offset; runs without a data offset follow it.
    Explicit,
    // Neither: the first track fragment's base is the moof box (its run has a data offset), later ones follow
    // the previous track fragment's data (their runs have none).
    Implicit,
};

enum class Mp4MoovPlace : std::uint8_t {
    // Before the media data ("fast start").
    First,
    // After the media data, as most recorders write it.
    Last,
    // Between the first and the second mdat box (needs mediaDataBoxes >= 2).
    Between,
};

struct Mp4Options {
    // No ftyp at all when empty (old QuickTime files).
    std::string majorBrand = "isom";
    std::vector<std::string> compatibleBrands = {"isom", "iso2", "avc1", "mp41"};
    std::vector<Mp4TrackOptions> tracks = {Mp4TrackOptions{Mp4TrackKind::Video},
                                           Mp4TrackOptions{Mp4TrackKind::Audio}};
    Mp4MoovPlace moov = Mp4MoovPlace::Last;
    // mvhd of version 1.
    bool movieVersion1 = false;
    // Chunks of the tracks alternate in the media data; otherwise track after track.
    bool interleave = true;
    // The chunks are spread over this many mdat boxes (at least 1), with a free box between neighbours.
    std::size_t mediaDataBoxes = 1;
    // 64-bit sizes for every mdat, for moov, and for every box inside moov.
    bool largeMediaData = false;
    bool largeMoov = false;
    bool largeNested = false;
    // co64 instead of stco in every track.
    bool co64 = false;
    // The last mdat has size 0 (to the end of the file); it must be the last box.
    bool mediaDataToEnd = false;
    // A uuid box and a free box after ftyp, and a wide box before each mdat (QuickTime).
    bool uuidBox = false;
    bool freeBox = false;
    bool wideBox = false;
    // edts/elst in each track and udta in moov: boxes the parser skips.
    bool extraBoxes = false;
    // mvhd after the tracks, and the children of trak, mdia, minf and stbl in reverse order (stco first, ...).
    bool reverseChildren = false;
    // Movie fragments: when not 0, moov (first) gets mvex with a trex per
    // track, its sample tables hold the first samplesInMoov samples of each
    // track (in one mdat after moov; 0: empty tables, as FFmpeg's empty_moov
    // writes), and the rest follow in moof and mdat pairs of this many samples
    // per track each.
    std::size_t fragmentSamples = 0;
    std::size_t samplesInMoov = 0;
    Mp4FragmentBase fragmentBase = Mp4FragmentBase::Moof;
    std::uint64_t seed = 31;
};

struct Mp4Sample {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

struct Mp4ChunkTruth {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t firstSample = 0;
    std::uint32_t sampleCount = 0;
};

struct Mp4Built {
    std::vector<std::byte> bytes;
    // Per track, in moov order: where each sample and chunk was written. The
    // samples of the movie fragments follow those of moov's tables; each run
    // is a chunk truth whose firstSample counts the fragments' samples only.
    std::vector<std::vector<Mp4Sample>> samples;
    std::vector<std::vector<Mp4ChunkTruth>> chunks;
    std::vector<std::vector<Mp4ChunkTruth>> runs;
};

[[nodiscard]] Mp4Built makeMp4(const Mp4Options& options = {});

// An MP4 file of any size, larger than memory (P12): `built`'s media data
// moved `gap` bytes further on. `built` must have moov first, one mdat with
// a 64-bit size and co64 offsets. The file is `head` (up to its first sample,
// with mdat's size and every chunk offset corrected), then `gap` bytes that
// belong to mdat but no sample (planted in a VirtualSource, they are its own
// content), then `media` (every sample).
struct SpreadMp4 {
    std::vector<std::byte> head;
    std::uint64_t gap = 0;
    std::vector<std::byte> media;
    // Per track, where each sample is in the spread file.
    std::vector<std::vector<Mp4Sample>> samples;

    [[nodiscard]] std::uint64_t size() const noexcept { return head.size() + gap + media.size(); }
    [[nodiscard]] std::uint64_t mediaOffset() const noexcept { return head.size() + gap; }
};

[[nodiscard]] SpreadMp4 spreadMp4(const Mp4Built& built, std::uint64_t gap);

// The boxes of an MP4 built above, in file order: container boxes (moov,
// trak, edts, mdia, minf, dinf, stbl, udta, mvex, moof, traf) are descended
// into, and so is stsd (its sample entries). Paths carry no track numbers:
// "moov/trak/mdia/minf/stbl/stco" appears once per track.
[[nodiscard]] std::vector<BoxPosition> mp4Boxes(std::span<const std::byte> file);

// The `index`th (0-based) box with this path; throws when there is none.
[[nodiscard]] BoxPosition findBox(const std::vector<BoxPosition>& boxes, std::string_view path,
                                  std::size_t index = 0);

// A box: 32-bit size (64-bit with `large`), type, payload.
[[nodiscard]] std::vector<std::byte> makeBox(std::string_view type, std::span<const std::byte> payload,
                                             bool large = false);
// A full box: version and flags, then the payload.
[[nodiscard]] std::vector<std::byte> makeFullBox(std::string_view type, std::uint8_t version, std::uint32_t flags,
                                                 std::span<const std::byte> payload);

}  // namespace recovery::test
