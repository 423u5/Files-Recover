#include "support/carving_formats.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/crc32.hpp"
#include "support/test_files.hpp"

#include <algorithm>
#include <string>

namespace recovery::test {

using carving::EndDetection;
using carving::EndDetectionMethod;
using carving::EndStatus;
using carving::FormatDescriptor;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;

namespace {

constexpr std::size_t kChunk = 64 * 1024;

// Keeps the first bytes of every test signature ('S', '<', 't', 0xFF) out of
// generated payloads, so a payload never holds a signature by chance.
void sanitize(std::vector<std::byte>& bytes) {
    for (std::byte& byte : bytes) {
        switch (static_cast<std::uint8_t>(byte)) {
        case 'S':
        case '<':
        case 't':
        case 0xFF:
            byte = static_cast<std::byte>(static_cast<std::uint8_t>(byte) - 1);
            break;
        default:
            break;
        }
    }
}

std::vector<std::byte> payload(std::size_t size, std::uint64_t seed) {
    std::vector<std::byte> bytes = makePattern(size, seed);
    sanitize(bytes);
    return bytes;
}

void append(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

void appendLe32(std::vector<std::byte>& out, std::uint32_t value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    storeLe32(out, at, value);
}

bool equalsText(std::span<const std::byte> bytes, std::string_view text) {
    if (bytes.size() != text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(text[i])) {
            return false;
        }
    }
    return true;
}

EndDetection endAt(EndStatus status, std::uint64_t length, std::string detail) {
    return EndDetection{status, length, std::move(detail)};
}

ValidationResult verdict(ValidationStatus status, std::uint64_t validBytes, std::string detail) {
    return ValidationResult{status, validBytes, std::move(detail)};
}

}  // namespace

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> bytes;
    append(bytes, text);
    return bytes;
}

// ---------------------------------------------------------------------------
// SizedFormat
// ---------------------------------------------------------------------------

SizedFormat::SizedFormat(std::uint64_t maximumSize) {
    descriptor_.id = "sized";
    descriptor_.name = "Sized test format";
    descriptor_.extension = "szd";
    descriptor_.signatures = {carving::textSignature("SZD1 magic", "SZD1")};
    descriptor_.minimumSize = kHeaderSize;
    descriptor_.maximumSize = maximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = EndDetectionMethod::SizeField;
}

HeaderCheck SizedFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < kHeaderSize) {
        return HeaderCheck::reject("header cut short");
    }
    if (loadLe32(header, 4) < kHeaderSize) {
        return HeaderCheck::reject("declared length below the header size");
    }
    return HeaderCheck::accept();
}

Result<EndDetection> SizedFormat::findEnd(IContentReader& content) const {
    Result<std::span<const std::byte>> header = content.read(0, kHeaderSize);
    if (!header.ok()) {
        return header.error();
    }
    const std::uint32_t length = loadLe32(*header, 4);
    if (length < kHeaderSize) {
        return endAt(EndStatus::Broken, 0, "declared length below the header size");
    }
    if (length > content.size()) {
        return endAt(EndStatus::Truncated, content.size(),
                     "declares " + std::to_string(length) + " bytes, " + std::to_string(content.size()) + " available");
    }
    return endAt(EndStatus::Found, length, "declared length");
}

