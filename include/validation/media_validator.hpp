#pragma once

// Media validation (P14): the engine's own deterministic decoders, one per
// carving format, that check the coded media inside a file rather than the
// structure around it. They decode and discard: no pixel or sample is kept
// beyond what the next one depends on.
//
//   jpeg   baseline, extended and progressive Huffman-coded scans: every
//          code, coefficient and MCU, restart intervals, the end of each scan
//   png    the zlib stream of every image (and APNG frame): inflate, the
//          exact size of the scanlines, their filter types, Adler-32
//   gif    the LZW data of every image: every code, the exact pixel count
//   bmp    RLE8 and RLE4 data: every run stays inside the bitmap
//   webp   VP8L (lossless) images and alpha in full; VP8 (lossy) frame
//          headers and partitions
//   wav    IMA and Microsoft ADPCM block headers; PCM has nothing coded
//   aac    ADTS: the CRC of protected frames, the first syntax element of
//          each raw data block (its window, bands and sections)
//   m4a, mp4
//          the decoder configurations (avcC, hvcC, esds) and every sample
//          they describe: AVC and HEVC parameter sets and slice headers, AAC
//          raw data blocks as for ADTS
//   mp3    not decoded: Unsupported
//
// See docs/formats/media_validation.md.

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"
#include "validation/validation.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace recovery::validation {

// Thread safety: validators are immutable; validate() is const and safe to
// call concurrently on different readers.
class IMediaValidator {
public:
    virtual ~IMediaValidator() = default;

    // The carving format whose files it decodes (FormatDescriptor::id).
    [[nodiscard]] virtual std::string_view formatId() const noexcept = 0;

    // Decodes the media of the file whose bytes are all of `content`. The
    // result's checker names the decoder. Problems of the content are results
    // (Failed, Truncated, Unsupported), never errors; errors are only the
    // reader's (Cancelled, source failures).
    [[nodiscard]] virtual Result<LevelResult> validate(carving::IContentReader& content,
                                                       const MediaLimits& limits) const = 0;

protected:
    IMediaValidator() = default;
    IMediaValidator(const IMediaValidator&) = default;
    IMediaValidator& operator=(const IMediaValidator&) = default;
    IMediaValidator(IMediaValidator&&) = default;
    IMediaValidator& operator=(IMediaValidator&&) = default;
};

// Thread safety: add() must not overlap other calls; the const functions are
// safe concurrently.
class MediaValidatorRegistry {
public:
    // Fails with InvalidInput, leaving the registry unchanged, for a null
    // validator or a format that has one already.
    [[nodiscard]] Status add(std::shared_ptr<const IMediaValidator> validator);

    // The validator for this format id, or nullptr.
    [[nodiscard]] const IMediaValidator* find(std::string_view formatId) const noexcept;
    [[nodiscard]] const std::vector<std::shared_ptr<const IMediaValidator>>& validators() const noexcept {
        return validators_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return validators_.size(); }

private:
    std::vector<std::shared_ptr<const IMediaValidator>> validators_;
};

// The decoders for the formats of P9 (jpeg, png, webp, gif, bmp), P10 (mp3,
// wav, m4a, aac) and P12 (mp4).
[[nodiscard]] std::vector<std::shared_ptr<const IMediaValidator>> mediaValidators();

// Adds mediaValidators() to `registry`. Fails with InvalidInput, leaving the
// registry unchanged, when one of their formats has a validator already.
[[nodiscard]] Status registerMediaValidators(MediaValidatorRegistry& registry);

}  // namespace recovery::validation
