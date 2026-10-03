#include "formats/jpeg_format.hpp"

#include "recovery/byte_order.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using detail::hexByte;
using detail::Walk;
using detail::WalkStatus;

constexpr std::uint8_t kTem = 0x01;
constexpr std::uint8_t kDht = 0xC4;
constexpr std::uint8_t kJpg = 0xC8;
constexpr std::uint8_t kDac = 0xCC;
constexpr std::uint8_t kRst0 = 0xD0;
constexpr std::uint8_t kRst7 = 0xD7;
constexpr std::uint8_t kSoi = 0xD8;
constexpr std::uint8_t kEoi = 0xD9;
constexpr std::uint8_t kSos = 0xDA;
constexpr std::uint8_t kDqt = 0xDB;
constexpr std::uint8_t kDnl = 0xDC;
constexpr std::uint8_t kDri = 0xDD;
constexpr std::uint8_t kDhp = 0xDE;
constexpr std::uint8_t kExp = 0xDF;
constexpr std::uint8_t kCom = 0xFE;

// Entropy-coded data is searched for markers in pieces of this size.
constexpr std::size_t kScanChunk = 64 * kKiB;
// Largest number of blocks in one MCU of an interleaved DCT scan (T.81 B.2.3).
constexpr std::uint32_t kMaxBlocksPerMcu = 10;

bool isFrameMarker(std::uint8_t code) noexcept {
    return code >= 0xC0 && code <= 0xCF && code != kDht && code != kJpg && code != kDac;
}

bool isAppMarker(std::uint8_t code) noexcept {
    return code >= 0xE0 && code <= 0xEF;
}

bool isRestartMarker(std::uint8_t code) noexcept {
    return code >= kRst0 && code <= kRst7;
}

// Codes that are not markers at all (FF 00 is a stuffed byte) or that T.81
// reserves (RES): never part of a valid file.
bool isReservedMarker(std::uint8_t code) noexcept {
    return code == 0x00 || (code >= 0x02 && code <= 0xBF);
}

// Codes that may follow SOI directly in a file some encoder wrote.
bool canStartImage(std::uint8_t code) noexcept {
    return isAppMarker(code) || isFrameMarker(code) || code == kDqt || code == kDht || code == kCom ||
           code == kDri || code == kDac;
}

bool isProgressive(std::uint8_t frame) noexcept {
    return frame == 0xC2 || frame == 0xC6 || frame == 0xCA || frame == 0xCE;
}

bool isLossless(std::uint8_t frame) noexcept {
    return frame == 0xC3 || frame == 0xC7 || frame == 0xCB || frame == 0xCF;
}

std::string markerName(std::uint8_t code) {
    if (isFrameMarker(code)) {
        return "SOF" + std::to_string(code - 0xC0);
    }
    if (isAppMarker(code)) {
        return "APP" + std::to_string(code - 0xE0);
    }
    switch (code) {
    case kDht:
        return "DHT";
    case kDac:
        return "DAC";
    case kSos:
        return "SOS";
    case kDqt:
        return "DQT";
    case kDnl:
        return "DNL";
    case kDri:
        return "DRI";
    case kDhp:
        return "DHP";
    case kExp:
        return "EXP";
    case kCom:
        return "COM";
    default:
        return "marker " + hexByte(code);
    }
}

std::string plural(std::uint64_t count, std::string_view noun) {
    return std::to_string(count) + " " + std::string(noun) + (count == 1 ? "" : "s");
}

std::uint64_t ceilDiv(std::uint64_t value, std::uint64_t divisor) noexcept {
    return (value + divisor - 1) / divisor;
}

struct Component {
    std::uint8_t id = 0;
    std::uint8_t h = 1;
    std::uint8_t v = 1;
    std::uint8_t quantizationTable = 0;
};

struct Frame {
    std::uint8_t marker = 0;
    std::uint8_t precision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Component> components;
    std::uint8_t hMax = 1;
    std::uint8_t vMax = 1;
    // Every sampling factor is in 1..4, so MCU sizes can be computed.
    bool samplingValid = false;

    [[nodiscard]] const Component* find(std::uint8_t id) const noexcept {
        for (const Component& component : components) {
            if (component.id == id) {
                return &component;
            }
        }
        return nullptr;
    }
};

