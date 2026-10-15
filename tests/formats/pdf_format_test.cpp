// PDF: documents built here as writers write them (one revision, CR LF
// lines, incremental updates, a linearized layout, cross-reference streams
// and object streams, hybrid files, lengths in objects of their own, JPEG
// images); every prefix; damaged objects, streams, tables and trailers;
// what cannot be checked (other filters, encryption, binary data); headers
// no writer makes; the limits; carving with the image formats (the images of
// a verified document are part of it); fuzzing.

#include "formats/pdf_format.hpp"

#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "format_test_helpers.hpp"
#include "formats/document_formats.hpp"
#include "formats/image_formats.hpp"
#include "support/image_builders.hpp"
#include "support/pdf_builders.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"
#include "validation/deflate_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
using test::compressedPdf;
using test::linearizedPdf;
using test::PdfWriter;
using test::storedZlib;
using test::textBytes;
using test::updatedPdf;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::headerOf;
using testing::isIntact;
using testing::verdictOf;

std::shared_ptr<const PdfFormat> makeFormat(PdfFormat::Limits limits = {}) {
    Result<std::shared_ptr<const PdfFormat>> made = PdfFormat::make(validation::deflateDecoder(), limits);
    return made.ok() ? *made : nullptr;
}

const PdfFormat& format() {
    static const std::shared_ptr<const PdfFormat> instance = makeFormat();
    return *instance;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::size_t find(const Bytes& data, std::string_view text, std::size_t from = 0) {
    const Bytes pattern = textBytes(text);
    const auto at = std::search(data.begin() + static_cast<std::ptrdiff_t>(from), data.end(), pattern.begin(),
                                pattern.end());
    return at == data.end() ? std::string::npos : static_cast<std::size_t>(at - data.begin());
}

Bytes jpeg() {
    return test::makeJpeg(test::JpegOptions{});
}

TEST(PdfFormatTest, Descriptor) {
    const carving::FormatDescriptor& d = format().descriptor();
    EXPECT_EQ(d.id, "pdf");
    EXPECT_EQ(d.extension, "pdf");
    EXPECT_FALSE(d.selfSynchronizing);
    EXPECT_TRUE(d.foundEndMeansContiguous);
    EXPECT_TRUE(d.trustValidFiles);
    ASSERT_EQ(d.signatures.size(), 1u);
    EXPECT_EQ(d.signatures[0].offset, 0u);
    EXPECT_EQ(d.minimumSize, PdfFormat::kMinimumSize);
    EXPECT_EQ(d.maximumSize, PdfFormat::kMaximumSize);
    EXPECT_EQ(d.endDetection, carving::EndDetectionMethod::StructureWalk);
    RECOVERY_EXPECT_OK(carving::validateDescriptor(d));
}

TEST(PdfFormatTest, TheFormatNeedsADecoderAndLimits) {
    RECOVERY_EXPECT_ERROR(PdfFormat::make(nullptr), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(PdfFormat::make(validation::deflateDecoder(), {0, 1, 1}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(PdfFormat::make(validation::deflateDecoder(), {1, 0, 1}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(PdfFormat::make(validation::deflateDecoder(), {1, 1, 0}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(PdfFormat::make(validation::deflateDecoder(), {1, 1, 1, 0}), ErrorCode::InvalidInput);

    carving::FormatRegistry registry;
    RECOVERY_EXPECT_ERROR(registerDocumentFormats(registry, nullptr), ErrorCode::InvalidInput);
    EXPECT_TRUE(registry.empty());
    RECOVERY_ASSERT_OK(registerDocumentFormats(registry, validation::deflateDecoder()));
    ASSERT_EQ(registry.size(), 2u);
    EXPECT_NE(registry.find("pdf"), nullptr);
    EXPECT_NE(registry.find("ole2"), nullptr);
    RECOVERY_EXPECT_ERROR(registerDocumentFormats(registry, validation::deflateDecoder()), ErrorCode::InvalidInput);
    EXPECT_EQ(registry.size(), 2u);
}

TEST(PdfFormatTest, DocumentsOfEveryLayoutAreIntact) {
    const std::vector<std::pair<std::string, Bytes>> documents = {
        {"one revision", test::simplePdf()},
        {"with a JPEG image", test::simplePdf("photo", jpeg())},
        {"three revisions", updatedPdf()},
        {"cross-reference stream", compressedPdf()},
        {"cross-reference stream without predictor", compressedPdf(false)},
        {"linearized", linearizedPdf()},
        {"hybrid reference", test::hybridPdf()},
    };
    for (const auto& [name, data] : documents) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(isIntact(format(), data));
        const carving::ValidationResult verdict = verdictOf(format(), data);
        EXPECT_TRUE(contains(verdict.detail, "every stream checked")) << verdict.detail;
    }
}

TEST(PdfFormatTest, TheDetailSaysWhatWasChecked) {
    EXPECT_EQ(endOf(format(), test::simplePdf()).detail, "6 objects, 2 streams, 1 revision");
    EXPECT_EQ(verdictOf(format(), test::simplePdf("x", jpeg())).detail,
              "7 objects, 3 streams, 1 revision, every stream checked");
    EXPECT_TRUE(contains(endOf(format(), updatedPdf()).detail, "3 revisions"));
    EXPECT_TRUE(contains(endOf(format(), linearizedPdf()).detail, "2 revisions, linearized"));
}

TEST(PdfFormatTest, LinesEndingInCarriageReturnLineFeedAreRead) {
    PdfWriter pdf("1.3", "\r\n");
    pdf.object("<</Type /Catalog /Pages 2 0 R>>");
    pdf.object("<</Type /Pages /Kids [] /Count 0>>");
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes("BT ET")));
    pdf.endWithTable(1);
    EXPECT_TRUE(isIntact(format(), pdf.bytes()));
}

TEST(PdfFormatTest, LengthsInObjectsOfTheirOwnAreChecked) {
    PdfWriter pdf;
    pdf.object("<</Type /Catalog /Pages 2 0 R>>");
    pdf.object("<</Type /Pages /Kids [] /Count 0>>");
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes("BT (indirect) Tj ET")), true);
    pdf.endWithTable(1);
    const Bytes good = pdf.bytes();
    EXPECT_TRUE(isIntact(format(), good));

    // The length object says one byte more than the data holds.
    const std::size_t at = find(good, "4 0 obj");
    ASSERT_NE(at, std::string::npos);
    const std::size_t value = at + std::string_view("4 0 obj\n").size();
    Bytes wrong = good;
    // (One more would be the end of line before "endstream", which writers count at times.)
    const std::string length = std::to_string(std::stoul(std::string(reinterpret_cast<const char*>(&good[value]), 2)) + 5);
    ASSERT_EQ(length.size(), 2u);
    wrong[value] = static_cast<std::byte>(length[0]);
    wrong[value + 1] = static_cast<std::byte>(length[1]);
    EXPECT_EQ(endOf(format(), wrong).status, EndStatus::Broken);
    EXPECT_EQ(verdictOf(format(), wrong).status, ValidationStatus::Invalid);
}

TEST(PdfFormatTest, NoPrefixIsComplete) {
    for (const Bytes& data :
         {test::simplePdf("x", jpeg()), compressedPdf(), updatedPdf(), linearizedPdf(), test::hybridPdf()}) {
        // A revision's end is a complete document, and so is the file without
        // its last end of line.
        std::vector<std::size_t> complete = {data.size() - 1};
        for (std::size_t at = find(data, "%%EOF\n"); at != std::string::npos; at = find(data, "%%EOF\n", at + 1)) {
            complete.push_back(at + 5);
            complete.push_back(at + 6);
        }
        for (std::size_t length = 0; length < data.size(); length += (length < 4096 ? 1 : 97)) {
            const std::span<const std::byte> prefix(data.data(), length);
            const carving::EndDetection end = endOf(format(), prefix);
            ASSERT_LE(end.length, length);
            const carving::ValidationResult verdict = verdictOf(format(), prefix);
            ASSERT_LE(verdict.validBytes, length);
            if (std::find(complete.begin(), complete.end(), length) != complete.end()) {
                continue;
            }
            ASSERT_NE(end.status, EndStatus::Found) << "prefix " << length << ": " << testing::describe(end);
            ASSERT_NE(verdict.status, ValidationStatus::Valid) << "prefix " << length;
        }
    }
}

TEST(PdfFormatTest, AnEarlierRevisionIsADocumentOfItsOwn) {
    const Bytes data = updatedPdf();
    const std::size_t first = find(data, "%%EOF\n") + 6;
    const Bytes original(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(first));
    EXPECT_TRUE(isIntact(format(), original));
}

TEST(PdfFormatTest, WhatFollowsTheLastRevisionIsNotTheDocuments) {
    const Bytes data = test::simplePdf();
    // What follows, and how many of its first bytes (blank lines) are the document's.
    const std::vector<std::pair<Bytes, std::size_t>> cases = {
        {Bytes(3000, std::byte{0}), 0},
        {concat({textBytes("Z"), testing::quietNoise(3000, 7)}), 0},
        {textBytes("\n\r\n  trailing words"), 5},
        // An older document's objects, left after this one: they do not
        // chain to its sections.
        {concat({textBytes("7 0 obj\n<</Old true>>\nendobj\n"), test::simplePdf("older")}), 0},
    };
    for (const auto& [after, blank] : cases) {
        const Bytes file = concat({data, after});
        const carving::EndDetection end = endOf(format(), file);
        EXPECT_EQ(end.status, EndStatus::Found) << end.detail;
        EXPECT_EQ(end.length, data.size() + blank);
    }
}

TEST(PdfFormatTest, PaddingAfterTheLastEofIsTheFilesWhenItRunsToItsEnd) {
    const Bytes data = test::simplePdf();
    EXPECT_EQ(verdictOf(format(), concat({data, Bytes(500, std::byte{0})})).status, ValidationStatus::Valid);
    EXPECT_EQ(verdictOf(format(), concat({data, textBytes("\r\n\r\n")})).status, ValidationStatus::Valid);
    EXPECT_EQ(verdictOf(format(), concat({data, textBytes("<html>")})).status, ValidationStatus::Invalid);
}

TEST(PdfFormatTest, DamagedStreamsTablesAndTrailersAreInvalid) {
    const Bytes data = test::simplePdf("damage", jpeg());
    // Flate data: a byte of the stored block changes, the Adler-32 does not.
    const std::size_t content = find(data, "BT /F1");
    ASSERT_NE(content, std::string::npos);
    Bytes flate = data;
    flate[content + 3] ^= std::byte{0x01};
    const carving::ValidationResult flateVerdict = verdictOf(format(), flate);
    EXPECT_EQ(flateVerdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(flateVerdict.detail, "Flate data")) << flateVerdict.detail;

    // The JPEG image: its frame header's marker is no marker.
    const std::size_t sof = find(data, "\xFF\xC0");
    ASSERT_NE(sof, std::string::npos);
    Bytes image = data;
    image[sof + 1] = std::byte{0x00};
    EXPECT_EQ(verdictOf(format(), image).status, ValidationStatus::Invalid);

    // A cross-reference entry's offset.
    const std::size_t entry = find(data, " 00000 n", find(data, "xref")) - 1;
    Bytes table = data;
    table[entry] = static_cast<std::byte>(table[entry] == std::byte{'1'} ? '2' : '1');
    const carving::ValidationResult tableVerdict = verdictOf(format(), table);
    EXPECT_EQ(tableVerdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(tableVerdict.detail, "cross-reference entry")) << tableVerdict.detail;

    // startxref.
    const std::size_t startxref = find(data, "startxref\n") + 10;
    Bytes pointer = data;
    pointer[startxref] = static_cast<std::byte>(pointer[startxref] == std::byte{'1'} ? '2' : '1');
    EXPECT_EQ(verdictOf(format(), pointer).status, ValidationStatus::Invalid);

    // Every one of them is still found whole: damage in place, not a gap.
    for (const Bytes* damaged : {&flate, &image, &table, &pointer}) {
        EXPECT_EQ(endOf(format(), *damaged).status, EndStatus::Found);
    }
}

// The document with every offset its cross-reference table (and, with
// `startxref`, startxref) records moved by `delta` bytes.
Bytes withOffsetsMoved(const Bytes& data, int delta, bool startxref = true) {
    Bytes moved = data;
    const auto rewrite = [&](std::size_t at, std::size_t digits) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            value = value * 10 + (static_cast<std::uint8_t>(moved[at + i]) - '0');
        }
        std::string text = std::to_string(static_cast<std::int64_t>(value) + delta);
        text.insert(0, digits - text.size(), '0');
        for (std::size_t i = 0; i < digits; ++i) {
            moved[at + i] = static_cast<std::byte>(text[i]);
        }
    };
    for (std::size_t at = find(data, " 00000 n", find(data, "xref\n")); at != std::string::npos;
         at = find(data, " 00000 n", at + 1)) {
        rewrite(at - 10, 10);
    }
    if (startxref) {
        const std::size_t at = find(data, "startxref\n") + 10;
        rewrite(at, find(data, "\n", at) - at);
    }
    return moved;
}

TEST(PdfFormatTest, OffsetsAtTheWhitespaceBeforeAnObjectAreTheObjects) {
    // Readers skip whitespace and comments where an offset points, and
    // Microsoft Print to PDF records every offset at the end of line before
    // its object (one byte early).
    const Bytes data = test::simplePdf("off by one");
    const Bytes early = withOffsetsMoved(data, -1);
    EXPECT_TRUE(isIntact(format(), early));
    // An offset inside an object's header, in the object before or in the
    // comment line before the first one is not one.
    EXPECT_EQ(verdictOf(format(), withOffsetsMoved(data, 1, false)).status, ValidationStatus::Invalid);
    const carving::ValidationResult earlier = verdictOf(format(), withOffsetsMoved(data, -2, false));
    EXPECT_EQ(earlier.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(earlier.detail, "cross-reference entry")) << earlier.detail;
}

TEST(PdfFormatTest, AHybridReferenceFileIsCheckedThroughBothSections) {
    const Bytes data = test::hybridPdf();
    EXPECT_TRUE(isIntact(format(), data));
    EXPECT_TRUE(contains(verdictOf(format(), data).detail, "every stream checked"));
    // /XRefStm pointing elsewhere.
    const std::size_t at = find(data, "/XRefStm ") + 9;
    Bytes wrong = data;
    wrong[at] = static_cast<std::byte>(wrong[at] == std::byte{'1'} ? '2' : '1');
    const carving::ValidationResult verdict = verdictOf(format(), wrong);
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "/XRefStm")) << verdict.detail;
}

