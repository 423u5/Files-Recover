#pragma once

// Builders of audio files for the format tests. They write files the way an
// encoder does, with data that decoders accept (tests/reference/
// check_audio_builder_files.sh runs FFmpeg and mpg123 on them):
//
//  * MP3: MPEG-1, MPEG-2 and MPEG-2.5 Layer III frames with real side
//    information and main data (count1 quadruples coded with Huffman table
//    B: quiet noise), a bit reservoir, CRCs, a Xing or Info tag with a LAME
//    extension, and ID3v2, APEv2, Lyrics3 and ID3v1 tags.
//  * ADTS: frames of AAC raw data blocks that decode to silence (channel
//    elements with no scale factor bands), padded with fill elements.
//  * M4A: ftyp, moov (mvhd, trak, mdia, hdlr, minf, stbl with stsd/esds,
//    stts, stsc, stsz, stco or co64) and mdat around the same AAC frames;
//    moov before or after mdat, 64-bit sizes, metadata, a video track.
//  * WAV: PCM, IEEE float, A-law, mu-law and IMA ADPCM, WAVE_FORMAT_EXTENSIBLE,
//    fact, LIST INFO, bext, JUNK and cue chunks.
//
// Content comes from a seed, so every file is reproducible and files made
// with different seeds differ.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

// ---------------------------------------------------------------------------
// MP3
// ---------------------------------------------------------------------------

enum class MpegVersion : std::uint8_t { Mpeg1, Mpeg2, Mpeg25 };
enum class Mp3Channels : std::uint8_t { Stereo, JointStereo, DualChannel, Mono };
// The first frame's info tag: none, Xing/Info fields only, or with a LAME
// extension (music length, music CRC and tag CRC). "Info" for a constant
// bitrate, "Xing" for a variable one, as LAME writes them.
enum class Mp3InfoTag : std::uint8_t { None, Xing, Lame };

struct Mp3Options {
    MpegVersion version = MpegVersion::Mpeg1;
    // Samples per second: one of the version's three rates.
    std::uint32_t sampleRate = 44100;
    // kbit/s of every frame; 0 varies it from frame to frame.
    std::uint32_t bitrate = 128;
    Mp3Channels channels = Mp3Channels::JointStereo;
    bool crc = false;
    // Audio frames (an info tag's frame comes on top).
    std::size_t frames = 20;
    Mp3InfoTag infoTag = Mp3InfoTag::Lame;
    // Main data that starts in earlier frames (main_data_begin above 0).
    bool reservoir = true;
    // ID3v2 tag before the frames: 0 for none, or the version 2, 3 or 4.
    std::uint8_t id3v2 = 0;
    // A picture frame in the ID3v2 tag holding these bytes (an embedded JPEG, say).
    std::vector<std::byte> picture;
    // Tags after the frames, in this order.
    bool apeTag = false;
    bool lyrics3 = false;
    bool id3v1 = false;
    std::uint64_t seed = 11;
};

[[nodiscard]] std::vector<std::byte> makeMp3(const Mp3Options& options = {});

struct Mp3Frame {
    // Offset of the frame header, and the frame's length.
    std::size_t offset = 0;
    std::size_t length = 0;
};

// The frames of a well-formed MP3 built above, the info tag's frame included
// (an independent, simple reader for building corruption tests).
[[nodiscard]] std::vector<Mp3Frame> mp3Frames(std::span<const std::byte> file);

// Tags on their own. The ID3v2 tag has a title and an artist frame (and a
// picture frame when `picture` is not empty) and 64 bytes of padding.
[[nodiscard]] std::vector<std::byte> id3v2Tag(std::uint8_t version, std::span<const std::byte> picture = {});
[[nodiscard]] std::vector<std::byte> id3v1Tag();
[[nodiscard]] std::vector<std::byte> apeTag();
[[nodiscard]] std::vector<std::byte> lyrics3Tag();

// ---------------------------------------------------------------------------
// AAC: ADTS and M4A
// ---------------------------------------------------------------------------