struct Marker {
    // Offset of the marker's first 0xFF (fill bytes included).
    std::uint64_t start = 0;
    std::uint8_t code = 0;
    // First byte after the marker code.
    std::uint64_t next = 0;
};

// Walks a JPEG from SOI to EOI. See structure_walk.hpp for what a walk reports.
class JpegWalker {
public:
    explicit JpegWalker(IContentReader& content) : content_(content) {}

    Result<Walk> run();

private:
    Result<std::optional<std::uint8_t>> byteAt(std::uint64_t offset);
    // The marker at `position`, where the structure needs one; nullopt when the walk ended.
    Result<std::optional<Marker>> markerAt(std::uint64_t position);
    // Scans entropy-coded data from `start` to the marker that ends it; nullopt when the walk ended.
    Result<std::optional<std::uint64_t>> scanEntropyData(std::uint64_t start, std::uint64_t scanStart,
                                                         const std::vector<std::uint8_t>& scanComponents);

    void frameHeader(const Marker& marker, std::span<const std::byte> payload);
    void scanHeader(const Marker& marker, std::span<const std::byte> payload,
                    std::vector<std::uint8_t>& scanComponents);
    void quantizationTables(const Marker& marker, std::span<const std::byte> payload);
    void huffmanTables(const Marker& marker, std::span<const std::byte> payload);
    void checkRestartCount(std::uint64_t scanStart, const std::vector<std::uint8_t>& scanComponents,
                           std::uint32_t restarts);

    std::string summary() const;

    IContentReader& content_;
    Walk walk_;
    std::optional<Frame> frame_;
    bool hierarchical_ = false;
    std::uint32_t scans_ = 0;
    std::uint16_t restartInterval_ = 0;
    std::array<bool, 4> quantizationDefined_{};
};

Result<std::optional<std::uint8_t>> JpegWalker::byteAt(std::uint64_t offset) {
    Result<std::optional<std::span<const std::byte>>> bytes = detail::readIfAvailable(content_, offset, 1);
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return std::optional<std::uint8_t>{};
    }
    return std::optional<std::uint8_t>(static_cast<std::uint8_t>((**bytes)[0]));
}

Result<std::optional<Marker>> JpegWalker::markerAt(std::uint64_t position) {
    Result<std::optional<std::uint8_t>> first = byteAt(position);
    if (!first.ok()) {
        return first.error();
    }
    if (!first->has_value()) {
        walk_.finish(WalkStatus::Truncated, position, "the data ends before the next marker");
        return std::optional<Marker>{};
    }
    if (**first != 0xFF) {
        walk_.finish(WalkStatus::Broken, position, "no marker where the structure needs one");
        return std::optional<Marker>{};
    }
    // Any marker may be preceded by 0xFF fill bytes.
    std::uint64_t offset = position + 1;
    for (;;) {
        Result<std::optional<std::uint8_t>> code = byteAt(offset);
        if (!code.ok()) {
            return code.error();
        }
        if (!code->has_value()) {
            walk_.finish(WalkStatus::Truncated, position, "the data ends inside a marker");
            return std::optional<Marker>{};
        }
        if (**code != 0xFF) {
            return std::optional<Marker>(Marker{position, **code, offset + 1});
        }
        ++offset;
        if (offset - position > JpegFormat::kMaxFillBytes) {
            walk_.finish(WalkStatus::Broken, position,
                         "more than " + std::to_string(JpegFormat::kMaxFillBytes) + " fill bytes: erased data");
            return std::optional<Marker>{};
        }
    }
}

