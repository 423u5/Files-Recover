#include "formats/wav_format.hpp"

#include "recovery/byte_order.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

constexpr std::size_t kRiffHeader = 12;
constexpr std::size_t kChunkHeader = 8;
constexpr std::size_t kFmtSize = 16;
constexpr std::size_t kExtensibleSize = 40;
constexpr std::uint32_t kPlaceholderSize = 0xFFFFFFFF;

constexpr std::uint16_t kPcm = 0x0001;
constexpr std::uint16_t kFloat = 0x0003;
constexpr std::uint16_t kALaw = 0x0006;
constexpr std::uint16_t kMuLaw = 0x0007;
constexpr std::uint16_t kExtensible = 0xFFFE;

// Bytes 2 to 15 of every KSDATAFORMAT_SUBTYPE GUID that wraps a format tag
// (xxxxxxxx-0000-0010-8000-00AA00389B71, in its little-endian byte order).
constexpr std::array<std::uint8_t, 14> kSubtypeSuffix = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                         0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

using FourCc = std::array<std::byte, 4>;

FourCc fourCcAt(std::span<const std::byte> bytes, std::size_t offset) {
    FourCc code{};
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = bytes[offset + i];
    }
    return code;
}

bool is(const FourCc& code, std::string_view name) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if (code[i] != static_cast<std::byte>(name[i])) {
            return false;
        }
    }
    return true;
}

bool isPrintable(const FourCc& code) noexcept {
    return std::all_of(code.begin(), code.end(), [](std::byte byte) {
        const auto c = static_cast<std::uint8_t>(byte);
        return c >= 0x20 && c <= 0x7E;
    });
}

bool isRiffWave(std::span<const std::byte> header) noexcept {
    constexpr std::string_view kRiff = "RIFF";
    constexpr std::string_view kWave = "WAVE";
    if (header.size() < kRiffHeader) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (header[i] != static_cast<std::byte>(kRiff[i]) || header[8 + i] != static_cast<std::byte>(kWave[i])) {
            return false;
        }
    }
    return true;
}

std::string formatName(std::uint16_t tag) {
    switch (tag) {
    case kPcm:
        return "PCM";
    case kFloat:
        return "IEEE float";
    case kALaw:
        return "A-law";
    case kMuLaw:
        return "mu-law";
    case 0x0002:
        return "MS ADPCM";
    case 0x0011:
        return "IMA ADPCM";
    case 0x0031:
        return "GSM 6.10";
    case 0x0055:
        return "MPEG Layer III";
    default:
        break;
    }
    constexpr std::string_view kDigits = "0123456789ABCDEF";
    std::string text = "format 0x";
    for (int shift = 12; shift >= 0; shift -= 4) {
        text += kDigits[(tag >> shift) & 0xF];
    }
    return text;
}

struct Format {
    // The format tag, or for WAVE_FORMAT_EXTENSIBLE the tag its subtype wraps.
    std::uint16_t tag = 0;
    bool extensible = false;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;

    // Formats whose blocks are whole samples of every channel.
    [[nodiscard]] bool framesAreSamples() const noexcept {
        return tag == kPcm || tag == kFloat || tag == kALaw || tag == kMuLaw;
    }
};

// Reads the fields of a fmt chunk payload (at least 16 bytes); an extensible
// format that is too short keeps its extensible tag.
Format parseFormat(std::span<const std::byte> payload) noexcept {
    Format format;
    format.tag = loadLe16(payload, 0);
    format.channels = loadLe16(payload, 2);
    format.sampleRate = loadLe32(payload, 4);
    format.byteRate = loadLe32(payload, 8);
    format.blockAlign = loadLe16(payload, 12);
    format.bitsPerSample = loadLe16(payload, 14);
    if (format.tag == kExtensible && payload.size() >= kExtensibleSize) {
        format.extensible = true;
        format.tag = loadLe16(payload, 24);
    }
    return format;
}