TEST(PdfFormatTest, AnObjectStreamThatDoesNotHoldWhatItsEntriesSayIsInvalid) {
    for (const Bytes& data : {compressedPdf(), test::hybridPdf()}) {
        const std::size_t at = find(data, "/N 2");
        ASSERT_NE(at, std::string::npos);
        Bytes fewer = data;
        fewer[at + 3] = std::byte{'1'};
        EXPECT_EQ(endOf(format(), fewer).status, EndStatus::Found);
        const carving::ValidationResult verdict = verdictOf(format(), fewer);
        EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
        EXPECT_TRUE(contains(verdict.detail, "which does not hold it")) << verdict.detail;
    }
}

TEST(PdfFormatTest, IndexCountsThatAddUpPastSixtyFourBitsListTooManyEntries) {
    // Nineteen /Index counts of 18 digits whose sum wraps around to 5: the 5
    // entries the stream holds must not pass for them (they would be read
    // past the stream's data).
    PdfWriter pdf;
    const std::uint32_t root = pdf.object("<</Type /Catalog /Pages 2 0 R>>");
    pdf.object("<</Type /Pages /Kids [] /Count 0>>");
    std::string index;
    for (int i = 0; i < 18; ++i) {
        index += "0 970881267037344822 ";
    }
    index += "0 970881267037344825";
    pdf.stream("/Type /XRef /W [1 2 1] /Size 5 /Index [" + index + "]", Bytes(20, std::byte{0}));
    pdf.endWithTable(root);
    EXPECT_EQ(endOf(format(), pdf.bytes()).status, EndStatus::Found);
    const carving::ValidationResult verdict = verdictOf(format(), pdf.bytes());
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid);
    EXPECT_TRUE(contains(verdict.detail, "fewer entries than its /Index lists")) << verdict.detail;
}

