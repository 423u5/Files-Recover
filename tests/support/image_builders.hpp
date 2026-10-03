#pragma once

// Builders of image files for the format tests. They write files the way an
// encoder does, with real image data where that is cheap, so external
// decoders accept them (tests/reference/check_image_builder_files.sh):
//
//  * JPEG: a baseline and progressive (spectral selection) encoder with a
//    DCT, quantization, Huffman coding, byte stuffing and restart markers;
//    JFIF and Exif segments (the Exif segment holds a thumbnail JPEG).
//  * PNG: every color type and bit depth, Adam7 interlacing, a zlib stream of
//    stored deflate blocks split over IDAT chunks, ancillary chunks, APNG.
//  * GIF: 87a and 89a, global and local color tables, LZW data, frames,
//    graphic control, comment and NETSCAPE loop extensions, interlacing.
//  * BMP: OS/2 core, BITMAPINFOHEADER, V4 and V5 headers; 1 to 32 bits per
//    pixel; uncompressed, bit fields, RLE8 and RLE4; top-down; a V5 color profile.
//  * WebP: simple and extended files, alpha, metadata and animations around
//    the VP8 and VP8L bitstreams that libwebp made (image_samples.hpp).
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
// JPEG
// ---------------------------------------------------------------------------

enum class JpegSampling : std::uint8_t { Gray, Yuv444, Yuv422, Yuv420 };

struct JpegOptions {
    std::uint16_t width = 64;
    std::uint16_t height = 48;
    JpegSampling sampling = JpegSampling::Yuv420;
    // SOF2, a DC scan and one AC scan per component; otherwise SOF0 and one scan.
    bool progressive = false;
    // MCUs per restart interval (0: no DRI and no restart markers).
    std::uint16_t restartInterval = 0;
    int quality = 80;
    // An APP0 JFIF segment.
    bool jfif = true;
    // An APP1 Exif segment whose IFD1 points to a thumbnail: a complete
    // 16x16 JPEG (SOI to EOI) inside the segment.
    bool exifThumbnail = false;
    // A COM segment with this text (none when empty).
    std::string comment;
    // 0xFF fill bytes written before every marker after SOI.
    std::size_t fillBytes = 0;
    std::uint64_t seed = 1;
};

[[nodiscard]] std::vector<std::byte> makeJpeg(const JpegOptions& options = {});

// A marker segment: FF <marker>, a 16-bit length, the payload.
[[nodiscard]] std::vector<std::byte> jpegSegment(std::uint8_t marker, std::span<const std::byte> payload);

struct JpegMarker {
    std::uint8_t code = 0;
    // Offset of the marker's 0xFF.
    std::size_t offset = 0;
    // Segment length (0 for markers without one).
    std::size_t length = 0;
};

// The markers of a well-formed JPEG in file order, restart markers included
// (an independent, simple reader for building corruption tests).
[[nodiscard]] std::vector<JpegMarker> jpegMarkers(std::span<const std::byte> file);

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

enum class PngColor : std::uint8_t { Gray = 0, Rgb = 2, Palette = 3, GrayAlpha = 4, Rgba = 6 };

struct PngOptions {
    std::uint32_t width = 32;
    std::uint32_t height = 24;
    PngColor color = PngColor::Rgb;
    std::uint8_t bitDepth = 8;
    bool interlaced = false;
    // Bytes of zlib data per IDAT chunk (0: one IDAT).
    std::size_t idatSize = 0;
    // tEXt before the image data and tIME after it.
    bool textChunks = false;
    // APNG: acTL and fcTL before the image data, a second frame in fdAT after it.
    bool animated = false;
    std::uint64_t seed = 2;
};

[[nodiscard]] std::vector<std::byte> makePng(const PngOptions& options = {});

// A chunk: length, type, data, CRC-32.
[[nodiscard]] std::vector<std::byte> pngChunk(std::string_view type, std::span<const std::byte> data);

struct PngChunkPosition {
    std::string type;
    // Offset of the chunk's length field.
    std::size_t offset = 0;
    std::size_t length = 0;
};

[[nodiscard]] std::vector<PngChunkPosition> pngChunks(std::span<const std::byte> file);

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

struct GifOptions {
    std::uint16_t width = 32;
    std::uint16_t height = 24;
    bool version89a = true;
    // 2^colorBits colors (1 to 8).
    std::uint8_t colorBits = 4;
    bool globalColorTable = true;
    bool localColorTables = false;
    std::size_t frames = 1;
    bool interlaced = false;
    // A graphic control extension before each frame (89a only).
    bool graphicControl = true;
    // A NETSCAPE2.0 looping application extension (89a only).
    bool loop = false;
    // A comment extension with this text (89a only; none when empty).
    std::string comment;
    std::uint64_t seed = 3;
};

[[nodiscard]] std::vector<std::byte> makeGif(const GifOptions& options = {});

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------

enum class BmpHeader : std::uint8_t { Core, Info, V4, V5 };
enum class BmpCompression : std::uint8_t { Rgb, Rle8, Rle4, Bitfields };

struct BmpOptions {
    std::int32_t width = 17;
    std::int32_t height = 11;
    std::uint16_t bitsPerPixel = 24;
    BmpHeader header = BmpHeader::Info;
    BmpCompression compression = BmpCompression::Rgb;
    bool topDown = false;
    // 0 in the file size field.
    bool zeroFileSize = false;
    // V5 only: an embedded color profile of this many bytes after the pixel data.
    std::size_t profileSize = 0;
    std::uint64_t seed = 4;
};

[[nodiscard]] std::vector<std::byte> makeBmp(const BmpOptions& options = {});

// ---------------------------------------------------------------------------
// WebP
// ---------------------------------------------------------------------------

enum class WebpKind : std::uint8_t {
    // A VP8 (lossy) or VP8L (lossless) image; simple format unless extended.
    Lossy,
    Lossless,
    // VP8X, an uncompressed ALPH chunk and VP8.
    LossyWithAlpha,
    // VP8X, ANIM and ANMF frames alternating VP8 and VP8L on a 32x16 canvas.
    Animated,
};

struct WebpOptions {
    WebpKind kind = WebpKind::Lossy;
    // VP8X even for a plain image (always for alpha and animation).
    bool extended = false;
    // Metadata chunks of these sizes (0: none); they make the file extended.
    std::size_t iccSize = 0;
    std::size_t exifSize = 0;
    std::size_t xmpSize = 0;
    // An unknown chunk of odd size (so it has a padding byte) after the image.
    bool unknownChunk = false;
    std::size_t frames = 2;
};

[[nodiscard]] std::vector<std::byte> makeWebp(const WebpOptions& options = {});

// A RIFF chunk: FourCC, 32-bit size, payload, and a padding byte when the size is odd.
[[nodiscard]] std::vector<std::byte> riffChunk(std::string_view fourCc, std::span<const std::byte> payload);

// The payload of the simple-format sample `name` (its only chunk: VP8 or VP8L).
[[nodiscard]] std::vector<std::byte> webpBitstream(std::string_view sampleName);

}  // namespace recovery::test