void JpegWalker::frameHeader(const Marker& marker, std::span<const std::byte> payload) {
    const bool isDhp = marker.code == kDhp;
    if (isDhp) {
        if (frame_.has_value() || hierarchical_) {
            walk_.noteProblem(marker.start, "DHP after the image's first frame header");
        }
        hierarchical_ = true;
    }
    // A second frame header without DHP ends the walk before this (run()).
    Frame frame;
    frame.marker = marker.code;
    if (payload.size() < 6) {
        walk_.noteProblem(marker.start, markerName(marker.code) + " is too short");
        if (!isDhp) {
            frame_ = std::move(frame);
        }
        return;
    }
    frame.precision = loadU8(payload, 0);
    frame.height = loadBe16(payload, 1);
    frame.width = loadBe16(payload, 3);
    const std::size_t count = loadU8(payload, 5);
    const std::string name = markerName(marker.code);
    if (payload.size() != 6 + 3 * count) {
        walk_.noteProblem(marker.start, name + " length does not match its " + plural(count, "component"));
    }
    if (count == 0) {
        walk_.noteProblem(marker.start, name + " has no components");
    }
    if (isProgressive(marker.code) && count > 4) {
        walk_.noteProblem(marker.start, "progressive frame with " + plural(count, "component"));
    }
    if (frame.width == 0) {
        walk_.noteProblem(marker.start, name + " has a width of 0");
    }
    const bool lossless = isLossless(marker.code);
    const bool precisionValid = lossless                ? frame.precision >= 2 && frame.precision <= 16
                                : marker.code == 0xC0   ? frame.precision == 8
                                                        : frame.precision == 8 || frame.precision == 12;
    if (!precisionValid) {
        walk_.noteProblem(marker.start, name + " has a sample precision of " + std::to_string(frame.precision));
    }
    frame.samplingValid = count > 0;
    for (std::size_t i = 0; i < count && 6 + 3 * i + 3 <= payload.size(); ++i) {
        Component component;
        component.id = loadU8(payload, 6 + 3 * i);
        const std::uint8_t sampling = loadU8(payload, 6 + 3 * i + 1);
        component.h = static_cast<std::uint8_t>(sampling >> 4);
        component.v = static_cast<std::uint8_t>(sampling & 0x0F);
        component.quantizationTable = loadU8(payload, 6 + 3 * i + 2);
        if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4) {
            walk_.noteProblem(marker.start, name + " component " + std::to_string(component.id) +
                                                " has sampling factors outside 1..4");
            frame.samplingValid = false;
        }
        if (component.quantizationTable > 3) {
            walk_.noteProblem(marker.start, name + " component " + std::to_string(component.id) +
                                                " names quantization table " +
                                                std::to_string(component.quantizationTable));
        }
        if (frame.find(component.id) != nullptr) {
            walk_.noteProblem(marker.start, name + " repeats component " + std::to_string(component.id));
        }
        frame.hMax = std::max(frame.hMax, component.h);
        frame.vMax = std::max(frame.vMax, component.v);
        frame.components.push_back(component);
    }
    if (!isDhp) {
        frame_ = std::move(frame);
    }
}