TEST(PdfFormatTest, AnEncryptedDocumentsCrossReferenceStreamIsStillChecked) {
    // Encryption leaves cross-reference streams in the clear (ISO 32000-1 7.6.1).
    PdfWriter pdf("1.5");
    pdf.object("<</Type /Catalog /Pages 2 0 R>>");
    pdf.object("<</Type /Pages /Kids [3 0 R] /Count 1>>");
    pdf.object("<</Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R>>");
    pdf.stream("/Filter /FlateDecode", testing::noise(400, 5));
    pdf.object("<</Filter /Standard /V 4 /R 4 /O <00> /U <00> /P -4>>");
    pdf.endWithStream(1, true, " /Encrypt 5 0 R");
    const Bytes data = pdf.bytes();
    EXPECT_EQ(endOf(format(), data).status, EndStatus::Found);
    const carving::ValidationResult verdict = verdictOf(format(), data);
    EXPECT_EQ(verdict.status, ValidationStatus::NotValidated) << verdict.detail;
    EXPECT_TRUE(contains(verdict.detail, "encrypted")) << verdict.detail;
    // Its entries still have to be right.
    const std::size_t page = find(data, "3 0 obj");
    Bytes wrong = data;
    wrong[page + 2] = std::byte{'1'};
    EXPECT_EQ(verdictOf(format(), wrong).status, ValidationStatus::Invalid);
}

