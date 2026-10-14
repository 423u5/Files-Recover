// ZIP: archives that Python's zipfile wrote (DEFLATE, stored, empty members
// and folders, data descriptors, ZIP64 members, a comment, LZMA and bzip2
// members) and reworked ones (ZIP64 end records, an APK signing block);
// every prefix; damaged headers, data, central directories and end
// records; encrypted members; the limits; carving with the image formats
// (the files stored in a verified archive are part of it); fuzzing.

#include "formats/zip_format.hpp"

#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "format_test_helpers.hpp"
#include "formats/archive_formats.hpp"
#include "formats/image_formats.hpp"
#include "recovery/byte_order.hpp"
#include "support/image_builders.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"
#include "support/zip_builders.hpp"
#include "support/zip_samples.hpp"
#include "validation/deflate_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::FileCandidate;
using carving::ValidationStatus;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::headerOf;
using testing::isIntact;
using testing::overwritten;
using testing::verdictOf;

constexpr std::uint32_t kLocal = 0x04034B50;
constexpr std::uint32_t kCentral = 0x02014B50;
constexpr std::uint32_t kEnd = 0x06054B50;

std::shared_ptr<const ZipFormat> makeFormat(ZipFormat::Limits limits = {}) {
    Result<std::shared_ptr<const ZipFormat>> made = ZipFormat::make(validation::deflateDecoder(), limits);
    return made.ok() ? *made : nullptr;
}

const ZipFormat& format() {
    static const std::shared_ptr<const ZipFormat> instance = makeFormat();
    return *instance;
}

Bytes vector(std::string_view name) {
    return test::zip_samples::named(name).data();
}

// Offsets of every 4-byte `signature` in `data`.
std::vector<std::size_t> offsetsOf(const Bytes& data, std::uint32_t signature) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i + 4 <= data.size(); ++i) {
        if (loadLe32(data, i) == signature) {
            out.push_back(i);
        }
    }
    return out;
}

