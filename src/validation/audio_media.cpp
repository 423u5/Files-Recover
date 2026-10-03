// Media validation of WAV and MP3 files.
//
// WAV: PCM, IEEE float, A-law and mu-law samples are stored as they are,
// with nothing coded to decode. IMA and Microsoft ADPCM code their samples in
// blocks, each starting with a header per channel; the 4-bit samples can take
// any value, so the headers are what can be wrong: IMA's step index (0 to 88)
// and the reserved byte after it (0: FFmpeg reads both as one 16-bit index
// and rejects the block, libsndfile reports a synchronisation error), and
// Microsoft's predictor index (one of the coefficient pairs fmt declares).
// Other codings are not decoded.
//
// MP3: MPEG audio is not decoded by the engine (the structure checks its
// frame headers, side information, bit reservoir and CRCs).

#include "content_bytes.hpp"
#include "media_decoders.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

constexpr std::uint16_t kPcm = 0x0001;
constexpr std::uint16_t kMsAdpcm = 0x0002;
constexpr std::uint16_t kFloat = 0x0003;
constexpr std::uint16_t kALaw = 0x0006;
constexpr std::uint16_t kMuLaw = 0x0007;
constexpr std::uint16_t kImaAdpcm = 0x0011;
constexpr std::uint16_t kExtensible = 0xFFFE;
constexpr std::uint32_t kMaxImaStep = 88;
constexpr std::uint32_t kMsStandardCoefficients = 7;

std::string hex16(std::uint32_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text = "0x";
    for (int shift = 12; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> shift) & 0x0F]);
    }
    return text;
}

class WavMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "wav"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content, const MediaLimits&) const override {
        const MediaVerdict verdict("wav decoder");
        const std::uint64_t size = content.size();
        if (size < 12) {
            return verdict.truncated(size, "the data ends inside the RIFF header");
        }
        Result<std::span<const std::byte>> riff = content.read(0, 12);
        if (!riff.ok()) {
            return riff.error();
        }
        const std::uint64_t riffEnd = 8 + std::uint64_t{loadLe32(*riff, 4)};
        const std::uint64_t end = std::min(size, riffEnd);
        std::vector<std::byte> format;
        bool formatCut = false;
        std::optional<std::uint64_t> dataBegin;
        std::uint64_t dataSize = 0;
        for (std::uint64_t offset = 12; offset + 8 <= end && !dataBegin.has_value();) {
            Result<std::span<const std::byte>> header = content.read(offset, 8);
            if (!header.ok()) {
                return header.error();
            }
            const std::uint32_t length = loadLe32(*header, 4);
            const bool fmt = loadU8(*header, 0) == 'f' && loadU8(*header, 1) == 'm' && loadU8(*header, 2) == 't' &&
                             loadU8(*header, 3) == ' ';
            const bool data = loadU8(*header, 0) == 'd' && loadU8(*header, 1) == 'a' && loadU8(*header, 2) == 't' &&
                              loadU8(*header, 3) == 'a';
            if (fmt && format.empty()) {
                const auto take = static_cast<std::size_t>(std::min<std::uint64_t>({length, 1024, size - offset - 8}));
                Result<std::span<const std::byte>> payload = content.read(offset + 8, take);
                if (!payload.ok()) {
                    return payload.error();
                }
                format.assign(payload->begin(), payload->end());
                formatCut = take < std::min<std::uint32_t>(length, 1024);
            } else if (data) {
                dataBegin = offset + 8;
                dataSize = length;
            }
            offset += 8 + std::uint64_t{length} + (length & 1U);
        }
        if (formatCut || (format.empty() && !dataBegin.has_value() && riffEnd > size)) {
            return verdict.truncated(size, "the data ends before the end of the fmt chunk");
        }
        if (format.size() < 16) {
            return verdict.failed(12, "no complete fmt chunk before the data");
        }
        std::uint16_t tag = loadLe16(format, 0);
        const std::uint16_t channels = loadLe16(format, 2);
        const std::uint16_t blockAlign = loadLe16(format, 12);
        const bool extensible = tag == kExtensible && format.size() >= 40;
        if (extensible) {
            tag = loadLe16(format, 24);
        }
        switch (tag) {
        case kPcm:
        case kFloat:
        case kALaw:
        case kMuLaw:
            return verdict.notApplicable("PCM, IEEE float, A-law or mu-law samples: nothing is coded");
        case kImaAdpcm:
        case kMsAdpcm:
            break;
        default:
            return verdict.unsupported("format " + hex16(tag) + " is not decoded");
        }
        const bool ima = tag == kImaAdpcm;
        const std::string name = ima ? "IMA ADPCM" : "Microsoft ADPCM";
        if (!dataBegin.has_value()) {
            if (riffEnd > size) {
                return verdict.truncated(size, "the data ends before the data chunk");
            }
            return verdict.failed(12, name + ": no data chunk");
        }
        std::uint32_t coefficients = 0;
        if (!ima && extensible) {
            // WAVE_FORMAT_EXTENSIBLE has no room for the coefficient table.
            return verdict.unsupported("Microsoft ADPCM inside WAVE_FORMAT_EXTENSIBLE is not decoded");
        }
        if (!ima) {
            coefficients = format.size() >= 22 ? loadLe16(format, 20) : 0;
            if (coefficients < kMsStandardCoefficients || format.size() < 22 + 4 * std::size_t{coefficients}) {
                return verdict.failed(12, name + ": fmt declares " + std::to_string(coefficients) +
                                              " coefficient pairs (at least the 7 standard ones)");
            }
        }
        const std::uint64_t headerSize = std::uint64_t{channels} * (ima ? 4 : 7);
        if (channels == 0 || blockAlign < headerSize) {
            return verdict.failed(12, name + ": blocks of " + std::to_string(blockAlign) + " bytes for " +
                                          std::to_string(channels) + " channels");
        }
        // The headers of every block.
        const std::uint64_t dataEnd = *dataBegin + dataSize;
        ContentBytes bytes(content, *dataBegin, dataEnd);
        std::vector<std::uint8_t> header(static_cast<std::size_t>(headerSize));
        std::uint64_t blocks = 0;
        for (std::uint64_t block = *dataBegin; block < dataEnd; block += blockAlign) {
            if (dataEnd - block < headerSize) {
                break;  // a final piece shorter than a header: no samples to check
            }
            bool complete = true;
            for (std::uint8_t& value : header) {
                complete = complete && bytes.next(value);
            }
            if (!complete) {
                if (bytes.error().has_value()) {
                    return *bytes.error();
                }
                break;  // the content ends inside the header
            }
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                if (ima) {
                    const std::uint32_t step = header[4 * channel + 2];
                    const std::uint32_t reserved = header[4 * channel + 3];
                    if (step > kMaxImaStep || reserved != 0) {
                        return verdict.failed(block, name + " block " + std::to_string(blocks) + ", channel " +
                                                         std::to_string(channel + 1) + ": step index " +
                                                         std::to_string(step) + " (0 to 88), reserved byte " +
                                                         std::to_string(reserved) + " (0)");
                    }
                } else if (header[channel] >= coefficients) {
                    return verdict.failed(block, name + " block " + std::to_string(blocks) + ", channel " +
                                                     std::to_string(channel + 1) + ": predictor " +
                                                     std::to_string(header[channel]) + " of " +
                                                     std::to_string(coefficients));
                }
            }
            ++blocks;
            if (blockAlign > headerSize && !bytes.skip(blockAlign - headerSize)) {
                if (bytes.error().has_value()) {
                    return *bytes.error();
                }
                break;
            }
        }
        std::string detail = name + ", " + std::to_string(channels) + (channels == 1 ? " channel: " : " channels: ") +
                             std::to_string(blocks) + " block headers valid; the 4-bit samples take any value";
        if (dataEnd > size) {
            return verdict.truncated(size, "the data ends inside the samples; " + detail);
        }
        return verdict.passed(std::move(detail));
    }
};

class Mp3MediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "mp3"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader&, const MediaLimits&) const override {
        return MediaVerdict("mp3 decoder")
            .unsupported("MPEG audio is not decoded by the engine (the structure checks its frame headers, side "
                         "information, bit reservoir and CRCs)");
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeWavMediaValidator() {
    return std::make_shared<WavMediaValidator>();
}

std::shared_ptr<const IMediaValidator> makeMp3MediaValidator() {
    return std::make_shared<Mp3MediaValidator>();
}

}  // namespace recovery::validation::detail