Result<ValidationResult> SizedFormat::validate(IContentReader& content) const {
    if (content.size() < kHeaderSize) {
        return verdict(ValidationStatus::Truncated, 0, "header cut short");
    }
    Result<std::span<const std::byte>> header = content.read(0, kHeaderSize);
    if (!header.ok()) {
        return header.error();
    }
    const std::uint32_t length = loadLe32(*header, 4);
    const std::uint32_t expectedCrc = loadLe32(*header, 8);
    if (length < kHeaderSize) {
        return verdict(ValidationStatus::Invalid, 0, "declared length below the header size");
    }
    if (length > content.size()) {
        return verdict(ValidationStatus::Truncated, kHeaderSize, "declared length beyond the content");
    }
    if (length < content.size()) {
        return verdict(ValidationStatus::Invalid, length, "content longer than the declared length");
    }
    std::uint32_t crc = 0;
    for (std::uint64_t position = kHeaderSize; position < length;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, length - position));
        Result<std::span<const std::byte>> chunk = content.read(position, count);
        if (!chunk.ok()) {
            return chunk.error();
        }
        crc = crc32Update(crc, *chunk);
        position += count;
    }
    if (crc != expectedCrc) {
        return verdict(ValidationStatus::Invalid, kHeaderSize, "payload CRC mismatch");
    }
    return verdict(ValidationStatus::Valid, length, "payload CRC matches");
}

std::vector<std::byte> makeSizedFile(std::size_t payloadSize, std::uint64_t seed) {
    return makeSizedFileAround(payload(payloadSize, seed));
}

std::vector<std::byte> makeSizedFileAround(const std::vector<std::byte>& body) {
    std::vector<std::byte> file;
    append(file, "SZD1");
    appendLe32(file, static_cast<std::uint32_t>(SizedFormat::kHeaderSize + body.size()));
    appendLe32(file, crc32(body));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

// ---------------------------------------------------------------------------
// MarkerFormat
// ---------------------------------------------------------------------------

MarkerFormat::MarkerFormat(std::uint64_t maximumSize) {
    descriptor_.id = "marker";
    descriptor_.name = "End-marker test format";
    descriptor_.extension = "mrk";
    descriptor_.signatures = {carving::textSignature("MK magic", "<MK>")};
    descriptor_.minimumSize = kHeaderSize + 5;
    descriptor_.maximumSize = maximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = EndDetectionMethod::EndMarker;
}

HeaderCheck MarkerFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < kHeaderSize || header[4] != std::byte{1}) {
        return HeaderCheck::reject("unsupported version");
    }
    return HeaderCheck::accept();
}

Result<EndDetection> MarkerFormat::findEnd(IContentReader& content) const {
    const std::vector<std::byte> marker = markerEnd();
    Result<std::optional<std::uint64_t>> found = carving::findPattern(content, marker, kHeaderSize, content.size());
    if (!found.ok()) {
        return found.error();
    }
    if (!found->has_value()) {
        return endAt(EndStatus::Truncated, content.size(),
                     "no end marker in the " + std::to_string(content.size()) + " bytes available");
    }
    return endAt(EndStatus::Found, **found + marker.size(), "end marker");
}

Result<ValidationResult> MarkerFormat::validate(IContentReader& content) const {
    const std::uint64_t size = content.size();
    if (size < kHeaderSize) {
        return verdict(ValidationStatus::Truncated, 0, "header cut short");
    }
    bool ended = false;
    if (size >= descriptor_.minimumSize) {
        Result<std::span<const std::byte>> tail = content.read(size - 5, 5);
        if (!tail.ok()) {
            return tail.error();
        }
        ended = equalsText(*tail, "</MK>");
    }
    const std::uint64_t bodyEnd = ended ? size - 5 : size;
    for (std::uint64_t position = kHeaderSize; position < bodyEnd;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, bodyEnd - position));
        Result<std::span<const std::byte>> chunk = content.read(position, count);
        if (!chunk.ok()) {
            return chunk.error();
        }
        const auto zero = std::find(chunk->begin(), chunk->end(), std::byte{0});
        if (zero != chunk->end()) {
            const std::uint64_t at = position + static_cast<std::uint64_t>(zero - chunk->begin());
            return verdict(ValidationStatus::Invalid, at, "zero byte in the body at " + std::to_string(at));
        }
        position += count;
    }
    if (!ended) {
        return verdict(ValidationStatus::Truncated, size, "no end marker");
    }
    return verdict(ValidationStatus::Valid, size, "body and end marker");
}