TEST(PdfFormatTest, ABrokenObjectBreaksTheWalk) {
    const Bytes data = test::simplePdf();
    const std::size_t page = find(data, "4 0 obj");
    Bytes broken = data;
    broken[page + 2] = std::byte{'X'};
    const carving::EndDetection end = endOf(format(), broken);
    EXPECT_EQ(end.status, EndStatus::Broken);
    EXPECT_EQ(end.length, page);

    // A /Length that reaches beyond the data breaks the walk where the
    // object starts, instead of making the document reach the source's end.
    const std::size_t length = find(data, "/Length ", find(data, "5 0 obj")) + 8;
    Bytes huge = data;
    huge[length] = std::byte{'9'};
    huge.insert(huge.begin() + static_cast<std::ptrdiff_t>(length), 8, std::byte{'9'});
    const carving::EndDetection hugeEnd = endOf(format(), concat({huge, Bytes(4096, std::byte{0})}));
    EXPECT_EQ(hugeEnd.status, EndStatus::Broken);
    EXPECT_EQ(hugeEnd.length, find(huge, "5 0 obj"));
}

TEST(PdfFormatTest, StreamsTheEngineCannotCheckLeaveTheDocumentUnverified) {
    struct Case {
        std::string name;
        Bytes data;
        std::string_view reason;
    };
    std::vector<Case> cases;
    {
        PdfWriter pdf;
        pdf.object("<</Type /Catalog /Pages 2 0 R>>");
        pdf.object("<</Type /Pages /Kids [] /Count 0>>");
        pdf.stream("/Filter /LZWDecode", Bytes(300, std::byte{0x80}));
        pdf.endWithTable(1);
        cases.push_back({"LZW", pdf.bytes(), "LZWDecode"});
    }
    {
        PdfWriter pdf;
        pdf.object("<</Type /Catalog /Pages 2 0 R>>");
        pdf.object("<</Type /Pages /Kids [] /Count 0>>");
        pdf.stream("/Filter /FlateDecode", testing::noise(500, 3));
        pdf.object("<</Filter /Standard /V 2 /R 3 /O <00> /U <00> /P -4>>");
        pdf.endWithTable(1, " /Encrypt 4 0 R");
        cases.push_back({"encrypted", pdf.bytes(), "encrypted"});
    }
    {
        PdfWriter pdf;
        pdf.object("<</Type /Catalog /Pages 2 0 R>>");
        pdf.object("<</Type /Pages /Kids [] /Count 0>>");
        pdf.stream("/Type /XObject /Subtype /Image", testing::noise(500, 4));
        pdf.endWithTable(1);
        cases.push_back({"binary data without a filter", pdf.bytes(), "binary data without a filter"});
    }
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(endOf(format(), c.data).status, EndStatus::Found);
        const carving::ValidationResult verdict = verdictOf(format(), c.data);
        EXPECT_EQ(verdict.status, ValidationStatus::NotValidated) << verdict.detail;
        EXPECT_TRUE(contains(verdict.detail, c.reason)) << verdict.detail;
    }
}