void JpegWalker::scanHeader(const Marker& marker, std::span<const std::byte> payload,
                            std::vector<std::uint8_t>& scanComponents) {
    scanComponents.clear();
    if (!frame_.has_value()) {
        walk_.noteProblem(marker.start, "SOS before any frame header");
    }
    if (payload.empty()) {
        walk_.noteProblem(marker.start, "SOS is too short");
        return;
    }
    const std::size_t count = loadU8(payload, 0);
    if (count < 1 || count > 4 || payload.size() != 1 + 2 * count + 3) {
        walk_.noteProblem(marker.start, "SOS length does not match its " + plural(count, "component"));
        return;
    }
    std::uint32_t blocksPerMcu = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t id = loadU8(payload, 1 + 2 * i);
        const std::uint8_t tables = loadU8(payload, 2 + 2 * i);
        if (std::find(scanComponents.begin(), scanComponents.end(), id) != scanComponents.end()) {
            walk_.noteProblem(marker.start, "SOS repeats component " + std::to_string(id));
        }
        scanComponents.push_back(id);
        if ((tables >> 4) > 3 || (tables & 0x0F) > 3) {
            walk_.noteProblem(marker.start, "SOS names an entropy table beyond 3");
        }
        if (!frame_.has_value()) {
            continue;
        }
        const Component* component = frame_->find(id);
        if (component == nullptr) {
            walk_.noteProblem(marker.start, "SOS names component " + std::to_string(id) + ", which the frame lacks");
            continue;
        }
        blocksPerMcu += static_cast<std::uint32_t>(component->h) * component->v;
        if (!isLossless(frame_->marker) && component->quantizationTable < quantizationDefined_.size() &&
            !quantizationDefined_[component->quantizationTable]) {
            walk_.noteProblem(marker.start, "component " + std::to_string(id) + " uses quantization table " +
                                                std::to_string(component->quantizationTable) +
                                                ", which is not defined");
        }
    }
    const std::uint8_t ss = loadU8(payload, 1 + 2 * count);
    const std::uint8_t se = loadU8(payload, 2 + 2 * count);
    const std::uint8_t ah = static_cast<std::uint8_t>(loadU8(payload, 3 + 2 * count) >> 4);
    const std::uint8_t al = static_cast<std::uint8_t>(loadU8(payload, 3 + 2 * count) & 0x0F);
    if (!frame_.has_value()) {
        return;
    }
    const std::uint8_t frame = frame_->marker;
    if (isLossless(frame)) {
        if (ss > 7 || ah != 0) {
            walk_.noteProblem(marker.start, "SOS parameters are invalid for a lossless frame");
        }
        return;
    }
    if (count > 1 && blocksPerMcu > kMaxBlocksPerMcu) {
        walk_.noteProblem(marker.start, "an MCU of " + plural(blocksPerMcu, "block") + " (at most 10)");
    }
    if (isProgressive(frame)) {
        const bool dcScan = ss == 0;
        const bool valid = dcScan ? se == 0 : count == 1 && se >= ss && se <= 63;
        if (!valid || ah > 13 || al > 13) {
            walk_.noteProblem(marker.start, "SOS spectral selection or approximation is invalid for a "
                                            "progressive frame");
        }
    } else if (ss > 63 || se > 63) {
        walk_.noteProblem(marker.start, "SOS spectral selection beyond 63");
    }
}

void JpegWalker::quantizationTables(const Marker& marker, std::span<const std::byte> payload) {
    if (payload.empty()) {
        walk_.noteProblem(marker.start, "DQT defines no table");
    }
    std::size_t i = 0;
    while (i < payload.size()) {
        const std::uint8_t info = loadU8(payload, i);
        const std::uint8_t precision = static_cast<std::uint8_t>(info >> 4);
        const std::uint8_t table = static_cast<std::uint8_t>(info & 0x0F);
        if (precision > 1 || table > 3) {
            walk_.noteProblem(marker.start, "DQT table with precision " + std::to_string(precision) + " and id " +
                                                std::to_string(table));
            return;
        }
        const std::size_t size = 64 * (static_cast<std::size_t>(precision) + 1);
        if (payload.size() - i - 1 < size) {
            walk_.noteProblem(marker.start, "DQT table " + std::to_string(table) + " runs beyond its segment");
            return;
        }
        quantizationDefined_[table] = true;
        i += 1 + size;
    }
}

void JpegWalker::huffmanTables(const Marker& marker, std::span<const std::byte> payload) {
    if (payload.empty()) {
        walk_.noteProblem(marker.start, "DHT defines no table");
    }
    std::size_t i = 0;
    while (i < payload.size()) {
        const std::uint8_t info = loadU8(payload, i);
        const std::uint8_t tableClass = static_cast<std::uint8_t>(info >> 4);
        const std::uint8_t table = static_cast<std::uint8_t>(info & 0x0F);
        if (tableClass > 1 || table > 3) {
            walk_.noteProblem(marker.start, "DHT table with class " + std::to_string(tableClass) + " and id " +
                                                std::to_string(table));
            return;
        }
        if (payload.size() - i < 17) {
            walk_.noteProblem(marker.start, "DHT table runs beyond its segment");
            return;
        }
        // Canonical codes of each length; as in libjpeg, a length whose codes
        // would include the all-ones code (or run out) makes the table invalid.
        std::size_t total = 0;
        std::uint32_t code = 0;
        bool codesValid = true;
        for (std::size_t length = 1; length <= 16; ++length) {
            const std::uint8_t count = loadU8(payload, i + length);
            total += count;
            code += count;
            if (code >= (1U << length)) {
                codesValid = false;
            }
            code <<= 1;
        }
        if (total > 256 || !codesValid) {
            walk_.noteProblem(marker.start, "DHT table " + std::to_string(table) + " has impossible code lengths");
            return;
        }
        if (payload.size() - i - 17 < total) {
            walk_.noteProblem(marker.start, "DHT table " + std::to_string(table) + " runs beyond its segment");
            return;
        }
        if (tableClass == 0) {
            for (std::size_t k = 0; k < total; ++k) {
                if (loadU8(payload, i + 17 + k) > 16) {
                    walk_.noteProblem(marker.start, "DHT DC table " + std::to_string(table) +
                                                        " has a symbol beyond 16");
                    return;
                }
            }
        }
        i += 17 + total;
    }
}

