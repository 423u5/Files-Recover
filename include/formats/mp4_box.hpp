#pragma once

// ISO base media file format (ISO/IEC 14496-12) boxes: the layer below the
// MP4 parser (mp4_parser.hpp). MP4, MOV (QuickTime), M4A, M4V and 3GP files
// are sequences of boxes:
//
//   [size: BE32][type: 4 bytes]                    size >= 8: the whole box
//   [1][type][size: BE64]                          a 64-bit size (>= 16)
//   [0][type]                                      the box extends to the end of its bounds
//   ... then for type 'uuid' a 16-byte extended type, then the payload
//
// A box's payload may hold further boxes (moov, trak, ...). Every box read
// here is checked against its bounds: the end of its parent, or the end of the
// content at the top level. Nothing is assumed about the order of the boxes,
// how many mdat boxes there are, or what follows a box: a later ftyp is one
// more box, not the end of the file (see docs/formats/mp4.md).

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::formats::mp4 {

// A four-character code: a box type, a brand, a handler or a codec.
class FourCc {
public:
    constexpr FourCc() noexcept = default;
    constexpr explicit FourCc(std::uint32_t value) noexcept : value_(value) {}
    // From a four-character literal, FourCc("moov"); anything else does not compile.
    consteval explicit FourCc(const char (&text)[5]) : value_(fromText(text)) {}

    // The four bytes at `offset` (bounds-checked like loadBe32).
    [[nodiscard]] static FourCc at(std::span<const std::byte> bytes, std::size_t offset) noexcept;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    // Every byte is printable ASCII (0x20-0x7E), as in every standard box type.
    [[nodiscard]] constexpr bool isPrintable() const noexcept {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const std::uint32_t c = (value_ >> shift) & 0xFF;
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
        }
        return true;
    }
    // "moov", or "0x00000000" when not printable (never raw file bytes).
    [[nodiscard]] std::string text() const;

    friend constexpr bool operator==(FourCc, FourCc) noexcept = default;

private:
    static consteval std::uint32_t fromText(const char (&text)[5]) {
        if (text[4] != '\0') {
            throw "a FourCc literal has exactly four characters";
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8) | static_cast<std::uint8_t>(text[i]);
        }
        return value;
    }

    std::uint32_t value_ = 0;
};

// The box types the parser knows (the plan's list, and the ones next to them).
namespace box {
inline constexpr FourCc kFtyp{"ftyp"};
inline constexpr FourCc kMoov{"moov"};
inline constexpr FourCc kMdat{"mdat"};
inline constexpr FourCc kFree{"free"};
inline constexpr FourCc kSkip{"skip"};
inline constexpr FourCc kWide{"wide"};
inline constexpr FourCc kUuid{"uuid"};
inline constexpr FourCc kMvhd{"mvhd"};
inline constexpr FourCc kTrak{"trak"};
inline constexpr FourCc kTkhd{"tkhd"};
inline constexpr FourCc kMdia{"mdia"};
inline constexpr FourCc kMdhd{"mdhd"};
inline constexpr FourCc kHdlr{"hdlr"};
inline constexpr FourCc kMinf{"minf"};
inline constexpr FourCc kStbl{"stbl"};
inline constexpr FourCc kStsd{"stsd"};
inline constexpr FourCc kStts{"stts"};
inline constexpr FourCc kStsc{"stsc"};
inline constexpr FourCc kStsz{"stsz"};
inline constexpr FourCc kStz2{"stz2"};
inline constexpr FourCc kStco{"stco"};
inline constexpr FourCc kCo64{"co64"};
inline constexpr FourCc kMvex{"mvex"};
inline constexpr FourCc kTrex{"trex"};
inline constexpr FourCc kMoof{"moof"};
inline constexpr FourCc kMfhd{"mfhd"};
inline constexpr FourCc kTraf{"traf"};
inline constexpr FourCc kTfhd{"tfhd"};
inline constexpr FourCc kTfdt{"tfdt"};
inline constexpr FourCc kTrun{"trun"};
inline constexpr FourCc kAvcC{"avcC"};
inline constexpr FourCc kHvcC{"hvcC"};
}  // namespace box

// Box types that ISO/IEC 14496-12 and QuickTime place at the top level of a
// file: ftyp, moov, mdat, free, skip, wide, uuid, meta, udta, pdin, moof,
// mfra, styp, sidx, ssix, prft, emsg, meco and pnot. Other types are allowed
// by the format; this only tells the familiar from the unfamiliar.
[[nodiscard]] bool isTopLevelType(FourCc type) noexcept;

// How a box records its size.
enum class BoxSize : std::uint8_t {
    // A 32-bit size.
    Compact,
    // Size field 1 and a 64-bit size after the type.
    Large,
    // Size field 0: the box extends to the end of its bounds (at the top
    // level, the end of the file). Its size is the rest of the bounds.
    ToEnd,
};

