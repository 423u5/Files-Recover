// Media validation of AAC in ADTS streams: the raw data block of every frame
// as far as aac_syntax.hpp reads it without the Huffman tables, and, in
// frames that carry one, the CRC.
//
// The CRC covers the header and the first bits of each channel element
// (aac_syntax.hpp, Crc16). Where those regions lie is known once the
// elements before them were read to their ends; otherwise it is found:
//
//  * a channel element read only up to its coded scale factors covers its
//    first 192 bits when it is at least that long; when it is shorter, its
//    end is one of the positions from which fill and data stream elements
//    and ID_END end the frame exactly (aac::readTail), and each is tried;
//  * a CPE whose first channel stream holds coded data has a second stream
//    that starts somewhere after it: every position is tried (the CRC
//    rolled bit by bit), and a match counts only where a second stream's
//    global_gain, ics_info and section_data read without error.
//
// A frame fails only when no position gives the stored CRC. This needs the
// configuration's channel elements to be one SCE or CPE (mono and stereo);
// the CRC of other frames whose elements were not all read to their ends,
// of frames with a program config element (whose coverage is not known),
// and of frames of more than one raw data block is not checked.

#include "aac_syntax.hpp"
#include "media_decoders.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace recovery::validation::detail {

namespace {

using aac::Block;
using aac::BlockStatus;
using aac::Crc16;
using aac::Element;

constexpr std::size_t kHeaderBytes = 7;
constexpr std::uint64_t kHeaderBits = 56;
constexpr std::uint64_t kCrcBits = 16;
constexpr std::uint64_t kElementRegion = 192;
constexpr std::uint64_t kSecondStreamRegion = 128;
constexpr std::uint64_t kEndBits = 3;
// Sync, MPEG version, layer, protection, profile, sampling frequency and
// channel configuration: what every frame of a stream shares (as the ADTS
// structure walk takes it).
constexpr std::uint32_t kStreamMask = 0xFFFFFDC0;

struct AdtsHeader {
    std::uint32_t key = 0;
    bool mpeg2 = false;
    bool crc = false;
    std::uint32_t profile = 0;
    std::uint32_t samplingIndex = 0;
    std::uint32_t channelConfig = 0;
    std::uint32_t length = 0;
    std::uint32_t rawBlocks = 0;
};

std::optional<AdtsHeader> parseHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderBytes) {
        return std::nullopt;
    }
    const std::uint32_t bits = loadBe32(bytes, 0);
    if ((bits >> 20) != 0xFFF || ((bits >> 17) & 3U) != 0) {
        return std::nullopt;
    }
    AdtsHeader header;
    header.key = bits & kStreamMask;
    header.mpeg2 = ((bits >> 19) & 1U) != 0;
    header.crc = ((bits >> 16) & 1U) == 0;
    header.profile = (bits >> 14) & 3U;
    header.samplingIndex = (bits >> 10) & 0xFU;
    header.channelConfig = (bits >> 6) & 7U;
    header.length = ((bits & 3U) << 11) | (std::uint32_t{loadU8(bytes, 4)} << 3) | (loadU8(bytes, 5) >> 5);
    header.rawBlocks = (loadU8(bytes, 6) & 3U) + 1;
    const std::uint32_t overhead = static_cast<std::uint32_t>(kHeaderBytes) + (header.crc ? 2 * header.rawBlocks : 0);
    if (header.samplingIndex >= aac::kSamplingIndices || header.length <= overhead) {
        return std::nullopt;
    }
    return header;
}

// The end of the ID3v2 tags before the frames.
Result<std::uint64_t> skipLeadingTags(carving::IContentReader& content) {
    std::uint64_t position = 0;
    while (content.size() - position >= 10) {
        Result<std::span<const std::byte>> header = content.read(position, 10);
        if (!header.ok()) {
            return header.error();
        }
        if (loadU8(*header, 0) != 'I' || loadU8(*header, 1) != 'D' || loadU8(*header, 2) != '3') {
            break;
        }
        std::uint64_t size = 0;
        for (std::size_t i = 6; i < 10; ++i) {
            size = (size << 7) | (loadU8(*header, i) & 0x7FU);
        }
        const bool footer = loadU8(*header, 3) == 4 && (loadU8(*header, 5) & 0x10U) != 0;
        position += 10 + size + (footer ? 10 : 0);
        if (position > content.size()) {
            return content.size();
        }
    }
    return position;
}

bool bitAt(std::span<const std::uint8_t> bytes, std::uint64_t bit) {
    return ((bytes[static_cast<std::size_t>(bit / 8)] >> (7 - bit % 8)) & 1U) != 0;
}

enum class CrcCheck : std::uint8_t { Match, Mismatch, Unchecked };