// What is wrong with a fmt chunk's payload (all of it), or an empty string.
std::string checkFormat(std::span<const std::byte> payload) {
    const Format format = parseFormat(payload);
    if (format.channels == 0 || format.sampleRate == 0 || format.blockAlign == 0) {
        return "fmt has 0 channels, samples per second or block alignment";
    }
    if (loadLe16(payload, 0) == kExtensible) {
        if (payload.size() < kExtensibleSize || loadLe16(payload, 16) < 22) {
            return "WAVE_FORMAT_EXTENSIBLE fmt is too short";
        }
        const std::uint16_t validBits = loadLe16(payload, 18);
        if (validBits > format.bitsPerSample || format.bitsPerSample % 8 != 0) {
            return "WAVE_FORMAT_EXTENSIBLE sample sizes do not fit";
        }
        for (std::size_t i = 0; i < kSubtypeSuffix.size(); ++i) {
            if (loadU8(payload, 26 + i) != kSubtypeSuffix[i]) {
                return "WAVE_FORMAT_EXTENSIBLE subtype is not a wrapped format tag";
            }
        }
    }
    if (!format.framesAreSamples()) {
        return {};
    }
    if (format.bitsPerSample == 0 || format.bitsPerSample > 64 ||
        (format.tag == kFloat && format.bitsPerSample != 32 && format.bitsPerSample != 64) ||
        ((format.tag == kALaw || format.tag == kMuLaw) && format.bitsPerSample != 8)) {
        return "fmt has " + std::to_string(format.bitsPerSample) + " bits per sample for " + formatName(format.tag);
    }
    const std::uint32_t sampleBytes = (format.bitsPerSample + 7U) / 8U;
    if (format.blockAlign != format.channels * sampleBytes) {
        return "fmt block alignment " + std::to_string(format.blockAlign) + " is not channels x bytes per sample";
    }
    if (std::uint64_t{format.byteRate} != std::uint64_t{format.sampleRate} * format.blockAlign) {
        return "fmt byte rate is not samples per second x block alignment";
    }
    return {};
}

struct Chunk {
    FourCc type{};
    std::uint32_t size = 0;
    std::uint64_t offset = 0;
    std::uint64_t payload = 0;
};

// Walks a WAV file's chunks to the end of its RIFF data. See structure_walk.hpp for what a walk reports.
class WavWalker {
public:
    WavWalker(IContentReader& content, Depth depth) : content_(content), depth_(depth) {}

    Result<Walk> run();

private:
    Status visitChunk(const Chunk& chunk);
    Status visitList(const Chunk& chunk);
    [[nodiscard]] std::string summary() const;

    IContentReader& content_;
    Depth depth_;
    Walk walk_;
    std::optional<Format> format_;
    std::optional<std::uint64_t> dataSize_;
    std::uint64_t dataChunks_ = 0;
};

Status WavWalker::visitList(const Chunk& chunk) {
    if (chunk.size < 4) {
        walk_.noteProblem(chunk.offset, "LIST is too short for its type");
        return success();
    }
    const std::uint64_t end = chunk.payload + chunk.size;
    std::uint64_t position = chunk.payload + 4;
    while (position < end) {
        if (end - position < kChunkHeader) {
            walk_.noteProblem(chunk.offset, "a LIST sub-chunk header crosses the end of the list");
            return success();
        }
        Result<std::span<const std::byte>> header = content_.read(position, kChunkHeader);
        if (!header.ok()) {
            return header.error();
        }
        const FourCc type = fourCcAt(*header, 0);
        const std::uint32_t size = loadLe32(*header, 4);
        if (!isPrintable(type) || size > end - position - kChunkHeader) {
            walk_.noteProblem(chunk.offset, "LIST sub-chunks do not fit in the list");
            return success();
        }
        // The last sub-chunk's pad byte may be missing.
        position = std::min(end, position + kChunkHeader + size + (size & 1));
    }
    return success();
}