struct BoxHeader {
    // A header is at most 32 bytes: size, type, 64-bit size, extended type.
    static constexpr std::size_t kMaxSize = 32;

    FourCc type;
    BoxSize sizeKind = BoxSize::Compact;
    // Content offset of the box's first byte.
    std::uint64_t offset = 0;
    // The whole box, header included.
    std::uint64_t size = 0;
    // 8, 16 with a 64-bit size, and 16 more for 'uuid'.
    std::uint32_t headerSize = 8;
    // The extended type of a 'uuid' box (zeros otherwise).
    std::array<std::byte, 16> userType{};

    [[nodiscard]] std::uint64_t payloadOffset() const noexcept { return offset + headerSize; }
    [[nodiscard]] std::uint64_t payloadSize() const noexcept { return size - headerSize; }
    [[nodiscard]] std::uint64_t end() const noexcept { return offset + size; }
};

enum class BoxStatus : std::uint8_t {
    // The box lies inside its bounds.
    Valid,
    // The bounds end inside the box header.
    HeaderCut,
    // The box's size runs past the bounds.
    TooLong,
    // The size is smaller than the box's own header (2 to 7, or a 64-bit size below 16).
    BadSize,
};

struct BoxRead {
    BoxStatus status = BoxStatus::Valid;
    // Complete when Valid. Otherwise as far as it could be read: always the
    // offset; the type once 8 bytes were there; the declared size when
    // TooLong or BadSize.
    BoxHeader header;
};

// The box header at `offset`, from `bytes`: the bytes of the content from
// `offset` on (at least min(limit - offset, BoxHeader::kMaxSize) of them for
// an exact answer; fewer mean the bounds end there). `limit` is the end of
// the box's bounds, `offset <= limit`. Never reads outside `bytes`.
[[nodiscard]] BoxRead parseBoxHeader(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t limit);

// The same, reading the content. `limit` is clipped to content.size(). Fails
// with InvalidInput when offset > limit, and with the reader's errors.
[[nodiscard]] Result<BoxRead> readBoxHeader(carving::IContentReader& content, std::uint64_t offset,
                                            std::uint64_t limit);

// The boxes that follow each other in [begin, end) of the content, one at a
// time: the top level of a file, or the payload of a container box.
//
// next() returns each box in turn, and an empty optional once the range is
// used up. A box that is not Valid ends the sequence: next() returns it once
// (its status says why) and then nothing. A ToEnd box also ends it. The
// sequence never reads outside [begin, end) or outside the content.
class BoxSequence {
public:
    // `end` is clipped to content.size(); begin > end is an empty sequence.
    BoxSequence(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end) noexcept;

    [[nodiscard]] Result<std::optional<BoxRead>> next();
    // Where the next box starts: the end of the last Valid box.
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
    [[nodiscard]] std::uint64_t end() const noexcept { return end_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    carving::IContentReader& content_;
    std::uint64_t position_;
    std::uint64_t end_;
    bool finished_ = false;
};

// ---------------------------------------------------------------------------
// The top level of a file
// ---------------------------------------------------------------------------

enum class LayoutEnd : std::uint8_t {
    // The last box ends where the content ends (or extends to it: size 0).
    EndOfData,
    // What follows the last box is not a box: a type that is not printable,
    // or a size smaller than its header.
    NotABox,
    // The content ends inside a box or inside a box header.
    Truncated,
    // `maxBoxes` boxes were read and more follow.
    BoxLimit,
};

// The top-level boxes of a file, in content order.
struct Layout {
    std::vector<BoxHeader> boxes;
    LayoutEnd end = LayoutEnd::EndOfData;
    // Where the scan stopped: the end of the last box, or for Truncated the
    // start of the box (or header) that the data ends in.
    std::uint64_t stoppedAt = 0;
    // For Truncated: the header of the box that runs past the data, when the
    // header itself was complete (for example an mdat cut short).
    std::optional<BoxHeader> cutBox;
    // Why the scan stopped (never file content).
    std::string detail;

    // The first box of the type, or nullptr.
    [[nodiscard]] const BoxHeader* first(FourCc type) const noexcept;
    // Every box of the type, in content order.
    [[nodiscard]] std::vector<BoxHeader> all(FourCc type) const;
};

// Reads the top-level boxes from `start` to the end of the content. It
// accepts any box whose type is printable and whose size fits, in any order
// and any number: a second ftyp or moov is listed like any other box (the
// caller decides what it means). Fails with InvalidInput when start is
// beyond the content or maxBoxes is 0, and with the reader's errors.
[[nodiscard]] Result<Layout> scanTopLevel(carving::IContentReader& content, std::uint64_t start = 0,
                                          std::uint64_t maxBoxes = 1'000'000);

}  // namespace recovery::formats::mp4