// Finds the CRC regions of a frame whose last channel element was not read
// to its end (see the top of the file).
class CrcSearch {
public:
    CrcSearch(std::span<const std::uint8_t> frame, std::uint64_t blockBegin, const aac::Config& config,
              std::uint16_t stored)
        : frame_(frame), blockBegin_(blockBegin), blockEnd_(std::uint64_t{frame.size()} * 8), config_(config),
          stored_(stored) {}

    // `prefix`: the CRC through the regions of the elements before `element`.
    CrcCheck run(std::uint16_t prefix, const Element& element);

private:
    // Second-stream starts s in [from, last] whose 128-bit windows are all
    // data; after the window come `tailBits` protected bits whose CRC from
    // zero is `tailCrc`. `reg1` is the CRC through the element's first region.
    bool rolling(std::uint16_t reg1, const Element& element, std::uint64_t from, std::uint64_t last,
                 std::uint64_t streamEnd, std::uint64_t tailBits, std::uint16_t tailCrc) const;
    // Starts s in [from, to) with windows cut at `elementEnd` and padded.
    bool padded(std::uint16_t reg1, const Element& element, std::uint64_t from, std::uint64_t to,
                std::uint64_t elementEnd, std::uint64_t tailBits, std::uint16_t tailCrc) const;

    std::span<const std::uint8_t> frame_;
    std::uint64_t blockBegin_;
    std::uint64_t blockEnd_;
    const aac::Config& config_;
    std::uint16_t stored_;
};

bool CrcSearch::rolling(std::uint16_t reg1, const Element& element, std::uint64_t from, std::uint64_t last,
                        std::uint64_t streamEnd, std::uint64_t tailBits, std::uint16_t tailCrc) const {
    if (from > last) {
        return false;
    }
    // CRC after the window = reg1 * x^128 + L(window); L slides one bit at a
    // time: L' = x * L + (outgoing bit) * x^144 + (incoming bit) * x^16.
    const std::uint16_t shifted = Crc16::multiply(reg1, Crc16::power(kSecondStreamRegion));
    const std::uint16_t outgoing = Crc16::power(kSecondStreamRegion + 16);
    const std::uint16_t tailPower = Crc16::power(tailBits);
    std::uint16_t window = Crc16::bits(0, frame_, from, kSecondStreamRegion);
    for (std::uint64_t s = from;; ++s) {
        const auto total = static_cast<std::uint16_t>(Crc16::multiply(shifted ^ window, tailPower) ^ tailCrc);
        if (total == stored_ && aac::secondStreamFits(frame_, s, streamEnd, element, config_)) {
            return true;
        }
        if (s == last) {
            return false;
        }
        window = Crc16::timesX(window);
        if (bitAt(frame_, s)) {
            window ^= outgoing;
        }
        if (bitAt(frame_, s + kSecondStreamRegion)) {
            window ^= Crc16::kX16;
        }
    }
}

bool CrcSearch::padded(std::uint16_t reg1, const Element& element, std::uint64_t from, std::uint64_t to,
                       std::uint64_t elementEnd, std::uint64_t tailBits, std::uint16_t tailCrc) const {
    const std::uint16_t tailPower = Crc16::power(tailBits);
    for (std::uint64_t s = from; s < to; ++s) {
        const std::uint16_t reg2 = Crc16::region(reg1, frame_, s, elementEnd, kSecondStreamRegion);
        if (static_cast<std::uint16_t>(Crc16::multiply(reg2, tailPower) ^ tailCrc) == stored_ &&
            aac::secondStreamFits(frame_, s, elementEnd, element, config_)) {
            return true;
        }
    }
    return false;
}

