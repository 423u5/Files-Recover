// ISO BMFF boxes (P11): four-character codes, box headers with 32-bit,
// 64-bit and "to the end" sizes and uuid extended types, bounds checks at
// every size and limit, box sequences, and the top-level scan (any order, a
// second ftyp is one more box, truncation, what is not a box, box limits).

#include "formats/mp4_box.hpp"

#include "format_test_helpers.hpp"
#include "mp4_test_helpers.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace recovery::formats {
namespace {

using mp4::BoxHeader;
using mp4::BoxRead;
using mp4::BoxSequence;
using mp4::BoxSize;
using mp4::BoxStatus;
using mp4::FourCc;
using mp4::Layout;
using mp4::LayoutEnd;
using testing::Bytes;
using testing::concat;

void putBe32(Bytes& out, std::uint64_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

void putBe64(Bytes& out, std::uint64_t value) {
    putBe32(out, value >> 32);
    putBe32(out, value & 0xFFFFFFFF);
}

void putText(Bytes& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

// A box header with a 32-bit size field, then `payload` zero bytes.
Bytes header32(std::uint32_t size, std::string_view type, std::size_t payload = 0) {
    Bytes out;
    putBe32(out, size);
    putText(out, type);
    out.resize(out.size() + payload);
    return out;
}

// A box header with a 64-bit size, then `payload` zero bytes.
Bytes header64(std::uint64_t size, std::string_view type, std::size_t payload = 0) {
    Bytes out;
    putBe32(out, 1);
    putText(out, type);
    putBe64(out, size);
    out.resize(out.size() + payload);
    return out;
}

Bytes box(std::string_view type, std::size_t payload) {
    return test::makeBox(type, Bytes(payload));
}

BoxRead parse(const Bytes& bytes, std::uint64_t offset = 0, std::uint64_t limit = UINT64_MAX) {
    return mp4::parseBoxHeader(bytes, offset, limit == UINT64_MAX ? offset + bytes.size() : limit);
}

Layout scan(const Bytes& bytes, std::uint64_t start = 0, std::uint64_t maxBoxes = 1'000'000) {
    carving::MemoryContentReader content(bytes);
    Result<Layout> layout = mp4::scanTopLevel(content, start, maxBoxes);
    EXPECT_TRUE(layout.ok());
    return layout.ok() ? *layout : Layout{};
}

std::string types(const Layout& layout) {
    std::string text;
    for (const BoxHeader& header : layout.boxes) {
        text += (text.empty() ? "" : " ") + header.type.text();
    }
    return text;
}

// ---------------------------------------------------------------------------
// FourCc
// ---------------------------------------------------------------------------

TEST(Mp4FourCcTest, LiteralsValuesAndText) {
    constexpr FourCc moov("moov");
    static_assert(moov.value() == 0x6D6F6F76);
    static_assert(moov == mp4::box::kMoov);
    static_assert(moov.isPrintable());
    static_assert(FourCc("M4A ").isPrintable());
    EXPECT_EQ(moov.text(), "moov");
    EXPECT_EQ(FourCc("url ").text(), "url ");
    EXPECT_EQ(FourCc(0x00000000).text(), "0x00000000");
    EXPECT_EQ(FourCc(0xA96E616D).text(), "0xA96E616D");  // iTunes' '(c)nam' is not ASCII
    EXPECT_NE(FourCc("moov"), FourCc("moof"));
}

TEST(Mp4FourCcTest, PrintableMeansAsciiFrom0x20To0x7E) {
    EXPECT_TRUE(FourCc(0x20202020).isPrintable());
    EXPECT_TRUE(FourCc(0x7E7E7E7E).isPrintable());
    EXPECT_FALSE(FourCc(0x7F616161).isPrintable());
    EXPECT_FALSE(FourCc(0x6161611F).isPrintable());
    EXPECT_FALSE(FourCc(0x61610061).isPrintable());
}

TEST(Mp4FourCcTest, ReadsFromBytes) {
    const Bytes bytes = header32(8, "trak");
    EXPECT_EQ(FourCc::at(bytes, 4), mp4::box::kTrak);
}

TEST(Mp4FourCcTest, TopLevelTypes) {
    for (const FourCc type : {mp4::box::kFtyp, mp4::box::kMoov, mp4::box::kMdat, mp4::box::kFree, mp4::box::kSkip,
                              mp4::box::kWide, mp4::box::kUuid, mp4::box::kMoof, FourCc("mfra"), FourCc("sidx"),
                              FourCc("pnot"), FourCc("meta"), FourCc("udta")}) {
        EXPECT_TRUE(mp4::isTopLevelType(type)) << type.text();
    }
    for (const FourCc type : {mp4::box::kTrak, mp4::box::kStbl, mp4::box::kMvhd, FourCc("abcd")}) {
        EXPECT_FALSE(mp4::isTopLevelType(type)) << type.text();
    }
}

// ---------------------------------------------------------------------------
// Box headers
// ---------------------------------------------------------------------------

TEST(Mp4BoxHeaderTest, CompactSize) {
    const BoxRead read = parse(header32(24, "free", 16), 100, 124);
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.type, mp4::box::kFree);
    EXPECT_EQ(read.header.sizeKind, BoxSize::Compact);
    EXPECT_EQ(read.header.offset, 100u);
    EXPECT_EQ(read.header.size, 24u);
    EXPECT_EQ(read.header.headerSize, 8u);
    EXPECT_EQ(read.header.payloadOffset(), 108u);
    EXPECT_EQ(read.header.payloadSize(), 16u);
    EXPECT_EQ(read.header.end(), 124u);
}

TEST(Mp4BoxHeaderTest, LargeSize) {
    BoxRead read = parse(header64(40, "mdat", 24));
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.sizeKind, BoxSize::Large);
    EXPECT_EQ(read.header.size, 40u);
    EXPECT_EQ(read.header.headerSize, 16u);
    EXPECT_EQ(read.header.payloadSize(), 24u);

    // An empty box with a 64-bit size, and one far beyond 4 GiB.
    read = parse(header64(16, "free"));
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.payloadSize(), 0u);
    const std::uint64_t huge = 5 * kGiB + 3;
    read = mp4::parseBoxHeader(header64(huge, "mdat"), 1000, 1000 + huge);
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.end(), 1000 + huge);
}