std::vector<std::byte> markerEnd() {
    return bytesOf("</MK>");
}

std::vector<std::byte> makeMarkerFile(std::size_t bodySize, std::uint64_t seed) {
    std::vector<std::byte> body = payload(bodySize, seed);
    for (std::byte& byte : body) {
        if (byte == std::byte{0}) {
            byte = std::byte{1};
        }
    }
    std::vector<std::byte> file = bytesOf("<MK>");
    file.push_back(std::byte{1});
    file.insert(file.end(), body.begin(), body.end());
    append(file, "</MK>");
    return file;
}

// ---------------------------------------------------------------------------
// BoxFormat
// ---------------------------------------------------------------------------

namespace {

bool isBoxType(std::span<const std::byte> type) {
    return std::all_of(type.begin(), type.end(), [](std::byte b) {
        const auto c = static_cast<std::uint8_t>(b);
        return c >= 'a' && c <= 'z';
    });
}

struct Walk {
    EndStatus status = EndStatus::Unknown;
    // Found: end of the tend box. Otherwise: start of the box that broke or did not fit.
    std::uint64_t position = 0;
    std::string detail;
};

Result<Walk> walkBoxes(IContentReader& content) {
    const std::uint64_t size = content.size();
    std::uint64_t position = 0;
    for (bool first = true;; first = false) {
        if (size - position < 8) {
            return Walk{EndStatus::Truncated, position, "box header cut short at " + std::to_string(position)};
        }
        Result<std::span<const std::byte>> header = content.read(position, 8);
        if (!header.ok()) {
            return header.error();
        }
        const std::uint32_t boxSize = loadLe32(*header, 0);
        const std::span<const std::byte> type = header->subspan(4, 4);
        if (boxSize < 8 || !isBoxType(type) || (first && !equalsText(type, "tbox"))) {
            return Walk{EndStatus::Broken, position, "invalid box at " + std::to_string(position)};
        }
        if (boxSize > size - position) {
            return Walk{EndStatus::Truncated, position, "box at " + std::to_string(position) + " cut short"};
        }
        if (equalsText(type, "tend")) {
            return Walk{EndStatus::Found, position + boxSize, "tend box"};
        }
        position += boxSize;
    }
}

}  // namespace

BoxFormat::BoxFormat(std::uint64_t maximumSize) {
    descriptor_.id = "box";
    descriptor_.name = "Box test format";
    descriptor_.extension = "box";
    descriptor_.signatures = {carving::textSignature("tbox box type", "tbox", 4)};
    descriptor_.minimumSize = 16;
    descriptor_.maximumSize = maximumSize;
    descriptor_.headerSize = 16;
    descriptor_.endDetection = EndDetectionMethod::StructureWalk;
}

HeaderCheck BoxFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < 8 || loadLe32(header, 0) < 8) {
        return HeaderCheck::reject("first box smaller than its header");
    }
    return HeaderCheck::accept();
}

Result<EndDetection> BoxFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkBoxes(content);
    if (!walk.ok()) {
        return walk.error();
    }
    switch (walk->status) {
    case EndStatus::Found:
        return endAt(EndStatus::Found, walk->position, walk->detail);
    case EndStatus::Truncated:
        return endAt(EndStatus::Truncated, content.size(), walk->detail);
    default:
        return endAt(EndStatus::Broken, walk->position, walk->detail);
    }
}

Result<ValidationResult> BoxFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkBoxes(content);
    if (!walk.ok()) {
        return walk.error();
    }
    switch (walk->status) {
    case EndStatus::Found:
        if (walk->position != content.size()) {
            return verdict(ValidationStatus::Invalid, walk->position, "data after the tend box");
        }
        return verdict(ValidationStatus::Valid, walk->position, "boxes up to tend");
    case EndStatus::Truncated:
        return verdict(ValidationStatus::Truncated, walk->position, walk->detail);
    default:
        return verdict(ValidationStatus::Invalid, walk->position, walk->detail);
    }
}