Status WavWalker::visitChunk(const Chunk& chunk) {
    if (is(chunk.type, "fmt ")) {
        if (format_.has_value()) {
            walk_.noteProblem(chunk.offset, "a second fmt chunk");
            return success();
        }
        if (dataChunks_ > 0) {
            walk_.noteProblem(chunk.offset, "fmt after the data");
        }
        if (chunk.size < kFmtSize) {
            walk_.noteProblem(chunk.offset, "fmt is too short");
            return success();
        }
        Result<std::span<const std::byte>> payload =
            content_.read(chunk.payload, std::min<std::size_t>(chunk.size, kExtensibleSize));
        if (!payload.ok()) {
            return payload.error();
        }
        format_ = parseFormat(*payload);
        if (std::string problem = checkFormat(*payload); !problem.empty()) {
            walk_.noteProblem(chunk.offset, std::move(problem));
        }
        return success();
    }
    if (is(chunk.type, "data")) {
        if (++dataChunks_ > 1) {
            walk_.noteProblem(chunk.offset, "a second data chunk");
            return success();
        }
        if (!format_.has_value()) {
            walk_.noteProblem(chunk.offset, "data before fmt");
        } else if (format_->framesAreSamples() && format_->blockAlign != 0 && chunk.size % format_->blockAlign != 0) {
            walk_.noteProblem(chunk.offset, "the data is not a whole number of blocks");
        }
        dataSize_ = chunk.size;
        return success();
    }
    if (is(chunk.type, "fact") && chunk.size < 4) {
        walk_.noteProblem(chunk.offset, "fact is too short");
        return success();
    }
    if (is(chunk.type, "LIST") && depth_ == Depth::Full) {
        return visitList(chunk);
    }
    // cue, bext, iXML, smpl, id3, JUNK and unknown chunks carry nothing the structure depends on.
    return success();
}

std::string WavWalker::summary() const {
    if (!format_.has_value()) {
        return "no fmt chunk";
    }
    std::string text = formatName(format_->tag);
    if (format_->extensible) {
        text += " (extensible)";
    }
    text += " " + std::to_string(format_->bitsPerSample) + "-bit, " + std::to_string(format_->sampleRate) + " Hz, " +
            std::to_string(format_->channels) + (format_->channels == 1 ? " channel" : " channels");
    if (dataSize_.has_value()) {
        text += ", " + std::to_string(*dataSize_) + " bytes of audio";
    }
    return text;
}