struct AacOptions {
    // One of the ADTS sampling frequencies.
    std::uint32_t sampleRate = 44100;
    // Channel configuration 1 (mono) or 2 (stereo).
    std::uint8_t channels = 2;
    // Audio object type minus one: 0 Main, 1 LC, 3 LTP.
    std::uint8_t profile = 1;
    // The ADTS header's MPEG-2 flag (MPEG-4 otherwise).
    bool mpeg2 = false;
    std::size_t frames = 20;
    // ID3v2 tag before the frames (0 for none, or 2 to 4), and ID3v1 after them.
    std::uint8_t id3v2 = 0;
    bool id3v1 = false;
    std::uint64_t seed = 12;
};

[[nodiscard]] std::vector<std::byte> makeAdts(const AacOptions& options = {});

// Offsets of the frames of an ADTS file built above.
[[nodiscard]] std::vector<std::size_t> adtsFrames(std::span<const std::byte> file);

// One AAC raw data block of silence for `channels` (1 or 2) channels, padded
// with fill elements to about `size` bytes (at least the few it needs).
[[nodiscard]] std::vector<std::byte> aacSilentFrame(std::uint8_t channels, std::size_t size);

struct M4aOptions {
    std::string majorBrand = "M4A ";
    std::vector<std::string> compatibleBrands = {"M4A ", "mp42", "isom"};
    // moov before mdat ("fast start"); after it otherwise, as encoders write it first.
    bool moovFirst = false;
    AacOptions audio;
    // Audio frames per chunk (stsc).
    std::size_t samplesPerChunk = 5;
    // Chunk offsets in a co64 box instead of stco.
    bool co64 = false;
    // mdat with a 64-bit size.
    bool largeMdat = false;
    // A free box before mdat, as FFmpeg writes it.
    bool freeBox = true;
    // udta with an iTunes-style meta/ilst title.
    bool metadata = false;
    // A video track after the sound track (with a few samples in mdat).
    bool videoTrack = false;
    // Whether the sound track exists at all (a video-only file without it).
    bool soundTrack = true;
};

[[nodiscard]] std::vector<std::byte> makeM4a(const M4aOptions& options = {});

struct BoxPosition {
    // "moov", "moov/trak/mdia/minf/stbl/stco", ... (container boxes are descended into).
    std::string path;
    std::size_t offset = 0;
    std::size_t size = 0;
};

// The boxes of an M4A built above, in file order.
[[nodiscard]] std::vector<BoxPosition> m4aBoxes(std::span<const std::byte> file);

// A box: 32-bit size, type, payload.
[[nodiscard]] std::vector<std::byte> mp4Box(std::string_view type, std::span<const std::byte> payload);

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

enum class WavEncoding : std::uint8_t { Pcm, Float, ALaw, MuLaw, ImaAdpcm };

struct WavOptions {
    WavEncoding encoding = WavEncoding::Pcm;
    std::uint16_t channels = 2;
    std::uint32_t sampleRate = 44100;
    // PCM: 8, 16, 24 or 32; Float: 32 or 64; ignored for the others.
    std::uint16_t bitsPerSample = 16;
    // Sample frames (PCM, float, A-law, mu-law) or blocks (IMA ADPCM).
    std::size_t frames = 1000;
    // WAVE_FORMAT_EXTENSIBLE for PCM or float.
    bool extensible = false;
    // Chunks: fact after fmt (always for IMA ADPCM), JUNK and bext before
    // fmt (BWF), LIST INFO after data, cue after data.
    bool fact = false;
    bool junk = false;
    bool bext = false;
    bool listInfo = false;
    bool cue = false;
    std::uint64_t seed = 14;
};

[[nodiscard]] std::vector<std::byte> makeWav(const WavOptions& options = {});

struct RiffChunk {
    std::string id;
    // Offset of the chunk header, and the payload size.
    std::size_t offset = 0;
    std::size_t size = 0;
};

// The top-level chunks of a RIFF file built above. (riffChunk() in
// image_builders.hpp writes one.)
[[nodiscard]] std::vector<RiffChunk> riffChunks(std::span<const std::byte> file);

}  // namespace recovery::test