TEST(Mp4BoxHeaderTest, SizeZeroExtendsToTheEndOfTheBounds) {
    const BoxRead read = mp4::parseBoxHeader(header32(0, "mdat"), 500, 9000);
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.sizeKind, BoxSize::ToEnd);
    EXPECT_EQ(read.header.size, 8500u);
    EXPECT_EQ(read.header.end(), 9000u);
}

TEST(Mp4BoxHeaderTest, UuidBoxesCarryAnExtendedType) {
    Bytes compact = header32(8 + 16 + 4, "uuid");
    for (std::uint8_t i = 0; i < 16; ++i) {
        compact.push_back(static_cast<std::byte>(0xC0 + i));
    }
    compact.resize(compact.size() + 4);
    BoxRead read = parse(compact);
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.headerSize, 24u);
    EXPECT_EQ(read.header.payloadSize(), 4u);
    EXPECT_EQ(read.header.userType[0], std::byte{0xC0});
    EXPECT_EQ(read.header.userType[15], std::byte{0xCF});

    Bytes large = header64(16 + 16, "uuid", 16);
    read = parse(large);
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.headerSize, 32u);
    EXPECT_EQ(read.header.payloadSize(), 0u);

    read = parse(header32(0, "uuid", 16));
    ASSERT_EQ(read.status, BoxStatus::Valid);
    EXPECT_EQ(read.header.headerSize, 24u);
    EXPECT_EQ(read.header.sizeKind, BoxSize::ToEnd);
}

