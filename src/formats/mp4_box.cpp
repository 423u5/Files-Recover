#include "formats/mp4_box.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace recovery::formats::mp4 {

namespace {

constexpr std::size_t kCompactHeader = 8;
constexpr std::size_t kLargeHeader = 16;
constexpr std::size_t kUserTypeSize = 16;

constexpr std::array<FourCc, 19> kTopLevelTypes = {
    box::kFtyp,     box::kMoov,     box::kMdat,     box::kFree,     box::kSkip,     box::kWide,     box::kUuid,
    FourCc("meta"), FourCc("udta"), FourCc("pdin"), box::kMoof,     FourCc("mfra"), FourCc("styp"), FourCc("sidx"),
    FourCc("ssix"), FourCc("prft"), FourCc("emsg"), FourCc("meco"), FourCc("pnot")};

// Whether `bytes`, fewer than a whole box header and cut off by the end of
// the content, could be the start of one: a size of 0, 1 or at least 8, and
// printable type bytes as far as there are any.
bool couldStartBox(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() >= 4) {
        const std::uint32_t size = loadBe32(bytes, 0);
        if (size != 0 && size != 1 && size < kCompactHeader) {
            return false;
        }
    }
    for (std::size_t i = 4; i < std::min<std::size_t>(bytes.size(), kCompactHeader); ++i) {
        const auto c = static_cast<std::uint8_t>(bytes[i]);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

}  // namespace

FourCc FourCc::at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return FourCc(loadBe32(bytes, offset));
}

std::string FourCc::text() const {
    if (isPrintable()) {
        std::string text(4, ' ');
        for (std::size_t i = 0; i < 4; ++i) {
            text[i] = static_cast<char>((value_ >> (24 - 8 * i)) & 0xFF);
        }
        return text;
    }
    constexpr std::string_view kDigits = "0123456789ABCDEF";
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text += kDigits[(value_ >> shift) & 0xF];
    }
    return text;
}

bool isTopLevelType(FourCc type) noexcept {
    return std::find(kTopLevelTypes.begin(), kTopLevelTypes.end(), type) != kTopLevelTypes.end();
}

BoxRead parseBoxHeader(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t limit) {
    BoxRead read;
    BoxHeader& header = read.header;
    header.offset = offset;
    const std::uint64_t room = limit >= offset ? limit - offset : 0;
    if (bytes.size() > room) {
        bytes = bytes.first(static_cast<std::size_t>(room));
    }
    if (bytes.size() < kCompactHeader) {
        read.status = BoxStatus::HeaderCut;
        return read;
    }
    header.type = FourCc::at(bytes, 4);
    const std::uint32_t compact = loadBe32(bytes, 0);
    std::uint64_t size = compact;
    std::uint32_t headerSize = kCompactHeader;
    if (compact == 0) {
        header.sizeKind = BoxSize::ToEnd;
    } else if (compact == 1) {
        header.sizeKind = BoxSize::Large;
        if (bytes.size() < kLargeHeader) {
            read.status = BoxStatus::HeaderCut;
            return read;
        }
        size = loadBe64(bytes, kCompactHeader);
        headerSize = kLargeHeader;
    }
    header.size = size;
    header.headerSize = headerSize;
    if (header.sizeKind != BoxSize::ToEnd && size < headerSize) {
        read.status = BoxStatus::BadSize;
        return read;
    }
    if (header.type == box::kUuid) {
        if (header.sizeKind != BoxSize::ToEnd && size < headerSize + kUserTypeSize) {
            read.status = BoxStatus::BadSize;
            return read;
        }
        if (bytes.size() < headerSize + kUserTypeSize) {
            read.status = BoxStatus::HeaderCut;
            return read;
        }
        std::copy_n(bytes.begin() + headerSize, kUserTypeSize, header.userType.begin());
        headerSize += kUserTypeSize;
        header.headerSize = headerSize;
    }
    if (header.sizeKind == BoxSize::ToEnd) {
        // The header fitted, so the rest of the bounds holds at least the header.
        header.size = room;
    }
    if (header.size > room) {
        read.status = BoxStatus::TooLong;
        return read;
    }
    return read;
}

Result<BoxRead> readBoxHeader(carving::IContentReader& content, std::uint64_t offset, std::uint64_t limit) {
    limit = std::min(limit, content.size());
    if (offset > limit) {
        return makeError(ErrorCode::InvalidInput, "box header offset " + std::to_string(offset) +
                                                      " is beyond its bounds (" + std::to_string(limit) + ")");
    }
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(limit - offset, BoxHeader::kMaxSize));
    if (length == 0) {
        return parseBoxHeader({}, offset, limit);
    }
    Result<std::span<const std::byte>> bytes = content.read(offset, length);
    if (!bytes.ok()) {
        return bytes.error();
    }
    return parseBoxHeader(*bytes, offset, limit);
}

