// OLE2 compound files: files built here as Office, Outlook and Windows
// Installer name their streams (versions 3 and 4, the mini stream, storages,
// DIFAT sectors, free sectors at the end); every prefix; damaged headers,
// FATs, directories and chains; the limits; carving with the image formats
// (nothing checks a compound file's data, so the files inside it are carved
// too); fuzzing.

#include "formats/ole2_format.hpp"

#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "format_test_helpers.hpp"
#include "formats/document_formats.hpp"
#include "formats/image_formats.hpp"
#include "recovery/byte_order.hpp"
#include "support/image_builders.hpp"
#include "support/ole2_builders.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"
#include "validation/deflate_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::FileCandidate;
using carving::ValidationStatus;
using test::compoundFile;
using test::Ole2Options;
using test::Ole2Stream;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::headerOf;
using testing::isIntact;
using testing::verdictOf;

std::shared_ptr<const Ole2Format> makeFormat(Ole2Format::Limits limits = {}) {
    Result<std::shared_ptr<const Ole2Format>> made = Ole2Format::make(limits);
    return made.ok() ? *made : nullptr;
}

const Ole2Format& format() {
    static const std::shared_ptr<const Ole2Format> instance = makeFormat();
    return *instance;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;

// The offset of directory entry `id` of a version 3 file.
std::size_t entryAt(const Bytes& file, std::uint32_t id) {
    const std::uint32_t first = loadLe32(file, 0x30);
    return (std::size_t{first} + 1) * 512 + 128 * std::size_t{id};
}

// The offset of the FAT entry of sector `sector` of a version 3 file.
std::size_t fatEntryAt(const Bytes& file, std::uint32_t sector) {
    const std::uint32_t fat = loadLe32(file, 0x4C + 4 * (sector / 128));
    return (std::size_t{fat} + 1) * 512 + 4 * (sector % 128);
}

TEST(Ole2FormatTest, Descriptor) {
    const carving::FormatDescriptor& d = format().descriptor();
    EXPECT_EQ(d.id, "ole2");
    EXPECT_EQ(d.extension, "ole");
    EXPECT_FALSE(d.selfSynchronizing);
    EXPECT_FALSE(d.foundEndMeansContiguous);
    // Nothing checks a compound file's data: its files are never trusted.
    EXPECT_FALSE(d.trustValidFiles);
    ASSERT_EQ(d.signatures.size(), 1u);
    EXPECT_EQ(d.minimumSize, Ole2Format::kMinimumSize);
    EXPECT_EQ(d.maximumSize, Ole2Format::kMaximumSize);
    RECOVERY_EXPECT_OK(carving::validateDescriptor(d));
    RECOVERY_EXPECT_ERROR(Ole2Format::make({0, 1}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(Ole2Format::make({1, 0}), ErrorCode::InvalidInput);
}

TEST(Ole2FormatTest, FilesOfEveryKindAreIntactAndNamedByTheirStreams) {
    Ole2Options installer;
    installer.rootClass = {0x84, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
    Ole2Options version4;
    version4.version = 4;
    const std::vector<std::tuple<std::string, Bytes, std::string>> files = {
        {"Word", test::wordDocument(), "doc"},
        {"Excel", test::excelWorkbook(), "xls"},
        {"PowerPoint", test::powerPointPresentation(), "ppt"},
        {"Outlook", test::outlookMessage(), "msg"},
        {"Windows Installer", compoundFile({{"", "\x05SummaryInformation", test::makePattern(500, 9)}}, installer),
         "msi"},
        {"version 4", compoundFile({{"", "WordDocument", test::makePattern(20000, 10)}}, version4), "doc"},
        {"thumbnails", compoundFile({{"", "Catalog", test::makePattern(300, 11)}, {"", "1", test::makePattern(5000, 12)}}),
         "db"},
        {"unknown", compoundFile({{"", "Contents", test::makePattern(7000, 13)}}), ""},
    };
    for (const auto& [name, data, extension] : files) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(isIntact(format(), data));
        EXPECT_EQ(endOf(format(), data).extension, extension);
        EXPECT_TRUE(contains(verdictOf(format(), data).detail, "every chain consistent"))
            << verdictOf(format(), data).detail;
    }
}

TEST(Ole2FormatTest, TheDetailSaysWhatWasChecked) {
    const Bytes doc = test::wordDocument();
    EXPECT_TRUE(contains(endOf(format(), doc).detail, "sectors of 512 bytes")) << endOf(format(), doc).detail;
    EXPECT_TRUE(contains(verdictOf(format(), doc).detail, "5 streams, every chain consistent"))
        << verdictOf(format(), doc).detail;
}

TEST(Ole2FormatTest, ALargeFileWhoseFatNeedsDifatSectorsIsIntact) {
    // More than 109 FAT sectors: the DIFAT continues in sectors of its own.
    const Bytes big = compoundFile({{"", "WordDocument", test::makePattern(7'500'000, 21)}});
    ASSERT_GT(loadLe32(big, 0x48), 0u);
    EXPECT_TRUE(isIntact(format(), big));
}

TEST(Ole2FormatTest, TheFileEndsAtItsLastSectorInUse) {
    Ole2Options options;
    options.trailingFreeSectors = 3;
    const Bytes padded = compoundFile({{"", "Workbook", test::makePattern(9000, 31)}}, options);
    const carving::EndDetection end = endOf(format(), padded);
    EXPECT_EQ(end.status, EndStatus::Found);
    EXPECT_EQ(end.length, padded.size() - 3 * 512);
    const Bytes used(padded.begin(), padded.begin() + static_cast<std::ptrdiff_t>(end.length));
    EXPECT_TRUE(isIntact(format(), used));

    // A named file keeps them: free sectors its FAT covers are the file's,
    // the last one also when it is cut short.
    const carving::ValidationResult named = verdictOf(format(), padded);
    EXPECT_EQ(named.status, ValidationStatus::Valid) << named.detail;
    EXPECT_EQ(named.validBytes, padded.size());
    EXPECT_TRUE(contains(named.detail, "3 free sectors after the last one in use")) << named.detail;
    const Bytes cut(padded.begin(), padded.end() - 100);
    EXPECT_EQ(verdictOf(format(), cut).status, ValidationStatus::Valid) << verdictOf(format(), cut).detail;
    // Bytes beyond the sectors the FAT covers are no part of the file.
    const std::size_t covered = (std::size_t{loadLe32(padded, 0x2C)} * 128 + 1) * 512;
    ASSERT_GT(covered, padded.size());
    const Bytes longer = concat({padded, Bytes(covered - padded.size() + 1, std::byte{0})});
    const carving::ValidationResult beyond = verdictOf(format(), longer);
    EXPECT_EQ(beyond.status, ValidationStatus::Invalid) << beyond.detail;
    EXPECT_EQ(beyond.validBytes, end.length);
    const Bytes exactly(longer.begin(), longer.end() - 1);
    EXPECT_EQ(verdictOf(format(), exactly).status, ValidationStatus::Valid);
}

TEST(Ole2FormatTest, NoPrefixIsComplete) {
    for (const Bytes& data : {test::wordDocument(), test::outlookMessage()}) {
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

TEST(Ole2FormatTest, HeadersNoWriterMakesAreRejected) {
    const Bytes doc = test::wordDocument();
    EXPECT_TRUE(headerOf(format(), doc).plausible);
    const std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> damage = {
        {0x1C, {0xFF, 0xFE}},  // big-endian byte order
        {0x1E, {0x0C, 0x00}},  // 4096-byte sectors in a version 3 file
        {0x20, {0x07, 0x00}},  // 128-byte mini sectors
        {0x38, {0x00, 0x20}},  // another mini stream cutoff
        {0x2C, {0x00, 0x00}},  // no FAT sector
    };
    for (const auto& [at, bytes] : damage) {
        Bytes bad = doc;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bad[at + i] = static_cast<std::byte>(bytes[i]);
        }
        EXPECT_FALSE(headerOf(format(), bad).plausible) << at;
    }
}

TEST(Ole2FormatTest, DamagedFatsDirectoriesAndChainsAreFound) {
    const Bytes doc = test::wordDocument();
    // A FAT entry that names no sector the FAT covers: the walk breaks.
    {
        Bytes bad = doc;
        const std::size_t at = fatEntryAt(bad, 0);
        bad[at + 2] = std::byte{0x7F};
        const carving::EndDetection end = endOf(format(), bad);
        EXPECT_EQ(end.status, EndStatus::Broken) << end.detail;
    }
    // A stream's chain cut short: the walk ends where the FAT says, the
    // validation fails.
    {
        Bytes bad = doc;
        const std::uint32_t start = loadLe32(bad, entryAt(bad, 1) + 0x74);  // the first stream (by tree order)
        const std::size_t at = fatEntryAt(bad, start);
        for (std::size_t i = 0; i < 4; ++i) {
            bad[at + i] = static_cast<std::byte>((kEndOfChain >> (8 * i)) & 0xFF);
        }
        EXPECT_EQ(endOf(format(), bad).status, EndStatus::Found);
        EXPECT_EQ(verdictOf(format(), bad).status, ValidationStatus::Invalid);
    }
    // A directory entry of a type no compound file has.
    {
        Bytes bad = doc;
        bad[entryAt(bad, 2) + 0x42] = std::byte{9};
        EXPECT_EQ(verdictOf(format(), bad).status, ValidationStatus::Invalid);
    }
    // A tree that reaches an entry twice.
    {
        Bytes bad = doc;
        const std::size_t root = entryAt(bad, 0);
        const std::uint32_t child = loadLe32(bad, root + 0x4C);
        const std::size_t entry = entryAt(bad, child);
        for (std::size_t i = 0; i < 4; ++i) {
            bad[entry + 0x44 + i] = static_cast<std::byte>((child >> (8 * i)) & 0xFF);
        }
        const carving::ValidationResult verdict = verdictOf(format(), bad);
        EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
        EXPECT_TRUE(contains(verdict.detail, "twice")) << verdict.detail;
    }
    // The root is not the first entry.
    {
        Bytes bad = doc;
        bad[entryAt(bad, 0) + 0x42] = std::byte{1};
        EXPECT_EQ(verdictOf(format(), bad).status, ValidationStatus::Invalid);
    }
}

TEST(Ole2FormatTest, AChainThatRunsInALoopIsDamage) {
    // A version 4 file records 64-bit sizes: a chain followed as far as a
    // size asks would never end when it loops. A chain longer than its table
    // has sectors runs in a loop.
    test::Ole2Options v4;
    v4.version = 4;
    Bytes bad = compoundFile({{"", "WordDocument", test::makePattern(20000, 9)}}, v4);
    const std::size_t sector = 4096;
    const std::size_t entry = (std::size_t{loadLe32(bad, 0x30)} + 1) * sector + 128;  // entry 1: the stream
    const std::uint32_t start = loadLe32(bad, entry + 0x74);
    storeLe64(bad, entry + 0x78, std::uint64_t{1} << 50);
    // Its first sector chains to itself.
    storeLe32(bad, (std::size_t{loadLe32(bad, 0x4C)} + 1) * sector + 4 * std::size_t{start}, start);
    const carving::ValidationResult verdict = verdictOf(format(), bad);
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "runs in a loop")) << verdict.detail;
}

TEST(Ole2FormatTest, TheLimitsStopTheWorkNotTheCarve) {
    const Bytes doc = test::wordDocument();
    const std::shared_ptr<const Ole2Format> fewSectors = makeFormat({8, 1 << 20});
    ASSERT_NE(fewSectors, nullptr);
    EXPECT_EQ(endOf(*fewSectors, doc).status, EndStatus::Broken);
    const std::shared_ptr<const Ole2Format> fewEntries = makeFormat({1 << 24, 2});
    ASSERT_NE(fewEntries, nullptr);
    EXPECT_EQ(endOf(*fewEntries, doc).status, EndStatus::Found);
    EXPECT_EQ(verdictOf(*fewEntries, doc).status, ValidationStatus::NotValidated);
}

// Carving: every image format and the documents.
testing::Carved carveWithDocuments(storage::IStorageSource& source) {
    testing::Carved carved;
    carving::FormatRegistry registry;
    const bool images = registerImageFormats(registry).ok();
    const bool documents = registerDocumentFormats(registry, validation::deflateDecoder()).ok();
    EXPECT_TRUE(images && documents);
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

TEST(Ole2FormatTest, ACompoundFileIsCarvedAndNamedByWhatItHolds) {
    const Bytes doc = test::wordDocument();
    test::VirtualSource source(256 * 1024);
    source.plant(8192, doc);
    RECOVERY_ASSERT_OK(source.open());
    const testing::Carved carved = carveWithDocuments(source);
    ASSERT_EQ(carved.candidates.size(), 1u);
    EXPECT_EQ(carved.candidates[0].formatId, "ole2");
    EXPECT_EQ(carved.candidates[0].extension, "doc");
    EXPECT_EQ(carved.candidates[0].length, doc.size());
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Valid);
}

TEST(Ole2FormatTest, TheFilesInsideACompoundFileAreCarvedAsIfItWereNotThere) {
    // A picture in a document's stream: the document is valid, but nothing
    // shows the picture's bytes are its own (another file may have taken
    // those sectors since), so the picture is carved as before.
    const Bytes png = test::makePng(test::PngOptions{});
    Bytes data(std::max<std::size_t>(5000, 1024 + png.size()), std::byte{0x20});
    std::copy(png.begin(), png.end(), data.begin() + 512);
    const Bytes doc = compoundFile({{"", "WordDocument", test::makePattern(6000, 41)}, {"", "Data", data}});
    ASSERT_TRUE(isIntact(format(), doc));
    test::VirtualSource source(256 * 1024);
    source.plant(4096, doc);
    RECOVERY_ASSERT_OK(source.open());
    const testing::Carved carved = carveWithDocuments(source);
    ASSERT_EQ(carved.candidates.size(), 2u);
    EXPECT_EQ(carved.candidates[0].formatId, "ole2");
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Valid);
    EXPECT_EQ(carved.candidates[1].formatId, "png");
    EXPECT_EQ(carved.candidates[1].length, png.size());
    EXPECT_EQ(carved.report.skippedInsideCandidates, 0u);
}

TEST(Ole2FormatTest, Fuzzing) {
    std::mt19937_64 random(20261015);
    for (const Bytes& data : {test::wordDocument(), test::outlookMessage(), test::powerPointPresentation()}) {
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