CrcCheck CrcSearch::run(std::uint16_t prefix, const Element& element) {
    const bool cpe = element.id == aac::kCpe;
    // The second stream starts after the first one's flags at the earliest.
    const std::uint64_t firstStart = element.secondStream.value_or(element.readTo + 3);
    const std::uint64_t lastEnd = blockEnd_ - kEndBits;
    bool tried = false;

    // The element at least as long as its regions, nothing protected after it.
    if (element.start + kElementRegion <= lastEnd) {
        tried = true;
        const std::uint16_t reg1 = Crc16::bits(prefix, frame_, element.start, kElementRegion);
        if (!cpe) {
            if (reg1 == stored_) {
                return CrcCheck::Match;
            }
        } else if (element.secondStream.has_value()) {
            if (*element.secondStream + kSecondStreamRegion <= lastEnd &&
                Crc16::bits(reg1, frame_, *element.secondStream, kSecondStreamRegion) == stored_) {
                return CrcCheck::Match;
            }
        } else if (firstStart + kSecondStreamRegion <= lastEnd &&
                   rolling(reg1, element, firstStart, lastEnd - kSecondStreamRegion, blockEnd_, 0, 0)) {
            return CrcCheck::Match;
        }
    }

    // The element ends where a tail of fill and data stream elements and
    // ID_END ends the block.
    for (std::uint64_t end = element.readTo; end <= lastEnd; ++end) {
        const std::optional<std::vector<aac::Span>> tail = aac::readTail(frame_, blockBegin_, end, blockEnd_);
        if (!tail.has_value()) {
            continue;
        }
        tried = true;
        std::uint64_t tailBits = 0;
        std::uint16_t tailCrc = 0;
        for (const aac::Span& span : *tail) {
            tailCrc = Crc16::bits(tailCrc, frame_, span.begin, span.end - span.begin);
            tailBits += span.end - span.begin;
        }
        const std::uint16_t reg1 = Crc16::region(prefix, frame_, element.start, end, kElementRegion);
        if (!cpe) {
            if (static_cast<std::uint16_t>(Crc16::multiply(reg1, Crc16::power(tailBits)) ^ tailCrc) == stored_) {
                return CrcCheck::Match;
            }
            continue;
        }
        if (element.secondStream.has_value()) {
            if (*element.secondStream < end && padded(reg1, element, *element.secondStream, *element.secondStream + 1,
                                                      end, tailBits, tailCrc)) {
                return CrcCheck::Match;
            }
            continue;
        }
        // Windows wholly inside the element were tried above when the
        // element's first region holds no padding and no data stream element follows.
        const bool coveredAbove = element.start + kElementRegion <= end && tail->empty();
        if (!coveredAbove && end >= firstStart + kSecondStreamRegion &&
            rolling(reg1, element, firstStart, end - kSecondStreamRegion, end, tailBits, tailCrc)) {
            return CrcCheck::Match;
        }
        const std::uint64_t paddedFrom =
            std::max(firstStart, end >= kSecondStreamRegion ? end - kSecondStreamRegion + 1 : 0);
        if (padded(reg1, element, paddedFrom, end, end, tailBits, tailCrc)) {
            return CrcCheck::Match;
        }
    }
    return tried ? CrcCheck::Mismatch : CrcCheck::Unchecked;
}

// The CRC of one frame of one raw data block.
CrcCheck checkCrc(std::span<const std::uint8_t> frame, const Block& block, std::uint64_t blockBegin,
                  const aac::Config& config) {
    const auto stored = static_cast<std::uint16_t>((frame[7] << 8) | frame[8]);
    if (block.count(aac::kPce) > 0) {
        return CrcCheck::Unchecked;
    }
    std::uint16_t crc = Crc16::bits(Crc16::kInitial, frame, 0, kHeaderBits);
    const auto add = [&](const Element& element) {
        switch (element.id) {
        case aac::kSce:
        case aac::kLfe:
            crc = Crc16::region(crc, frame, element.start, *element.end, kElementRegion);
            break;
        case aac::kCpe:
            crc = Crc16::region(crc, frame, element.start, *element.end, kElementRegion);
            crc = Crc16::region(crc, frame, *element.secondStream, *element.end, kSecondStreamRegion);
            break;
        case aac::kDse:
            crc = Crc16::bits(crc, frame, element.start, *element.end - element.start);
            break;
        default:
            break;
        }
    };
    if (block.status == BlockStatus::Complete) {
        for (const Element& element : block.elements) {
            add(element);
        }
        return crc == stored ? CrcCheck::Match : CrcCheck::Mismatch;
    }
    // Stopped inside the last element read: only a lone channel element can be searched.
    if (block.elements.empty() || block.elements.back().end.has_value() || !block.elements.back().isChannel() ||
        (config.channelConfig != 1 && config.channelConfig != 2)) {
        return CrcCheck::Unchecked;
    }
    for (std::size_t i = 0; i + 1 < block.elements.size(); ++i) {
        add(block.elements[i]);
    }
    CrcSearch search(frame, blockBegin, config, stored);
    return search.run(crc, block.elements.back());
}

struct Tally {
    std::uint64_t frames = 0;
    std::uint64_t complete = 0;
    std::uint64_t stopped = 0;
    std::uint64_t multiBlock = 0;
    std::uint64_t crcMatched = 0;
    std::uint64_t crcUnchecked = 0;
};