void JpegWalker::checkRestartCount(std::uint64_t scanStart, const std::vector<std::uint8_t>& scanComponents,
                                   std::uint32_t restarts) {
    if (restartInterval_ == 0 || !frame_.has_value() || hierarchical_ || isLossless(frame_->marker) ||
        !frame_->samplingValid || frame_->height == 0 || frame_->width == 0 || scanComponents.empty()) {
        return;
    }
    const Frame& frame = *frame_;
    std::uint64_t mcus = 0;
    if (scanComponents.size() == 1) {
        // A non-interleaved scan codes the component's own blocks, one per MCU.
        const Component* component = frame.find(scanComponents.front());
        if (component == nullptr) {
            return;
        }
        const std::uint64_t width = ceilDiv(std::uint64_t{frame.width} * component->h, frame.hMax);
        const std::uint64_t height = ceilDiv(std::uint64_t{frame.height} * component->v, frame.vMax);
        mcus = ceilDiv(width, 8) * ceilDiv(height, 8);
    } else {
        mcus = ceilDiv(frame.width, 8ULL * frame.hMax) * ceilDiv(frame.height, 8ULL * frame.vMax);
    }
    const std::uint64_t expected = ceilDiv(mcus, restartInterval_) - 1;
    if (restarts != expected) {
        walk_.noteProblem(scanStart, "the scan has " + plural(restarts, "restart marker") + "; its " +
                                         plural(mcus, "MCU") + " in intervals of " +
                                         std::to_string(restartInterval_) + " need " + std::to_string(expected));
    }
}

Result<std::optional<std::uint64_t>> JpegWalker::scanEntropyData(std::uint64_t start, std::uint64_t scanStart,
                                                                 const std::vector<std::uint8_t>& scanComponents) {
    const std::uint64_t size = content_.size();
    std::uint64_t position = start;
    std::uint32_t restarts = 0;
    for (;;) {
        if (position >= size) {
            walk_.finish(WalkStatus::Truncated, size, "the data ends inside entropy-coded data");
            return std::optional<std::uint64_t>{};
        }
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kScanChunk, size - position));
        Result<std::span<const std::byte>> chunk = content_.read(position, length);
        if (!chunk.ok()) {
            return chunk.error();
        }
        const void* found = std::memchr(chunk->data(), 0xFF, length);
        if (found == nullptr) {
            position += length;
            continue;
        }
        const std::uint64_t ff = position + static_cast<std::uint64_t>(static_cast<const std::byte*>(found) -
                                                                       chunk->data());
        // The byte after the 0xFF (and after any fill bytes) decides what it is.
        std::uint64_t offset = ff + 1;
        std::uint8_t code = 0;
        for (;;) {
            Result<std::optional<std::uint8_t>> next = byteAt(offset);
            if (!next.ok()) {
                return next.error();
            }
            if (!next->has_value()) {
                walk_.finish(WalkStatus::Truncated, size, "the data ends inside entropy-coded data");
                return std::optional<std::uint64_t>{};
            }
            if (**next != 0xFF) {
                code = **next;
                break;
            }
            ++offset;
            if (offset - ff > JpegFormat::kMaxFillBytes) {
                walk_.finish(WalkStatus::Broken, ff,
                             "more than " + std::to_string(JpegFormat::kMaxFillBytes) + " fill bytes: erased data");
                return std::optional<std::uint64_t>{};
            }
        }
        if (code == 0x00) {
            // A stuffed 0xFF data byte.
            position = offset + 1;
            continue;
        }
        if (isRestartMarker(code)) {
            if (restartInterval_ == 0) {
                walk_.finish(WalkStatus::Broken, ff, "a restart marker in a scan without a restart interval");
                return std::optional<std::uint64_t>{};
            }
            const std::uint32_t expected = restarts % 8;
            if (static_cast<std::uint32_t>(code - kRst0) != expected) {
                walk_.finish(WalkStatus::Broken, ff,
                             "restart marker RST" + std::to_string(code - kRst0) + " where RST" +
                                 std::to_string(expected) + " belongs");
                return std::optional<std::uint64_t>{};
            }
            ++restarts;
            position = offset + 1;
            continue;
        }
        // Any other marker ends the entropy-coded data.
        if (ff == start) {
            walk_.noteProblem(scanStart, "a scan without entropy-coded data");
        }
        checkRestartCount(scanStart, scanComponents, restarts);
        return std::optional<std::uint64_t>(ff);
    }
}