BoxSequence::BoxSequence(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end) noexcept
    : content_(content), position_(begin), end_(std::min(end, content.size())), finished_(begin > end_) {}

Result<std::optional<BoxRead>> BoxSequence::next() {
    if (finished_ || position_ >= end_) {
        finished_ = true;
        return std::optional<BoxRead>{};
    }
    Result<BoxRead> read = readBoxHeader(content_, position_, end_);
    if (!read.ok()) {
        return read.error();
    }
    if (read->status != BoxStatus::Valid) {
        finished_ = true;
        return std::optional<BoxRead>(*read);
    }
    position_ = read->header.end();
    if (read->header.sizeKind == BoxSize::ToEnd) {
        finished_ = true;
    }
    return std::optional<BoxRead>(*read);
}

const BoxHeader* Layout::first(FourCc type) const noexcept {
    const auto found = std::find_if(boxes.begin(), boxes.end(), [&](const BoxHeader& b) { return b.type == type; });
    return found == boxes.end() ? nullptr : &*found;
}

std::vector<BoxHeader> Layout::all(FourCc type) const {
    std::vector<BoxHeader> matching;
    std::copy_if(boxes.begin(), boxes.end(), std::back_inserter(matching),
                 [&](const BoxHeader& b) { return b.type == type; });
    return matching;
}

Result<Layout> scanTopLevel(carving::IContentReader& content, std::uint64_t start, std::uint64_t maxBoxes) {
    const std::uint64_t size = content.size();
    if (start > size) {
        return makeError(ErrorCode::InvalidInput, "the scan starts beyond the content");
    }
    if (maxBoxes == 0) {
        return makeError(ErrorCode::InvalidInput, "maxBoxes must not be 0");
    }
    Layout layout;
    BoxSequence sequence(content, start, size);
    const auto stop = [&](LayoutEnd end, std::uint64_t at, std::string detail) -> Result<Layout> {
        layout.end = end;
        layout.stoppedAt = at;
        layout.detail = std::move(detail);
        return std::move(layout);
    };
    for (;;) {
        if (layout.boxes.size() == maxBoxes && !sequence.finished() && sequence.position() < size) {
            return stop(LayoutEnd::BoxLimit, sequence.position(),
                        "more than " + std::to_string(maxBoxes) + " top-level boxes");
        }
        Result<std::optional<BoxRead>> next = sequence.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value()) {
            return stop(LayoutEnd::EndOfData, sequence.position(),
                        std::to_string(layout.boxes.size()) + " top-level boxes end at the end of the data");
        }
        const BoxRead& read = **next;
        const BoxHeader& header = read.header;
        const std::uint64_t at = header.offset;
        switch (read.status) {
        case BoxStatus::Valid:
            if (!header.type.isPrintable()) {
                return stop(LayoutEnd::NotABox, at,
                            "the bytes at " + std::to_string(at) + " are not a box (type " + header.type.text() + ")");
            }
            layout.boxes.push_back(header);
            break;
        case BoxStatus::HeaderCut: {
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(size - at, BoxHeader::kMaxSize));
            Result<std::span<const std::byte>> rest = content.read(at, length);
            if (!rest.ok()) {
                return rest.error();
            }
            if (length >= kCompactHeader ? header.type.isPrintable() : couldStartBox(*rest)) {
                return stop(LayoutEnd::Truncated, at, "the data ends inside a box header");
            }
            return stop(LayoutEnd::NotABox, at, "the " + std::to_string(length) + " bytes at " + std::to_string(at) +
                                                    " are not a box header");
        }
        case BoxStatus::TooLong:
            if (!header.type.isPrintable()) {
                return stop(LayoutEnd::NotABox, at,
                            "the bytes at " + std::to_string(at) + " are not a box (type " + header.type.text() + ")");
            }
            layout.cutBox = header;
            return stop(LayoutEnd::Truncated, at,
                        "the data ends inside box '" + header.type.text() + "' (" + std::to_string(size - at) +
                            " of " + std::to_string(header.size) + " bytes)");
        case BoxStatus::BadSize:
            return stop(LayoutEnd::NotABox, at,
                        "the bytes at " + std::to_string(at) + " are not a box (size " + std::to_string(header.size) +
                            " is smaller than its header)");
        }
    }
}

}  // namespace recovery::formats::mp4