Bytes flipped(Bytes data, std::size_t offset) {
    data[offset] ^= std::byte{0xFF};
    return data;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// The archives every check of the format can read in full.
constexpr std::array<std::string_view, 8> kCheckable = {"mixed",   "stored",  "streamed", "zip64_members",
                                                        "zip64_end", "comment", "apk",      "docx"};

TEST(ZipFormatTest, Descriptor) {
    const carving::FormatDescriptor& d = format().descriptor();
    EXPECT_EQ(d.id, "zip");
    EXPECT_EQ(d.extension, "zip");
    EXPECT_TRUE(d.selfSynchronizing);
    EXPECT_TRUE(d.foundEndMeansContiguous);
    ASSERT_EQ(d.signatures.size(), 1u);
    EXPECT_EQ(d.signatures[0].offset, 0u);
    EXPECT_EQ(d.minimumSize, ZipFormat::kMinimumSize);
    EXPECT_EQ(d.maximumSize, ZipFormat::kMaximumSize);
    EXPECT_EQ(d.endDetection, carving::EndDetectionMethod::StructureWalk);
    RECOVERY_EXPECT_OK(carving::validateDescriptor(d));
}

TEST(ZipFormatTest, TheFormatNeedsADecoderAndLimits) {
    RECOVERY_EXPECT_ERROR(ZipFormat::make(nullptr), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(ZipFormat::make(validation::deflateDecoder(), {0, 1}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(ZipFormat::make(validation::deflateDecoder(), {1, 0}), ErrorCode::InvalidInput);

    carving::FormatRegistry registry;
    RECOVERY_EXPECT_ERROR(registerArchiveFormats(registry, nullptr), ErrorCode::InvalidInput);
    EXPECT_TRUE(registry.empty());
    RECOVERY_ASSERT_OK(registerArchiveFormats(registry, validation::deflateDecoder()));
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_NE(registry.find("zip"), nullptr);
    RECOVERY_EXPECT_ERROR(registerArchiveFormats(registry, validation::deflateDecoder()), ErrorCode::InvalidInput);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(ZipFormatTest, ArchivesOfEveryKindAreIntact) {
    for (const std::string_view name : kCheckable) {
        SCOPED_TRACE(std::string(name));
        const Bytes data = vector(name);
        EXPECT_TRUE(isIntact(format(), data));
        const carving::ValidationResult verdict = verdictOf(format(), data);
        EXPECT_TRUE(contains(verdict.detail, "every member's CRC-32 right")) << verdict.detail;
    }
}

TEST(ZipFormatTest, TheDetailSaysWhatWasChecked) {
    EXPECT_EQ(verdictOf(format(), vector("mixed")).detail,
              "6 members, 75700 bytes unpacked, every member's CRC-32 right");
    EXPECT_TRUE(contains(verdictOf(format(), vector("zip64_members")).detail, "ZIP64"));
    EXPECT_TRUE(contains(verdictOf(format(), vector("zip64_end")).detail, "ZIP64"));
    EXPECT_TRUE(contains(verdictOf(format(), vector("apk")).detail, "an APK signing block"));
    EXPECT_EQ(endOf(format(), vector("mixed")).detail, "6 members");
}

TEST(ZipFormatTest, DataDescriptorsAreFollowedForEveryMethod) {
    // Written to a stream that cannot seek: every member's sizes and CRC-32
    // are in a data descriptor after its data, the stored ones too.
    const Bytes data = vector("streamed");
    for (const std::size_t at : offsetsOf(data, kLocal)) {
        EXPECT_NE(loadLe16(data, at + 6) & 0x0008, 0) << "member at " << at;
    }
    const carving::EndDetection end = endOf(format(), data);
    EXPECT_EQ(end.status, EndStatus::Found);
    EXPECT_EQ(end.length, data.size());
    EXPECT_EQ(verdictOf(format(), data).status, ValidationStatus::Valid);
}

TEST(ZipFormatTest, MembersTheEngineCannotDecodeLeaveTheArchiveUnchecked) {
    const Bytes lzma = vector("lzma");
    const carving::EndDetection end = endOf(format(), lzma);
    EXPECT_EQ(end.status, EndStatus::Found);
    EXPECT_EQ(end.length, lzma.size());
    carving::ValidationResult verdict = verdictOf(format(), lzma);
    EXPECT_EQ(verdict.status, ValidationStatus::NotValidated);
    EXPECT_EQ(verdict.validBytes, lzma.size());
    EXPECT_TRUE(contains(verdict.detail, "1 member was not checked (member 2 uses LZMA)")) << verdict.detail;

    verdict = verdictOf(format(), vector("bzip2"));
    EXPECT_EQ(verdict.status, ValidationStatus::NotValidated);
    EXPECT_TRUE(contains(verdict.detail, "member 1 uses bzip2")) << verdict.detail;
}

TEST(ZipFormatTest, EncryptedMembersAreNotChecked) {
    Bytes data = vector("stored");
    const std::size_t local = offsetsOf(data, kLocal)[1];
    const std::size_t central = offsetsOf(data, kCentral)[1];
    data = overwritten(data, local + 6, {0x01, 0x00});
    data = overwritten(data, central + 8, {0x01, 0x00});
    const carving::ValidationResult verdict = verdictOf(format(), data);
    EXPECT_EQ(verdict.status, ValidationStatus::NotValidated);
    EXPECT_TRUE(contains(verdict.detail, "member 2 is encrypted")) << verdict.detail;
}

TEST(ZipFormatTest, NoPrefixIsComplete) {
    for (const std::string_view name : kCheckable) {
        SCOPED_TRACE(std::string(name));
        const Bytes data = vector(name);
        for (std::size_t length = 0; length < data.size(); length += (length < 4096 ? 1 : 97)) {
            const std::span<const std::byte> prefix(data.data(), length);
            const carving::EndDetection end = endOf(format(), prefix);
            ASSERT_NE(end.status, EndStatus::Found) << "prefix " << length << ": " << testing::describe(end);
            ASSERT_LE(end.length, length);
            const carving::ValidationResult verdict = verdictOf(format(), prefix);
            ASSERT_NE(verdict.status, ValidationStatus::Valid) << "prefix " << length;
            ASSERT_LE(verdict.validBytes, length);
        }
    }
}

TEST(ZipFormatTest, ADirectoryOrEndRecordCutShortIsTruncated) {
    const Bytes data = vector("mixed");
    const std::size_t directory = offsetsOf(data, kCentral).front();
    for (const std::size_t length : {directory + 10, data.size() - 10, data.size() - 1}) {
        SCOPED_TRACE(length);
        const std::span<const std::byte> prefix(data.data(), length);
        EXPECT_EQ(endOf(format(), prefix).status, EndStatus::Truncated);
        EXPECT_EQ(verdictOf(format(), prefix).status, ValidationStatus::Truncated);
    }
}

TEST(ZipFormatTest, DamagedMemberDataFailsItsCrc) {
    // The structure is intact, so the end is still found; only the data's
    // decoding or its CRC-32 tells.
    const Bytes mixed = vector("mixed");
    const std::vector<std::size_t> locals = offsetsOf(mixed, kLocal);
    ASSERT_EQ(locals.size(), 6u);
    // data/stored.bin (member 5) is stored: its CRC-32 tells.
    const std::size_t stored = locals[4] + 30 + std::string_view("data/stored.bin").size();
    Bytes damaged = overwritten(mixed, stored + 100, {0x55});
    EXPECT_EQ(endOf(format(), damaged).status, EndStatus::Found);
    carving::ValidationResult verdict = verdictOf(format(), damaged);
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_EQ(verdict.detail, "the CRC-32 of member 5 is wrong");

    // logs/long.log (member 6) is DEFLATE: its decoding or its CRC-32 tells.
    const std::size_t deflated = locals[5] + 30 + std::string_view("logs/long.log").size();
    damaged = overwritten(mixed, deflated + 40, {0x00, 0xFF});
    EXPECT_EQ(endOf(format(), damaged).status, EndStatus::Found);
    verdict = verdictOf(format(), damaged);
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "member 6")) << verdict.detail;
    EXPECT_GE(verdict.validBytes, deflated);
}

TEST(ZipFormatTest, TheCentralDirectoryMustDescribeTheMembers) {
    const Bytes mixed = vector("mixed");
    const std::vector<std::size_t> entries = offsetsOf(mixed, kCentral);
    ASSERT_EQ(entries.size(), 6u);
    // Entry 2's CRC-32.
    carving::ValidationResult verdict = verdictOf(format(), flipped(mixed, entries[1] + 16));
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "central directory entry 2 does not match the member")) << verdict.detail;
    // Entry 3 names an offset where no member starts.
    verdict = verdictOf(format(), overwritten(mixed, entries[2] + 42, {0x01, 0x00, 0x00, 0x00}));
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "where no member starts")) << verdict.detail;
    // The end record counts one entry too many.
    const std::size_t end = offsetsOf(mixed, kEnd).back();
    verdict = verdictOf(format(), overwritten(mixed, end + 10, {0x07, 0x00}));
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "counts 7 entries")) << verdict.detail;
}