std::string JpegWalker::summary() const {
    std::string text;
    if (frame_.has_value()) {
        text = markerName(frame_->marker) + " " + std::to_string(frame_->width) + "x" +
               std::to_string(frame_->height) + ", " + plural(frame_->components.size(), "component") + ", ";
    }
    return text + plural(scans_, "scan") + ", EOI";
}

Result<Walk> JpegWalker::run() {
    Result<std::optional<std::span<const std::byte>>> soi = detail::readIfAvailable(content_, 0, 2);
    if (!soi.ok()) {
        return soi.error();
    }
    if (!soi->has_value()) {
        return walk_.finish(WalkStatus::Truncated, 0, "the data ends inside SOI");
    }
    if (loadU8(**soi, 0) != 0xFF || loadU8(**soi, 1) != kSoi) {
        return walk_.finish(WalkStatus::Broken, 0, "the file does not start with SOI");
    }
    std::uint64_t position = 2;
    std::vector<std::uint8_t> scanComponents;
    for (;;) {
        Result<std::optional<Marker>> read = markerAt(position);
        if (!read.ok()) {
            return read.error();
        }
        if (!read->has_value()) {
            return walk_;
        }
        const Marker marker = **read;
        const std::uint8_t code = marker.code;
        if (code == kEoi) {
            if (!frame_.has_value()) {
                walk_.noteProblem(marker.start, "EOI before any frame header");
            } else if (scans_ == 0) {
                walk_.noteProblem(marker.start, "EOI before any scan");
            }
            return walk_.finish(WalkStatus::Complete, marker.next, summary());
        }
        if (code == kSoi) {
            return walk_.finish(WalkStatus::Broken, marker.start, "SOI inside the image: another file starts here");
        }
        if (isRestartMarker(code) || code == kTem) {
            // Markers without parameters; decoders pass over them between segments.
            position = marker.next;
            continue;
        }
        if (isReservedMarker(code)) {
            return walk_.finish(WalkStatus::Broken, marker.start, "reserved marker " + hexByte(code));
        }
        if (isFrameMarker(code) && frame_.has_value() && !hierarchical_) {
            // A second image's frame header: this image's data ended before
            // it, whatever the bytes behind the marker say.
            return walk_.finish(WalkStatus::Broken, marker.start,
                                "a second frame header (" + markerName(code) + ") in a non-hierarchical image");
        }

        // Every other marker starts a segment with a 16-bit length (which counts itself).
        Result<std::optional<std::span<const std::byte>>> lengthField =
            detail::readIfAvailable(content_, marker.next, 2);
        if (!lengthField.ok()) {
            return lengthField.error();
        }
        if (!lengthField->has_value()) {
            return walk_.finish(WalkStatus::Truncated, marker.start, "the data ends inside a segment header");
        }
        const std::uint16_t length = loadBe16(**lengthField, 0);
        if (length < 2) {
            return walk_.finish(WalkStatus::Broken, marker.start,
                                markerName(code) + " segment length " + std::to_string(length) + " is below 2");
        }
        const std::uint64_t segmentEnd = marker.next + length;
        if (segmentEnd > content_.size()) {
            return walk_.finish(WalkStatus::Truncated, marker.start,
                                "the data ends inside the " + markerName(code) + " segment");
        }
        const bool parsed = isFrameMarker(code) || code == kDhp || code == kSos || code == kDqt || code == kDht ||
                            code == kDri || code == kDnl || code == kExp;
        std::span<const std::byte> payload;
        if (parsed) {
            Result<std::span<const std::byte>> bytes = content_.read(marker.next + 2, length - 2U);
            if (!bytes.ok()) {
                return bytes.error();
            }
            payload = *bytes;
        }

        if (isFrameMarker(code) || code == kDhp) {
            frameHeader(marker, payload);
        } else if (code == kSos) {
            scanHeader(marker, payload, scanComponents);
            ++scans_;
            Result<std::optional<std::uint64_t>> dataEnd = scanEntropyData(segmentEnd, marker.start, scanComponents);
            if (!dataEnd.ok()) {
                return dataEnd.error();
            }
            if (!dataEnd->has_value()) {
                return walk_;
            }
            position = **dataEnd;
            continue;
        } else if (code == kDqt) {
            quantizationTables(marker, payload);
        } else if (code == kDht) {
            huffmanTables(marker, payload);
        } else if (code == kDri) {
            if (payload.size() != 2) {
                walk_.noteProblem(marker.start, "DRI length is not 4");
            } else {
                restartInterval_ = loadBe16(payload, 0);
            }
        } else if (code == kDnl) {
            if (payload.size() != 2 || scans_ == 0) {
                walk_.noteProblem(marker.start, "DNL is misplaced or has the wrong length");
            } else if (frame_.has_value() && frame_->height == 0) {
                frame_->height = loadBe16(payload, 0);
            }
        } else if (code == kExp) {
            if (payload.size() != 1 || !hierarchical_) {
                walk_.noteProblem(marker.start, "EXP outside a hierarchical image or with the wrong length");
            }
        } else if (code == kJpg || (code >= 0xF0 && code <= 0xFD)) {
            walk_.noteProblem(marker.start, "marker " + hexByte(code) + " is reserved for JPEG extensions");
        }
        // APPn, COM and DAC carry nothing the structure depends on.
        position = segmentEnd;
    }
}