TEST(Mp4BoxHeaderTest, SizesSmallerThanTheHeaderAreBad) {
    for (std::uint32_t size = 2; size < 8; ++size) {
        const BoxRead read = parse(header32(size, "free", 32));
        EXPECT_EQ(read.status, BoxStatus::BadSize) << size;
        EXPECT_EQ(read.header.size, size);
    }
    for (const std::uint64_t size : {0ULL, 1ULL, 8ULL, 15ULL}) {
        EXPECT_EQ(parse(header64(size, "mdat", 32)).status, BoxStatus::BadSize) << size;
    }
    // A uuid box too small for its extended type.
    EXPECT_EQ(parse(header32(23, "uuid", 32)).status, BoxStatus::BadSize);
    EXPECT_EQ(parse(header64(31, "uuid", 32)).status, BoxStatus::BadSize);
}

TEST(Mp4BoxHeaderTest, BoxesLongerThanTheirBoundsAreTooLong) {
    BoxRead read = mp4::parseBoxHeader(header32(100, "trak", 8), 0, 99);
    EXPECT_EQ(read.status, BoxStatus::TooLong);
    EXPECT_EQ(read.header.size, 100u);
    EXPECT_EQ(read.header.type, mp4::box::kTrak);
    EXPECT_EQ(mp4::parseBoxHeader(header32(100, "trak", 8), 0, 100).status, BoxStatus::Valid);

    // The largest 64-bit size at a non-zero offset: no overflow, just too long.
    read = mp4::parseBoxHeader(header64(UINT64_MAX, "mdat"), 4096, UINT64_MAX);
    EXPECT_EQ(read.status, BoxStatus::TooLong);
    read = mp4::parseBoxHeader(header64(UINT64_MAX - 4095, "mdat"), 4096, UINT64_MAX);
    EXPECT_EQ(read.status, BoxStatus::TooLong);
    read = mp4::parseBoxHeader(header64(UINT64_MAX - 4096, "mdat"), 4096, UINT64_MAX);
    EXPECT_EQ(read.status, BoxStatus::Valid);
}

TEST(Mp4BoxHeaderTest, HeadersCutByTheBounds) {
    const Bytes compact = header32(16, "free", 8);
    for (std::uint64_t limit = 0; limit < 8; ++limit) {
        EXPECT_EQ(mp4::parseBoxHeader(compact, 0, limit).status, BoxStatus::HeaderCut) << limit;
    }
    const Bytes large = header64(64, "mdat", 48);
    for (std::uint64_t limit = 8; limit < 16; ++limit) {
        const BoxRead read = mp4::parseBoxHeader(large, 0, limit);
        EXPECT_EQ(read.status, BoxStatus::HeaderCut) << limit;
        EXPECT_EQ(read.header.type, mp4::box::kMdat);
    }
    const Bytes uuid = header32(64, "uuid", 56);
    for (std::uint64_t limit = 8; limit < 24; ++limit) {
        EXPECT_EQ(mp4::parseBoxHeader(uuid, 0, limit).status, BoxStatus::HeaderCut) << limit;
    }
    // Fewer bytes given than the bounds allow: the same as bounds ending there.
    EXPECT_EQ(mp4::parseBoxHeader(std::span(large).first(12), 0, 64).status, BoxStatus::HeaderCut);
    // An offset beyond the limit has no room at all.
    EXPECT_EQ(mp4::parseBoxHeader(compact, 50, 40).status, BoxStatus::HeaderCut);
}

TEST(Mp4BoxHeaderTest, NoCombinationOfSizeAndBoundsEscapesTheBounds) {
    const std::vector<std::uint64_t> sizes = {0, 1, 2, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 40, 0xFFFFFFFF};
    for (const std::uint64_t size : sizes) {
        for (const bool wide : {false, true}) {
            for (const std::string_view type : {"free", "uuid"}) {
                Bytes bytes = wide ? header64(size, type, 24) : header32(static_cast<std::uint32_t>(size), type, 32);
                for (std::uint64_t limit = 0; limit <= 48; ++limit) {
                    const BoxRead read = mp4::parseBoxHeader(bytes, 0, limit);
                    if (read.status == BoxStatus::Valid) {
                        EXPECT_LE(read.header.end(), limit);
                        EXPECT_GE(read.header.size, read.header.headerSize);
                        EXPECT_LE(read.header.headerSize, BoxHeader::kMaxSize);
                    }
                }
            }
        }
    }
}