TEST(PdfFormatTest, HeadersNoWriterMakesAreRejected) {
    EXPECT_TRUE(headerOf(format(), test::simplePdf()).plausible);
    for (const std::string_view text :
         {"%PDF-\n1 0 obj\n", "%PDF-x.4\n1 0 obj\n", "%PDF-1.\n1 0 obj\n",
          // Text that mentions a header, not a document.
          "%PDF-1.4 is the version this tool writes, see the manual for more about it.\n"}) {
        Bytes header = textBytes(text);
        header.resize(4096, std::byte{' '});
        EXPECT_FALSE(headerOf(format(), header).plausible) << text;
    }
}

TEST(PdfFormatTest, TheLimitsStopTheWorkNotTheCarve) {
    const Bytes data = test::simplePdf("limits", jpeg());
    const std::shared_ptr<const PdfFormat> few = makeFormat({2, 4 * kGiB, 1 * kGiB});
    ASSERT_NE(few, nullptr);
    EXPECT_EQ(endOf(*few, data).status, EndStatus::Found);
    EXPECT_EQ(verdictOf(*few, data).status, ValidationStatus::NotValidated);
    const std::shared_ptr<const PdfFormat> small = makeFormat({1 << 20, 10, 1 * kGiB});
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(verdictOf(*small, data).status, ValidationStatus::NotValidated);

    PdfWriter pdf;
    pdf.object("<</Type /Catalog /Pages 2 0 R>>");
    pdf.object("<</Type /Pages /Kids [] /Count 0>>");
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes(std::string(3000, 'a'))), true);
    pdf.endWithTable(1);
    const std::shared_ptr<const PdfFormat> searching = makeFormat({1 << 20, 4 * kGiB, 1000});
    ASSERT_NE(searching, nullptr);
    EXPECT_EQ(endOf(*searching, pdf.bytes()).status, EndStatus::Broken);
    EXPECT_EQ(endOf(format(), pdf.bytes()).status, EndStatus::Found);

    // Each revision's end follows the chain of cross-reference sections: their
    // number bounds the walk's time. Beyond the limit the walk breaks.
    const std::shared_ptr<const PdfFormat> fewRevisions = makeFormat({1 << 20, 4 * kGiB, 1 * kGiB, 2});
    ASSERT_NE(fewRevisions, nullptr);
    const carving::EndDetection cut = endOf(*fewRevisions, updatedPdf());
    EXPECT_EQ(cut.status, EndStatus::Broken);
    EXPECT_TRUE(contains(cut.detail, "more than 2")) << cut.detail;
    EXPECT_EQ(endOf(format(), updatedPdf()).status, EndStatus::Found);
    // Cross-reference streams unpack within the document's budget too.
    const carving::ValidationResult packed = verdictOf(*small, compressedPdf());
    EXPECT_EQ(packed.status, ValidationStatus::NotValidated);
    EXPECT_TRUE(contains(packed.detail, "the document's streams may unpack to")) << packed.detail;
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