TEST(ZipFormatTest, AnArchiveSeenFromALaterMemberIsBrokenAtItsDirectory) {
    // The first members are gone (overwritten, or in another fragment): the
    // central directory places the archive's start earlier.
    const Bytes mixed = vector("mixed");
    const std::size_t second = offsetsOf(mixed, kLocal)[1];
    const std::size_t directory = offsetsOf(mixed, kCentral).front();
    const Bytes rest(mixed.begin() + static_cast<std::ptrdiff_t>(second), mixed.end());
    EXPECT_TRUE(headerOf(format(), rest).plausible);
    const carving::EndDetection end = endOf(format(), rest);
    EXPECT_EQ(end.status, EndStatus::Broken);
    EXPECT_EQ(end.length, directory - second);
    EXPECT_TRUE(contains(end.detail, "starts " + std::to_string(second) + " bytes before")) << end.detail;
}

TEST(ZipFormatTest, ADamagedSizeBreaksTheWalkInsteadOfReachingTheEndOfTheSource) {
    // A size beyond the data must not stretch the archive over everything
    // after it: the member headers there would be skipped as its own.
    const Bytes mixed = vector("mixed");
    const std::size_t member = offsetsOf(mixed, kLocal)[2];
    const Bytes damaged = overwritten(mixed, member + 18, {0xFF, 0xFF, 0xFF, 0x7F});
    const carving::EndDetection end = endOf(format(), concat({damaged, Bytes(100'000)}));
    EXPECT_EQ(end.status, EndStatus::Broken);
    EXPECT_EQ(end.length, member);
    EXPECT_TRUE(contains(end.detail, "reaches beyond the data")) << end.detail;
}

TEST(ZipFormatTest, HeadersNoWriterMakesAreRejected) {
    const Bytes good = vector("mixed");
    EXPECT_TRUE(headerOf(format(), good).plausible);
    struct Case {
        std::size_t offset;
        std::vector<std::uint8_t> bytes;
        std::string_view reason;
    };
    const std::vector<Case> cases = {
        {0, {'P', 'K', 0x05, 0x06}, "no ZIP local header"},
        {4, {64}, "beyond ZIP 6.3"},
        {6, {0x80, 0x00}, "flags that no ZIP writer sets"},
        {8, {7, 0}, "unknown compression method 7"},
        {26, {0, 0}, "a member without a name"},
        {30, {0}, "a NUL byte in the member's name"},
        {18, {1, 0, 0, 0}, "a stored member whose compressed and uncompressed sizes differ"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(c.reason));
        Bytes damaged = good;
        for (std::size_t i = 0; i < c.bytes.size(); ++i) {
            damaged[c.offset + i] = static_cast<std::byte>(c.bytes[i]);
        }
        const carving::HeaderCheck check = headerOf(format(), damaged);
        EXPECT_FALSE(check.plausible);
        EXPECT_TRUE(contains(check.reason, c.reason)) << check.reason;
    }
}

TEST(ZipFormatTest, TheLimitsStopTheWorkNotTheCarve) {
    const Bytes mixed = vector("mixed");
    // Fewer bytes may be unpacked than the members hold: the rest is not checked.
    const std::shared_ptr<const ZipFormat> small = makeFormat({ZipFormat::Limits{}.maxMembers, 10'000});
    ASSERT_NE(small, nullptr);
    carving::ValidationResult verdict = verdictOf(*small, mixed);
    EXPECT_EQ(verdict.status, ValidationStatus::NotValidated);
    EXPECT_TRUE(contains(verdict.detail, "unpack to more than 10000 bytes")) << verdict.detail;
    EXPECT_EQ(endOf(*small, mixed).status, EndStatus::Found);

    // More members than allowed: the walk stops.
    const std::shared_ptr<const ZipFormat> few = makeFormat({3, ZipFormat::Limits{}.maxUnpacked});
    ASSERT_NE(few, nullptr);
    const carving::EndDetection end = endOf(*few, mixed);
    EXPECT_EQ(end.status, EndStatus::Broken);
    EXPECT_TRUE(contains(end.detail, "more than 3 members")) << end.detail;
}

TEST(ZipFormatTest, TheDecoderFindsTheEndOfRawDeflateData) {
    // docs/readme.txt, member 2 of "mixed": DEFLATE with known sizes.
    const Bytes mixed = vector("mixed");
    const std::size_t at = offsetsOf(mixed, kLocal)[1];
    const std::size_t data = at + 30 + loadLe16(mixed, at + 26) + loadLe16(mixed, at + 28);
    const std::uint32_t compressed = loadLe32(mixed, at + 18);
    const std::uint32_t size = loadLe32(mixed, at + 22);
    carving::MemoryContentReader content(mixed);
    const std::shared_ptr<const IDeflateDecoder> decoder = validation::deflateDecoder();

    Result<IDeflateDecoder::Outcome> outcome = decoder->decode(content, data, mixed.size(), size);
    RECOVERY_ASSERT_OK(outcome);
    EXPECT_EQ(outcome->kind, IDeflateDecoder::Outcome::Kind::Done);
    EXPECT_EQ(outcome->length, compressed);
    EXPECT_EQ(outcome->output, size);
    EXPECT_EQ(outcome->crc32, loadLe32(mixed, at + 14));

    outcome = decoder->decode(content, data, data + compressed / 2, size);
    RECOVERY_ASSERT_OK(outcome);
    EXPECT_EQ(outcome->kind, IDeflateDecoder::Outcome::Kind::Truncated);

    outcome = decoder->decode(content, data, mixed.size(), size - 1);
    RECOVERY_ASSERT_OK(outcome);
    EXPECT_EQ(outcome->kind, IDeflateDecoder::Outcome::Kind::TooLarge);
    EXPECT_LE(outcome->output, size - 1);

    // A block of the reserved type 3 (BTYPE = 11).
    const Bytes reserved = {std::byte{0x07}, std::byte{0x00}};
    carving::MemoryContentReader bad(reserved);
    outcome = decoder->decode(bad, 0, reserved.size(), 100);
    RECOVERY_ASSERT_OK(outcome);
    EXPECT_EQ(outcome->kind, IDeflateDecoder::Outcome::Kind::Invalid);
    EXPECT_TRUE(contains(outcome->detail, "reserved type 3")) << outcome->detail;
}

// Carving: every image format and ZIP.
testing::Carved carveWithArchives(storage::IStorageSource& source) {
    testing::Carved carved;
    carving::FormatRegistry registry;
    const bool images = registerImageFormats(registry).ok();
    const bool archives = registerArchiveFormats(registry, validation::deflateDecoder()).ok();
    EXPECT_TRUE(images && archives);
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry);
    EXPECT_TRUE(scanner.ok());
    if (!scanner.ok()) {
        return carved;
    }
    carving::FileCarver carver(source, carving::CarveOptions{});
    Result<carving::CarveReport> report = carver.run(*scanner, [&](FileCandidate&& candidate) {
        carved.candidates.push_back(std::move(candidate));
        return success();
    });
    EXPECT_TRUE(report.ok());
    if (report.ok()) {
        carved.report = std::move(*report);
    }
    return carved;
}

// A ZIP archive with a PNG stored in it: its signature shows.
Bytes archiveAround(const Bytes& png) {
    return test::storedArchive({{"icons/app.png", png}});
}

TEST(ZipFormatTest, FilesStoredInAVerifiedArchiveArePartOfIt) {
    const Bytes png = test::makePng(test::PngOptions{});
    const Bytes archive = archiveAround(png);
    ASSERT_TRUE(isIntact(format(), archive));
    test::VirtualSource source(256 * 1024);
    source.plant(4096, archive);
    RECOVERY_ASSERT_OK(source.open());
    const testing::Carved carved = carveWithArchives(source);
    ASSERT_EQ(carved.candidates.size(), 1u);
    EXPECT_EQ(carved.candidates[0].formatId, "zip");
    EXPECT_EQ(carved.candidates[0].length, archive.size());
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Valid);
    EXPECT_GE(carved.report.skippedInsideCandidates, 1u);
}

TEST(ZipFormatTest, FilesStoredInADamagedArchiveAreCarvedAsBefore) {
    // One byte of the PNG damaged: the archive's CRC-32 fails, so nothing
    // shows that the bytes inside are the archive's own.
    const Bytes png = test::makePng(test::PngOptions{});
    Bytes archive = archiveAround(png);
    const std::size_t pngAt = 30 + std::string_view("icons/app.png").size();
    archive[pngAt + png.size() - 20] ^= std::byte{0x01};
    test::VirtualSource source(256 * 1024);
    source.plant(4096, archive);
    RECOVERY_ASSERT_OK(source.open());
    const testing::Carved carved = carveWithArchives(source);
    ASSERT_EQ(carved.candidates.size(), 2u);
    EXPECT_EQ(carved.candidates[0].formatId, "zip");
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Invalid);
    EXPECT_EQ(carved.candidates[1].formatId, "png");
    EXPECT_EQ(carved.candidates[1].sourceOffset, 4096 + pngAt);
    EXPECT_EQ(carved.report.skippedInsideCandidates, 0u);
}

TEST(ZipFormatTest, AnArchiveIsOneCandidateWhateverItsMembers) {
    // Every member's local header is a hit: they are the archive's own, also
    // when the archive does not validate.
    const Bytes mixed = vector("mixed");
    Bytes damaged = mixed;
    const std::size_t lastMember = offsetsOf(mixed, kLocal).back();
    damaged[lastMember + 60] ^= std::byte{0x10};
    const std::array<const Bytes*, 2> archives = {&mixed, &damaged};
    for (const Bytes* archive : archives) {
        test::VirtualSource source(64 * 1024);
        source.plant(512, *archive);
        RECOVERY_ASSERT_OK(source.open());
        const testing::Carved carved = carveWithArchives(source);
        ASSERT_EQ(carved.candidates.size(), 1u);
        EXPECT_EQ(carved.candidates[0].formatId, "zip");
        EXPECT_EQ(carved.candidates[0].sourceOffset, 512u);
        EXPECT_EQ(carved.candidates[0].length, mixed.size());
    }
}

TEST(ZipFormatTest, Fuzzing) {
    std::mt19937_64 random(20261012);
    for (const std::string_view name : {"mixed", "streamed", "zip64_end", "apk", "lzma"}) {
        SCOPED_TRACE(std::string(name));
        const Bytes data = vector(name);
        for (int i = 0; i < 300; ++i) {
            Bytes damaged = data;
            const int changes = 1 + static_cast<int>(random() % 4);
            for (int n = 0; n < changes; ++n) {
                damaged[random() % damaged.size()] = static_cast<std::byte>(random() & 0xFF);
            }
            if (i % 3 == 1) {
                damaged.resize(random() % damaged.size());
            }
            (void)headerOf(format(), damaged);
            const carving::EndDetection end = endOf(format(), damaged);
            ASSERT_LE(end.length, damaged.size()) << "iteration " << i;
            const carving::ValidationResult verdict = verdictOf(format(), damaged);
            ASSERT_LE(verdict.validBytes, damaged.size()) << "iteration " << i;
        }
    }
}

}  // namespace
}  // namespace recovery::formats