std::string plural(std::uint64_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

class AacMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "aac"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content, const MediaLimits&) const override {
        const MediaVerdict verdict("aac decoder");
        Result<std::uint64_t> start = skipLeadingTags(content);
        if (!start.ok()) {
            return start.error();
        }
        const std::uint64_t size = content.size();
        std::uint64_t position = *start;
        std::optional<AdtsHeader> first;
        aac::Config config;
        Tally tally;
        std::vector<std::uint8_t> frame;
        while (size - position >= kHeaderBytes) {
            Result<std::span<const std::byte>> bytes = content.read(position, kHeaderBytes);
            if (!bytes.ok()) {
                return bytes.error();
            }
            const std::optional<AdtsHeader> header = parseHeader(*bytes);
            if (!header.has_value() || (first.has_value() && header->key != first->key)) {
                break;
            }
            if (!first.has_value()) {
                first = header;
                if (header->profile + 1 == aac::kAacSsr) {
                    return verdict.unsupported("AAC SSR is not read");
                }
                config.objectType = header->profile + 1;
                config.samplingIndex = header->samplingIndex;
                config.channelConfig = header->channelConfig;
            }
            if (header->length > size - position) {
                return verdict.truncated(position, "the data ends inside frame " + std::to_string(tally.frames + 1) +
                                                       "; " + summary(*first, tally));
            }
            Result<std::span<const std::byte>> data = content.read(position, header->length);
            if (!data.ok()) {
                return data.error();
            }
            frame.resize(data->size());
            std::transform(data->begin(), data->end(), frame.begin(),
                           [](std::byte value) { return static_cast<std::uint8_t>(value); });
            ++tally.frames;
            if (std::optional<LevelResult> failed = checkFrame(verdict, frame, *header, config, position, tally)) {
                return *failed;
            }
            position += header->length;
        }
        if (!first.has_value()) {
            return verdict.failed(position, "no ADTS frame");
        }
        return verdict.passed(summary(*first, tally), Coverage::Partial);
    }

private:
    static std::optional<LevelResult> checkFrame(const MediaVerdict& verdict, std::span<const std::uint8_t> frame,
                                                 const AdtsHeader& header, const aac::Config& config,
                                                 std::uint64_t offset, Tally& tally) {
        const std::string name = "frame " + std::to_string(tally.frames);
        const std::uint64_t end = std::uint64_t{frame.size()} * 8;
        if (header.rawBlocks > 1) {
            // raw_data_block_position fields and per-block CRCs: only the first block is read.
            ++tally.multiBlock;
            const std::uint64_t begin = kHeaderBits + (header.crc ? kCrcBits * header.rawBlocks : 0);
            const Block block = aac::readRawDataBlock(frame, begin, end, config);
            if (block.status == BlockStatus::Invalid) {
                return verdict.failed(offset + block.at / 8, name + ", raw data block 1: " + block.detail);
            }
            ++(block.status == BlockStatus::Complete ? tally.complete : tally.stopped);
            if (header.crc) {
                ++tally.crcUnchecked;
            }
            return std::nullopt;
        }
        const std::uint64_t begin = kHeaderBits + (header.crc ? kCrcBits : 0);
        const Block block = aac::readRawDataBlock(frame, begin, end, config);
        if (block.status == BlockStatus::Invalid) {
            return verdict.failed(offset + block.at / 8, name + ": " + block.detail);
        }
        if (block.status == BlockStatus::Complete) {
            const std::uint64_t aligned = (block.end + 7) / 8 * 8;
            if (aligned != end) {
                return verdict.failed(offset + aligned / 8,
                                      name + ": ID_END ends the raw data block " +
                                          plural(frame.size() - aligned / 8, "byte", "bytes") +
                                          " before the end of the frame");
            }
            ++tally.complete;
        } else {
            ++tally.stopped;
        }
        if (!header.crc) {
            return std::nullopt;
        }
        switch (checkCrc(frame, block, begin, config)) {
        case CrcCheck::Match:
            ++tally.crcMatched;
            break;
        case CrcCheck::Unchecked:
            ++tally.crcUnchecked;
            break;
        case CrcCheck::Mismatch:
            return verdict.failed(offset, name + ": the CRC does not match the bits it protects");
        }
        return std::nullopt;
    }

    static std::string summary(const AdtsHeader& first, const Tally& tally) {
        const std::uint32_t channels = first.channelConfig == 7 ? 8 : first.channelConfig;
        std::string text = std::string(first.mpeg2 ? "MPEG-2 " : "MPEG-4 ") + aac::objectTypeName(first.profile + 1) +
                           ", " + std::to_string(aac::sampleRate(first.samplingIndex)) + " Hz, " +
                           (channels == 0 ? std::string("channels in a PCE")
                                          : plural(channels, "channel", "channels")) +
                           ": " + plural(tally.frames, "frame", "frames") +
                           "; raw data blocks read through ID_END in " +
                           std::to_string(tally.complete) + ", up to coded data in " +
                           std::to_string(tally.stopped);
        if (tally.multiBlock > 0) {
            text += "; " + plural(tally.multiBlock, "frame", "frames") + " of several blocks (first block read)";
        }
        if (first.crc) {
            text += "; CRC matches in " + std::to_string(tally.crcMatched);
            if (tally.crcUnchecked > 0) {
                text += ", not checked in " + std::to_string(tally.crcUnchecked);
            }
        }
        return text;
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeAacMediaValidator() {
    return std::make_shared<AacMediaValidator>();
}

}  // namespace recovery::validation::detail