TEST(Mp4BoxHeaderTest, ReadingFromContent) {
    const Bytes bytes = concat({box("free", 8), box("skip", 0)});
    carving::MemoryContentReader content(bytes);
    Result<BoxRead> read = mp4::readBoxHeader(content, 16, 1000);  // the limit is clipped to the content
    RECOVERY_ASSERT_OK(read);
    EXPECT_EQ(read->status, BoxStatus::Valid);
    EXPECT_EQ(read->header.type, mp4::box::kSkip);
    read = mp4::readBoxHeader(content, 24, 24);
    RECOVERY_ASSERT_OK(read);
    EXPECT_EQ(read->status, BoxStatus::HeaderCut);
    read = mp4::readBoxHeader(content, 0, 12);
    RECOVERY_ASSERT_OK(read);
    EXPECT_EQ(read->status, BoxStatus::TooLong);
    RECOVERY_EXPECT_ERROR(mp4::readBoxHeader(content, 25, 100), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(mp4::readBoxHeader(content, 10, 5), ErrorCode::InvalidInput);
}

// ---------------------------------------------------------------------------
// Box sequences
// ---------------------------------------------------------------------------

TEST(Mp4BoxSequenceTest, BoxesThatFillTheRange) {
    const Bytes bytes = concat({box("mvhd", 4), box("trak", 20), box("udta", 0)});
    carving::MemoryContentReader content(bytes);
    BoxSequence sequence(content, 0, bytes.size());
    std::string seen;
    for (;;) {
        Result<std::optional<BoxRead>> next = sequence.next();
        RECOVERY_ASSERT_OK(next);
        if (!next->has_value()) {
            break;
        }
        ASSERT_EQ((*next)->status, BoxStatus::Valid);
        seen += (*next)->header.type.text();
    }
    EXPECT_EQ(seen, "mvhdtrakudta");
    EXPECT_TRUE(sequence.finished());
    EXPECT_EQ(sequence.position(), bytes.size());
}

TEST(Mp4BoxSequenceTest, ABoxThatDoesNotFitEndsTheSequence) {
    Bytes bytes = concat({box("mvhd", 4), box("trak", 20)});
    carving::MemoryContentReader content(bytes);
    BoxSequence sequence(content, 0, bytes.size() - 1);
    Result<std::optional<BoxRead>> next = sequence.next();
    RECOVERY_ASSERT_OK(next);
    EXPECT_EQ((*next)->status, BoxStatus::Valid);
    next = sequence.next();
    RECOVERY_ASSERT_OK(next);
    ASSERT_TRUE(next->has_value());
    EXPECT_EQ((*next)->status, BoxStatus::TooLong);
    EXPECT_EQ(sequence.position(), 12u);
    next = sequence.next();
    RECOVERY_ASSERT_OK(next);
    EXPECT_FALSE(next->has_value());
}

TEST(Mp4BoxSequenceTest, SizeZeroEndsTheSequence) {
    const Bytes bytes = concat({box("free", 0), header32(0, "mdat", 20)});
    carving::MemoryContentReader content(bytes);
    BoxSequence sequence(content, 0, bytes.size());
    ASSERT_TRUE(sequence.next()->has_value());
    Result<std::optional<BoxRead>> last = sequence.next();
    ASSERT_TRUE(last->has_value());
    EXPECT_EQ((*last)->header.sizeKind, BoxSize::ToEnd);
    EXPECT_EQ((*last)->header.size, 28u);
    EXPECT_FALSE(sequence.next()->has_value());
}

TEST(Mp4BoxSequenceTest, EmptyAndInvertedRanges) {
    const Bytes bytes = box("free", 8);
    carving::MemoryContentReader content(bytes);
    BoxSequence empty(content, 8, 8);
    EXPECT_FALSE(empty.next()->has_value());
    BoxSequence inverted(content, 12, 4);
    EXPECT_TRUE(inverted.finished());
    EXPECT_FALSE(inverted.next()->has_value());
    // The end is clipped to the content.
    BoxSequence beyond(content, 0, 1000);
    EXPECT_EQ(beyond.end(), bytes.size());
    EXPECT_EQ((*beyond.next())->status, BoxStatus::Valid);
    EXPECT_FALSE(beyond.next()->has_value());
}

// ---------------------------------------------------------------------------
// The top level
// ---------------------------------------------------------------------------

TEST(Mp4ScanTopLevelTest, BoxesInAnyOrder) {
    const Bytes moovLast = concat({box("ftyp", 16), box("free", 0), box("mdat", 100), box("moov", 40)});
    Layout layout = scan(moovLast);
    EXPECT_EQ(layout.end, LayoutEnd::EndOfData) << layout.detail;
    EXPECT_EQ(types(layout), "ftyp free mdat moov");
    EXPECT_EQ(layout.stoppedAt, moovLast.size());
    ASSERT_NE(layout.first(mp4::box::kMoov), nullptr);
    EXPECT_EQ(layout.first(mp4::box::kMoov)->offset, 24u + 8 + 108);

    const Bytes scrambled =
        concat({box("mdat", 10), box("wide", 0), box("mdat", 20), box("moov", 8), box("ftyp", 8), box("mdat", 0)});
    layout = scan(scrambled);
    EXPECT_EQ(layout.end, LayoutEnd::EndOfData);
    EXPECT_EQ(types(layout), "mdat wide mdat moov ftyp mdat");
    EXPECT_EQ(layout.all(mp4::box::kMdat).size(), 3u);
}

TEST(Mp4ScanTopLevelTest, ASecondFtypIsOneMoreBoxNotTheEnd) {
    const Bytes one = concat({box("ftyp", 16), box("moov", 40), box("mdat", 100)});
    const Bytes two = concat({one, one});
    const Layout layout = scan(two);
    EXPECT_EQ(layout.end, LayoutEnd::EndOfData);
    EXPECT_EQ(types(layout), "ftyp moov mdat ftyp moov mdat");
    EXPECT_EQ(layout.all(mp4::box::kFtyp)[1].offset, one.size());
}

TEST(Mp4ScanTopLevelTest, WhatLooksLikeABoxInsideMdatIsSkipped) {
    Bytes media = box("ftyp", 16);
    const Bytes more = box("moov", 8);
    media.insert(media.end(), more.begin(), more.end());
    const Bytes file = concat({box("ftyp", 16), test::makeBox("mdat", media), box("moov", 8)});
    EXPECT_EQ(types(scan(file)), "ftyp mdat moov");
}

TEST(Mp4ScanTopLevelTest, UnfamiliarTypesAreListed) {
    const Bytes file = concat({box("ftyp", 8), box("abcd", 4), box("moov", 8)});
    const Layout layout = scan(file);
    EXPECT_EQ(types(layout), "ftyp abcd moov");
    EXPECT_FALSE(mp4::isTopLevelType(layout.boxes[1].type));
}

TEST(Mp4ScanTopLevelTest, WhatIsNotABoxStopsTheScan) {
    const Bytes boxes = concat({box("ftyp", 8), box("moov", 8)});
    // Zero padding (a cluster's slack), an unprintable type, a size below the header.
    for (const Bytes& tail : {Bytes(64), concat({header32(16, "mo\x01v", 8)}), header32(5, "free", 8)}) {
        const Layout layout = scan(concat({boxes, tail}));
        EXPECT_EQ(layout.end, LayoutEnd::NotABox) << layout.detail;
        EXPECT_EQ(layout.boxes.size(), 2u);
        EXPECT_EQ(layout.stoppedAt, boxes.size());
    }
    // A 64-bit size below 16.
    EXPECT_EQ(scan(concat({boxes, header64(12, "mdat", 8)})).end, LayoutEnd::NotABox);
}

TEST(Mp4ScanTopLevelTest, TruncatedBoxesAndHeaders) {
    const Bytes boxes = concat({box("ftyp", 8), box("moov", 8)});
    // The data ends inside mdat.
    Layout layout = scan(concat({boxes, header32(1000, "mdat", 100)}));
    EXPECT_EQ(layout.end, LayoutEnd::Truncated);
    EXPECT_EQ(layout.stoppedAt, boxes.size());
    ASSERT_TRUE(layout.cutBox.has_value());
    EXPECT_EQ(layout.cutBox->type, mp4::box::kMdat);
    EXPECT_EQ(layout.cutBox->size, 1000u);
    EXPECT_NE(layout.detail.find("108 of 1000"), std::string::npos) << layout.detail;
    // ... inside a 64-bit mdat far beyond 4 GiB.
    layout = scan(concat({boxes, header64(6 * kGiB, "mdat", 10)}));
    EXPECT_EQ(layout.end, LayoutEnd::Truncated);
    EXPECT_EQ(layout.cutBox->size, 6 * kGiB);

    // Inside a header: what is there could start a box.
    for (const std::size_t cut : {1, 3, 4, 5, 7}) {
        const Bytes head = header32(4096, "mdat");
        layout = scan(concat({boxes, std::span(head).first(cut)}));
        EXPECT_EQ(layout.end, LayoutEnd::Truncated) << cut;
        EXPECT_FALSE(layout.cutBox.has_value());
    }
    const Bytes large = header64(4096, "mdat");
    layout = scan(concat({boxes, std::span(large).first(12)}));
    EXPECT_EQ(layout.end, LayoutEnd::Truncated);
    // ... or could not: a size of 2 to 7, an unprintable type byte.
    const Bytes small = header32(3, "mdat");
    EXPECT_EQ(scan(concat({boxes, std::span(small).first(6)})).end, LayoutEnd::NotABox);
    const Bytes binary = header32(64, "m\x02" "at");
    EXPECT_EQ(scan(concat({boxes, std::span(binary).first(6)})).end, LayoutEnd::NotABox);
}

TEST(Mp4ScanTopLevelTest, SizeZeroRunsToTheEndOfTheData) {
    const Bytes file = concat({box("ftyp", 8), box("moov", 8), header32(0, "mdat", 5000)});
    const Layout layout = scan(file);
    EXPECT_EQ(layout.end, LayoutEnd::EndOfData);
    ASSERT_EQ(layout.boxes.size(), 3u);
    EXPECT_EQ(layout.boxes[2].sizeKind, BoxSize::ToEnd);
    EXPECT_EQ(layout.boxes[2].end(), file.size());
}

TEST(Mp4ScanTopLevelTest, BoxLimit) {
    const Bytes file = concat({box("free", 0), box("free", 0), box("free", 0), box("free", 0), box("free", 0)});
    Layout layout = scan(file, 0, 3);
    EXPECT_EQ(layout.end, LayoutEnd::BoxLimit);
    EXPECT_EQ(layout.boxes.size(), 3u);
    EXPECT_EQ(layout.stoppedAt, 24u);
    layout = scan(file, 0, 5);
    EXPECT_EQ(layout.end, LayoutEnd::EndOfData);
    EXPECT_EQ(layout.boxes.size(), 5u);
}

TEST(Mp4ScanTopLevelTest, StartingInsideTheContent) {
    const Bytes file = concat({Bytes(100, std::byte{0xEE}), box("moov", 8), box("mdat", 8)});
    const Layout layout = scan(file, 100);
    EXPECT_EQ(layout.end, LayoutEnd::EndOfData);
    EXPECT_EQ(types(layout), "moov mdat");
    EXPECT_EQ(layout.boxes[0].offset, 100u);
    EXPECT_EQ(scan(file, file.size()).boxes.size(), 0u);
    EXPECT_EQ(scan({}).end, LayoutEnd::EndOfData);
}

TEST(Mp4ScanTopLevelTest, InvalidArguments) {
    const Bytes file = box("free", 0);
    carving::MemoryContentReader content(file);
    RECOVERY_EXPECT_ERROR(mp4::scanTopLevel(content, 9), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(mp4::scanTopLevel(content, 0, 0), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery::formats