std::vector<std::byte> makeBoxFile(std::size_t firstPayload, const std::vector<std::size_t>& dataPayloads,
                                   std::uint64_t seed) {
    std::vector<std::byte> file;
    const auto addBox = [&](std::string_view type, std::size_t payloadSize) {
        appendLe32(file, static_cast<std::uint32_t>(8 + payloadSize));
        append(file, type);
        const std::vector<std::byte> body = payload(payloadSize, seed++);
        file.insert(file.end(), body.begin(), body.end());
    };
    addBox("tbox", firstPayload);
    for (const std::size_t size : dataPayloads) {
        addBox("data", size);
    }
    addBox("tend", 0);
    return file;
}

// ---------------------------------------------------------------------------
// SyncFormat
// ---------------------------------------------------------------------------

SyncFormat::SyncFormat() {
    descriptor_.id = "sync";
    descriptor_.name = "Frame-sync test format";
    descriptor_.extension = "syn";
    descriptor_.signatures = {carving::byteSignature("frame sync", {0xFF, 0xE0}, 0, {0xFF, 0xE0})};
    descriptor_.minimumSize = 64;
    descriptor_.maximumSize = 1024 * 1024;
    descriptor_.headerSize = 4;
    descriptor_.endDetection = EndDetectionMethod::None;
}

HeaderCheck SyncFormat::checkHeader(std::span<const std::byte> /*header*/) const {
    return HeaderCheck::accept();
}

Result<EndDetection> SyncFormat::findEnd(IContentReader& content) const {
    return endAt(EndStatus::Unknown, std::min(content.size(), kEstimate), "no end information");
}

Result<ValidationResult> SyncFormat::validate(IContentReader& content) const {
    return verdict(ValidationStatus::Valid, content.size(), "nothing to check");
}

// ---------------------------------------------------------------------------
// ScriptedFormat
// ---------------------------------------------------------------------------

ScriptedFormat::ScriptedFormat(FormatDescriptor descriptor) : descriptor_(std::move(descriptor)) {}

ScriptedFormat& ScriptedFormat::onHeader(HeaderStep step) {
    header_ = std::move(step);
    return *this;
}

ScriptedFormat& ScriptedFormat::onEnd(EndStep step) {
    end_ = std::move(step);
    return *this;
}

ScriptedFormat& ScriptedFormat::onValidate(ValidateStep step) {
    validate_ = std::move(step);
    return *this;
}

HeaderCheck ScriptedFormat::checkHeader(std::span<const std::byte> header) const {
    return header_ ? header_(header) : HeaderCheck::accept();
}

Result<EndDetection> ScriptedFormat::findEnd(IContentReader& content) const {
    return end_ ? end_(content) : endAt(EndStatus::Found, content.size(), "scripted");
}

Result<ValidationResult> ScriptedFormat::validate(IContentReader& content) const {
    return validate_ ? validate_(content) : verdict(ValidationStatus::Valid, content.size(), "scripted");
}

FormatDescriptor scriptedDescriptor(std::string id, std::string_view magic, std::uint64_t maximumSize,
                                    std::uint32_t offset) {
    FormatDescriptor descriptor;
    descriptor.name = "Scripted " + id;
    descriptor.signatures = {carving::textSignature(id + " magic", magic, offset)};
    descriptor.id = std::move(id);
    descriptor.extension = "bin";
    const std::size_t reach = descriptor.signatures.front().reach();
    descriptor.minimumSize = std::max<std::uint64_t>(reach, 16);
    descriptor.maximumSize = maximumSize;
    descriptor.headerSize = static_cast<std::uint32_t>(std::max<std::size_t>(reach, 16));
    descriptor.endDetection = EndDetectionMethod::StructureWalk;
    return descriptor;
}

}  // namespace recovery::test