Result<Walk> walkJpeg(IContentReader& content) {
    return JpegWalker(content).run();
}

}  // namespace

JpegFormat::JpegFormat() {
    descriptor_.id = "jpeg";
    descriptor_.name = "JPEG image";
    descriptor_.extension = "jpg";
    descriptor_.signatures = {carving::byteSignature("JPEG SOI", {0xFF, 0xD8, 0xFF})};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
}

HeaderCheck JpegFormat::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < 4 || loadU8(header, 0) != 0xFF || loadU8(header, 1) != kSoi) {
        return HeaderCheck::reject("no SOI marker");
    }
    // Follow the chain of segments as far as the header holds it.
    std::size_t position = 2;
    bool first = true;
    while (position < header.size()) {
        if (loadU8(header, position) != 0xFF) {
            return HeaderCheck::reject(first ? "no marker after SOI" : "the segment chain breaks");
        }
        std::size_t offset = position + 1;
        while (offset < header.size() && loadU8(header, offset) == 0xFF) {
            ++offset;
        }
        if (offset >= header.size()) {
            break;
        }
        const std::uint8_t code = loadU8(header, offset);
        if (first && !canStartImage(code)) {
            return HeaderCheck::reject("marker " + hexByte(code) + " cannot follow SOI");
        }
        if (code == kSoi || code == kEoi || isReservedMarker(code)) {
            return HeaderCheck::reject("marker " + hexByte(code) + " in the header");
        }
        if (isRestartMarker(code) || code == kTem) {
            position = offset + 1;
            first = false;
            continue;
        }
        if (header.size() - offset < 3) {
            break;
        }
        const std::uint16_t length = loadBe16(header, offset + 1);
        if (length < 2) {
            return HeaderCheck::reject(markerName(code) + " segment length below 2");
        }
        if (code == kSos) {
            break;
        }
        position = offset + 1 + length;
        first = false;
    }
    return HeaderCheck::accept();
}

Result<EndDetection> JpegFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkJpeg(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> JpegFormat::validate(IContentReader& content) const {
    Result<Walk> walk = walkJpeg(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::verdictOf(*walk, content.size());
}

}  // namespace recovery::formats