Result<Walk> WavWalker::run() {
    const std::uint64_t size = content_.size();
    Result<std::span<const std::byte>> start =
        content_.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size, kRiffHeader)));
    if (!start.ok()) {
        return start.error();
    }
    if (size < kRiffHeader) {
        constexpr std::string_view kRiff = "RIFF";
        for (std::size_t i = 0; i < std::min<std::size_t>(start->size(), 4); ++i) {
            if ((*start)[i] != static_cast<std::byte>(kRiff[i])) {
                return walk_.finish(WalkStatus::Broken, 0, "no RIFF WAVE header");
            }
        }
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside the RIFF header");
    }
    if (!isRiffWave(*start)) {
        return walk_.finish(WalkStatus::Broken, 0, "no RIFF WAVE header");
    }
    const std::uint32_t riffSize = loadLe32(*start, 4);
    if (riffSize < 4 + kChunkHeader) {
        return walk_.finish(WalkStatus::Broken, 0,
                            "the RIFF size " + std::to_string(riffSize) + " leaves no room for a chunk");
    }
    const std::uint64_t fileEnd = 8ULL + riffSize;
    std::uint64_t position = kRiffHeader;
    while (position < fileEnd) {
        if (fileEnd - position < kChunkHeader) {
            return walk_.finish(WalkStatus::Broken, position, "a chunk header crosses the end of the RIFF data");
        }
        Result<std::optional<std::span<const std::byte>>> header =
            detail::readIfAvailable(content_, position, kChunkHeader);
        if (!header.ok()) {
            return header.error();
        }
        if (!header->has_value()) {
            return walk_.finish(WalkStatus::Truncated, position, "the data ends before the next chunk");
        }
        const Chunk chunk{fourCcAt(**header, 0), loadLe32(**header, 4), position, position + kChunkHeader};
        if (!isPrintable(chunk.type)) {
            return walk_.finish(WalkStatus::Broken, position, "invalid chunk id");
        }
        if (chunk.size > fileEnd - chunk.payload) {
            return walk_.finish(WalkStatus::Broken, position,
                                "chunk " + detail::fourCcText(chunk.type) + " runs beyond the RIFF data");
        }
        // The last chunk's pad byte may be missing: the RIFF size ends the file.
        const std::uint64_t end = std::min(fileEnd, chunk.payload + chunk.size + (chunk.size & 1));
        if (end > size) {
            return walk_.finish(WalkStatus::Truncated, position,
                                "the data ends inside chunk " + detail::fourCcText(chunk.type));
        }
        if (Status visited = visitChunk(chunk); !visited.ok()) {
            return visited.error();
        }
        position = end;
    }
    if (!format_.has_value()) {
        walk_.noteProblem(kRiffHeader, "no fmt chunk");
    }
    if (dataChunks_ == 0) {
        walk_.noteProblem(kRiffHeader, "no data chunk");
    }
    return walk_.finish(WalkStatus::Complete, fileEnd, summary());
}

Result<Walk> walkWav(IContentReader& content, Depth depth) {
    return WavWalker(content, depth).run();
}

}  // namespace

WavFormat::WavFormat() {
    descriptor_.id = "wav";
    descriptor_.name = "WAV audio";
    descriptor_.extension = "wav";
    // "RIFF", any size, "WAVE".
    descriptor_.signatures = {carving::byteSignature(
        "RIFF WAVE header", {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}, 0,
        {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF})};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::SizeField;
}

HeaderCheck WavFormat::checkHeader(std::span<const std::byte> header) const {
    if (!isRiffWave(header)) {
        return HeaderCheck::reject("no RIFF WAVE header");
    }
    const std::uint32_t riffSize = loadLe32(header, 4);
    if (riffSize == kPlaceholderSize) {
        return HeaderCheck::reject("the RIFF size is the placeholder of a recording that was never finished");
    }
    if (riffSize < 4 + 2 * kChunkHeader + kFmtSize) {
        return HeaderCheck::reject("the RIFF size leaves no room for fmt and data");
    }
    const std::uint64_t fileEnd = 8ULL + riffSize;
    std::uint64_t position = kRiffHeader;
    while (position < fileEnd && position + kChunkHeader <= header.size()) {
        const auto at = static_cast<std::size_t>(position);
        const FourCc type = fourCcAt(header, at);
        const std::uint32_t size = loadLe32(header, at + 4);
        if (!isPrintable(type)) {
            return HeaderCheck::reject("invalid chunk id");
        }
        if (size > fileEnd - position - kChunkHeader) {
            return HeaderCheck::reject("chunk " + detail::fourCcText(type) + " runs beyond the RIFF data");
        }
        if (is(type, "fmt ") && size >= kFmtSize && header.size() - at - kChunkHeader >= kFmtSize) {
            const Format format = parseFormat(header.subspan(at + kChunkHeader, kFmtSize));
            if (format.tag == 0 || format.channels == 0 || format.sampleRate == 0 || format.blockAlign == 0) {
                return HeaderCheck::reject("fmt has zero fields");
            }
            break;
        }
        if (is(type, "data")) {
            break;
        }
        position += kChunkHeader + size + (size & 1);
    }
    return HeaderCheck::accept();
}

Result<EndDetection> WavFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkWav(content, Depth::Layout);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> WavFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkWav(content, Depth::Full);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