TEST(PdfFormatTest, TheImagesOfAVerifiedDocumentArePartOfIt) {
    const Bytes data = test::simplePdf("photo", jpeg());
    test::VirtualSource source(256 * 1024);
    source.plant(4096, data);
    RECOVERY_ASSERT_OK(source.open());
    const testing::Carved carved = carveWithDocuments(source);
    ASSERT_EQ(carved.candidates.size(), 1u);
    EXPECT_EQ(carved.candidates[0].formatId, "pdf");
    EXPECT_EQ(carved.candidates[0].extension, "pdf");
    EXPECT_EQ(carved.candidates[0].length, data.size());
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Valid);
    EXPECT_GE(carved.report.skippedInsideCandidates, 1u);
}

TEST(PdfFormatTest, TheImagesOfAnUncheckedOrDamagedDocumentAreCarvedAsBefore) {
    const Bytes image = jpeg();
    // A stream nothing checks: the document is kept, never trusted.
    PdfWriter unchecked;
    unchecked.object("<</Type /Catalog /Pages 2 0 R>>");
    unchecked.object("<</Type /Pages /Kids [] /Count 0>>");
    unchecked.stream("/Filter /JBIG2Decode", Bytes(200, std::byte{0x80}));
    unchecked.stream("/Subtype /Image /Filter /DCTDecode", image);
    unchecked.endWithTable(1);
    // A damaged Flate stream.
    Bytes damaged = test::simplePdf("photo", image);
    damaged[find(damaged, "BT /F1") + 3] ^= std::byte{0x01};
    for (const Bytes& data : {unchecked.bytes(), damaged}) {
        test::VirtualSource source(256 * 1024);
        source.plant(4096, data);
        RECOVERY_ASSERT_OK(source.open());
        const testing::Carved carved = carveWithDocuments(source);
        ASSERT_EQ(carved.candidates.size(), 2u);
        EXPECT_EQ(carved.candidates[0].formatId, "pdf");
        EXPECT_NE(carved.candidates[0].validation.status, ValidationStatus::Valid);
        EXPECT_EQ(carved.candidates[1].formatId, "jpeg");
        EXPECT_EQ(carved.candidates[1].sourceOffset, 4096 + find(data, "\xFF\xD8\xFF"));
        EXPECT_EQ(carved.report.skippedInsideCandidates, 0u);
    }
}

TEST(PdfFormatTest, DocumentsOneAfterAnotherAreCarvedApart) {
    const Bytes first = test::simplePdf("first");
    const Bytes second = updatedPdf();
    test::VirtualSource source(64 * 1024);
    source.plant(512, first);
    source.plant(512 + ((first.size() + 511) / 512) * 512, second);
    RECOVERY_ASSERT_OK(source.open());
    const testing::Carved carved = carveWithDocuments(source);
    ASSERT_EQ(carved.candidates.size(), 2u);
    EXPECT_EQ(carved.candidates[0].length, first.size());
    EXPECT_EQ(carved.candidates[1].length, second.size());
    EXPECT_EQ(carved.candidates[1].validation.status, ValidationStatus::Valid);
}

TEST(PdfFormatTest, Fuzzing) {
    std::mt19937_64 random(20261014);
    for (const Bytes& data :
         {test::simplePdf("fuzz", jpeg()), updatedPdf(), compressedPdf(), linearizedPdf(), test::hybridPdf()}) {
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
