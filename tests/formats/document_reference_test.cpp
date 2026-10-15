// Cross-checks against documents this project did not write.
//
// DocumentSamplesTest runs always: every embedded sample (documents that
// Microsoft Edge, Microsoft Print to PDF, the XPS Document Writer, Windows'
// structured storage, Windows Installer and qpdf wrote) must be recognised,
// validated, named and carved back byte for byte.
//
// DocumentReferenceTest reads folders of documents made elsewhere: the
// Windows Installer packages in C:\Windows\Installer, documents that Office,
// LibreOffice, a browser or a PDF printer wrote. It is skipped unless
// RECOVERY_DOCUMENT_REFERENCE_DIR names folders (';' between them; their
// subfolders are read too) of .pdf, OLE2 (.doc, .xls, .ppt, .msg, .msi, ...)
// and ZIP-based (.docx, .xlsx, .odt, .epub, .apk, ...) files; see
// docs/testing/testing.md. A file a program wrote must never be called
// damaged, must be carved back to its end, and named by its content as its
// name says (a .jar file whose members say nothing of Java: as a plain
// archive).
//
// DocumentBuilderExport writes the test builders' own files to
// RECOVERY_DOCUMENT_EXPORT_DIR (skipped otherwise), so that independent
// readers can check them: tests/reference/check_document_builder_files.sh.

#include "formats/archive_formats.hpp"
#include "formats/document_formats.hpp"

#include "carving/signature_scanner.hpp"
#include "format_test_helpers.hpp"
#include "formats/image_formats.hpp"
#include "recovery/byte_order.hpp"
#include "support/document_samples.hpp"
#include "support/image_builders.hpp"
#include "support/ole2_builders.hpp"
#include "support/pdf_builders.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"
#include "validation/deflate_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::IFileFormat;
using carving::ValidationStatus;
using testing::Bytes;

const IFileFormat& formatWithId(std::string_view id) {
    static const std::vector<std::shared_ptr<const IFileFormat>> formats = [] {
        std::vector<std::shared_ptr<const IFileFormat>> all;
        Result<std::vector<std::shared_ptr<const IFileFormat>>> documents =
            documentFormats(validation::deflateDecoder());
        Result<std::vector<std::shared_ptr<const IFileFormat>>> archives =
            archiveFormats(validation::deflateDecoder());
        if (documents.ok() && archives.ok()) {
            all = *documents;
            all.insert(all.end(), archives->begin(), archives->end());
        }
        return all;
    }();
    for (const auto& format : formats) {
        if (format->descriptor().id == id) {
            return *format;
        }
    }
    std::abort();
}

// What the engine makes of a file named with `extension` (lower case, no
// dot): the format that carves it, and the extension its content gives.
struct Expected {
    std::string_view extension;
    std::string_view formatId;
    std::string_view named;
};

constexpr std::array<Expected, 40> kExpected = {{
    {"pdf", "pdf", ""},
    {"doc", "ole2", "doc"},
    {"dot", "ole2", "doc"},
    {"xls", "ole2", "xls"},
    {"xlt", "ole2", "xls"},
    {"ppt", "ole2", "ppt"},
    {"pps", "ole2", "ppt"},
    {"pot", "ole2", "ppt"},
    {"msg", "ole2", "msg"},
    {"msi", "ole2", "msi"},
    {"msp", "ole2", "msp"},
    {"mst", "ole2", "mst"},
    {"vsd", "ole2", "vsd"},
    {"pub", "ole2", "pub"},
    {"db", "ole2", "db"},
    {"docx", "zip", "docx"},
    {"dotx", "zip", "docx"},
    {"docm", "zip", "docm"},
    {"dotm", "zip", "docm"},
    {"xlsx", "zip", "xlsx"},
    {"xltx", "zip", "xlsx"},
    {"xlsm", "zip", "xlsm"},
    {"xltm", "zip", "xlsm"},
    {"pptx", "zip", "pptx"},
    {"ppsx", "zip", "pptx"},
    {"potx", "zip", "pptx"},
    {"pptm", "zip", "pptm"},
    {"ppsm", "zip", "pptm"},
    {"vsdx", "zip", "vsdx"},
    {"xps", "zip", "xps"},
    {"oxps", "zip", "xps"},
    {"odt", "zip", "odt"},
    {"ods", "zip", "ods"},
    {"odp", "zip", "odp"},
    {"odg", "zip", "odg"},
    {"epub", "zip", "epub"},
    {"jar", "zip", "jar"},
    {"apk", "zip", "apk"},
    {"ott", "zip", ""},
    {"zip", "zip", ""},
}};

const Expected* expectedFor(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension.size() < 2) {
        return nullptr;
    }
    const std::string_view name = std::string_view(extension).substr(1);
    for (const Expected& expected : kExpected) {
        if (expected.extension == name) {
            return &expected;
        }
    }
    return nullptr;
}

// What a .jar file's members say, read here from its central directory: a
// JAR when a member is its manifest or its first member is in META-INF/ or a
// class file; else a plain archive (the resource archives of IntelliJ and
// Android Studio hold only XML). Only the names count: a pom.xml may hold the
// text "META-INF/MANIFEST.MF".
std::string_view jarNamed(const Bytes& bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::size_t end = text.rfind("PK\x05\x06");
    if (end == std::string_view::npos || end + 22 > bytes.size()) {
        return "";
    }
    const std::size_t entries = loadLe16(bytes, end + 10);
    std::size_t at = loadLe32(bytes, end + 16);
    std::string_view first;
    std::uint64_t firstOffset = ~std::uint64_t{0};
    for (std::size_t i = 0; i < entries && at + 46 <= bytes.size() && loadLe32(bytes, at) == 0x02014B50; ++i) {
        const std::size_t name = loadLe16(bytes, at + 28);
        if (at + 46 + name > bytes.size()) {
            break;
        }
        const std::string_view member = text.substr(at + 46, name);
        if (member == "META-INF/MANIFEST.MF") {
            return "jar";
        }
        if (loadLe32(bytes, at + 42) < firstOffset) {
            firstOffset = loadLe32(bytes, at + 42);
            first = member;
        }
        at += 46 + name + loadLe16(bytes, at + 30) + loadLe16(bytes, at + 32);
    }
    return first.starts_with("META-INF/") || first.ends_with(".class") ? "jar" : "";
}

std::vector<std::filesystem::path> foldersFromEnvironment(const wchar_t* name) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    std::vector<std::filesystem::path> folders;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return folders;
    }
    std::wstring text(value);
    std::free(value);
    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t end = std::min(text.find(L';', start), text.size());
        if (end > start) {
            folders.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return folders;
}

// What each embedded sample must be found to be.
struct SampleVerdict {
    std::string_view name;
    ValidationStatus status;
    // The extension its content gives (empty: the format's own).
    std::string_view extension;
};

constexpr std::array<SampleVerdict, 11> kSampleVerdicts = {{
    {"edge_print.pdf", ValidationStatus::Valid, ""},
    {"mspdf_shapes.pdf", ValidationStatus::Valid, ""},
    // Encrypted: its streams are ciphertext.
    {"qpdf_aes256.pdf", ValidationStatus::NotValidated, ""},
    {"qpdf_linearized.pdf", ValidationStatus::Valid, ""},
    {"qpdf_linearized_objstm.pdf", ValidationStatus::Valid, ""},
    {"qpdf_objstm.pdf", ValidationStatus::Valid, ""},
    {"stg_excel_v4.xls", ValidationStatus::Valid, "xls"},
    {"stg_shrunk.doc", ValidationStatus::Valid, "doc"},
    {"stg_word.doc", ValidationStatus::Valid, "doc"},
    {"wi_created.msi", ValidationStatus::Valid, "msi"},
    {"xps_shapes.oxps", ValidationStatus::Valid, "xps"},
}};

TEST(DocumentSamplesTest, EverySampleIsRecognisedValidatedNamedAndCarvedBack) {
    namespace samples = test::document_samples;
    ASSERT_EQ(samples::all().size(), kSampleVerdicts.size());
    std::uint64_t size = 1 * kMiB;
    for (const samples::Sample& sample : samples::all()) {
        size += sample.size + 8192;
    }
    test::VirtualSource source(size);
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 4096;
    for (std::size_t i = 0; i < samples::all().size(); ++i) {
        const samples::Sample& sample = samples::all()[i];
        const SampleVerdict& expected = kSampleVerdicts[i];
        ASSERT_EQ(sample.name, expected.name);
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes bytes = sample.data();
        const IFileFormat& format = formatWithId(sample.format);
        EXPECT_TRUE(testing::headerOf(format, bytes).plausible);
        // No other document or archive format claims it.
        for (const std::string_view other : {"pdf", "ole2", "zip"}) {
            if (other != sample.format) {
                EXPECT_FALSE(testing::headerOf(formatWithId(other), bytes).plausible) << other;
            }
        }
        const carving::EndDetection end = testing::endOf(format, testing::concat({bytes, testing::quietNoise(4096, i)}));
        EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
        EXPECT_EQ(end.length, bytes.size()) << testing::describe(end);
        EXPECT_EQ(end.extension, expected.extension);
        const carving::ValidationResult verdict = testing::verdictOf(format, bytes);
        EXPECT_EQ(verdict.status, expected.status) << testing::describe(verdict);
        offsets.push_back(offset);
        source.plant(offset, bytes);
        offset += (bytes.size() / 4096 + 2) * 4096;
    }
    RECOVERY_ASSERT_OK(source.open());

    // Carved with the image formats too: one candidate per document, and the
    // JPEG images of the verified ones (the PDFs, the XPS) are part of them.
    carving::FormatRegistry registry;
    RECOVERY_ASSERT_OK(registerImageFormats(registry));
    RECOVERY_ASSERT_OK(registerDocumentFormats(registry, validation::deflateDecoder()));
    RECOVERY_ASSERT_OK(registerArchiveFormats(registry, validation::deflateDecoder()));
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry);
    RECOVERY_ASSERT_OK(scanner);
    carving::FileCarver carver(source, carving::CarveOptions{});
    std::vector<carving::FileCandidate> candidates;
    Result<carving::CarveReport> report = carver.run(*scanner, [&](carving::FileCandidate&& candidate) {
        candidates.push_back(std::move(candidate));
        return success();
    });
    RECOVERY_ASSERT_OK(report);
    for (std::size_t i = 0; i < samples::all().size(); ++i) {
        const samples::Sample& sample = samples::all()[i];
        SCOPED_TRACE(sample.name);
        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const carving::FileCandidate& c) {
            return c.sourceOffset == offsets[i];
        });
        ASSERT_NE(found, candidates.end());
        EXPECT_EQ(found->formatId, sample.format);
        const std::string_view extension = kSampleVerdicts[i].extension;
        EXPECT_EQ(found->extension,
                  extension.empty() ? formatWithId(sample.format).descriptor().extension : std::string(extension));
        EXPECT_EQ(found->validation.status, kSampleVerdicts[i].status) << testing::describe(*found);
        EXPECT_TRUE(testing::carvedBytes(source, *found) == sample.data());
    }
    EXPECT_EQ(candidates.size(), samples::all().size());
    EXPECT_GE(report->skippedInsideCandidates, 1u);
}

bool isPadding(std::span<const std::byte> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) {
        const auto c = static_cast<std::uint8_t>(b);
        return c == 0 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
    });
}

TEST(DocumentReferenceTest, ExternalFilesAreRecognisedValidatedCarvedAndNamed) {
    const std::vector<std::filesystem::path> folders = foldersFromEnvironment(L"RECOVERY_DOCUMENT_REFERENCE_DIR");
    if (folders.empty()) {
        GTEST_SKIP() << "set RECOVERY_DOCUMENT_REFERENCE_DIR to run against documents made elsewhere";
    }
    std::map<std::string, std::size_t> verdicts;
    std::map<std::string, std::size_t> unchecked;
    std::map<std::string, std::size_t> names;
    std::size_t files = 0;
    std::size_t shorter = 0;
    for (const std::filesystem::path& folder : folders) {
        std::error_code ec;
        for (auto item = std::filesystem::recursive_directory_iterator(
                 folder, std::filesystem::directory_options::skip_permission_denied, ec);
             item != std::filesystem::recursive_directory_iterator(); item.increment(ec)) {
            if (ec) {
                break;
            }
            const Expected* expected = expectedFor(item->path());
            if (expected == nullptr || !item->is_regular_file(ec) || item->file_size(ec) > 1 * kGiB) {
                continue;
            }
            const Bytes bytes = test::readFile(item->path());
            SCOPED_TRACE(item->path().string());
            const IFileFormat& format = formatWithId(expected->formatId);
            if (!testing::headerOf(format, bytes).plausible) {
                // Not a file of the format at all (a text file named .msg, an RTF named .doc).
                ++verdicts["(other content) ." + std::string(expected->extension)];
                continue;
            }
            ++files;
            // Carved as on a disk: other data follows.
            const carving::EndDetection end =
                testing::endOf(format, testing::concat({bytes, testing::quietNoise(4096, files)}));
            EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
            EXPECT_LE(end.length, bytes.size()) << testing::describe(end);
            if (end.status != EndStatus::Found || end.length > bytes.size()) {
                continue;
            }
            if (end.length < bytes.size()) {
                // What the carve leaves out: a PDF's padding, an OLE2 file's free sectors.
                ++shorter;
                if (expected->formatId == "pdf") {
                    EXPECT_TRUE(isPadding(std::span(bytes).subspan(static_cast<std::size_t>(end.length))));
                }
            }
            if (!expected->named.empty()) {
                EXPECT_EQ(end.extension, expected->named == "jar" ? jarNamed(bytes) : expected->named);
            }
            ++names["." + std::string(expected->extension) + " named ." +
                    (end.extension.empty() ? format.descriptor().extension : end.extension)];
            const carving::ValidationResult named = testing::verdictOf(format, bytes);
            const carving::ValidationResult carved =
                testing::verdictOf(format, std::span(bytes).first(static_cast<std::size_t>(end.length)));
            EXPECT_TRUE(named.status == ValidationStatus::Valid || named.status == ValidationStatus::NotValidated)
                << testing::describe(named);
            EXPECT_EQ(carved.status, named.status) << testing::describe(carved);
            ++verdicts[std::string(expected->formatId) + " " + std::string(carving::toString(named.status))];
            if (named.status == ValidationStatus::NotValidated) {
                const std::size_t reason = named.detail.find(" not checked (");
                ++unchecked[reason == std::string::npos ? named.detail : named.detail.substr(reason + 1)];
            }
        }
    }
    ASSERT_GT(files, 0u) << "no documents in RECOVERY_DOCUMENT_REFERENCE_DIR";
    std::cout << "checked " << files << " documents (" << shorter << " carved shorter than the file):\n";
    for (const auto& [verdict, n] : verdicts) {
        std::cout << "  " << verdict << ": " << n << "\n";
    }
    for (const auto& [reason, n] : unchecked) {
        std::cout << "  unchecked, " << n << "x: " << reason << "\n";
    }
    for (const auto& [naming, n] : names) {
        std::cout << "  " << naming << ": " << n << "\n";
    }
}

TEST(DocumentBuilderExport, WritesFiles) {
    const std::optional<std::filesystem::path> directory =
        testing::directoryFromEnvironment(L"RECOVERY_DOCUMENT_EXPORT_DIR", false);
    if (!directory) {
        GTEST_SKIP() << "set RECOVERY_DOCUMENT_EXPORT_DIR to export the builders' files for other readers";
    }
    std::vector<std::pair<std::string, Bytes>> files;
    files.emplace_back("builder_simple.pdf", test::simplePdf());
    files.emplace_back("builder_image.pdf", test::simplePdf("photo", test::makeJpeg(test::JpegOptions{})));
    files.emplace_back("builder_updated.pdf", test::updatedPdf());
    files.emplace_back("builder_compressed.pdf", test::compressedPdf());
    files.emplace_back("builder_compressed_nopredictor.pdf", test::compressedPdf(false));
    files.emplace_back("builder_hybrid.pdf", test::hybridPdf());
    {
        test::PdfWriter pdf("1.3", "\r\n");
        pdf.object("<</Type /Catalog /Pages 2 0 R>>");
        pdf.object("<</Type /Pages /Kids [] /Count 0>>");
        pdf.stream("/Filter /FlateDecode", test::storedZlib(test::textBytes("BT ET")), true);
        pdf.endWithTable(1);
        files.emplace_back("builder_crlf_length_object.pdf", pdf.bytes());
    }
    files.emplace_back("builder_word.doc", test::wordDocument());
    files.emplace_back("builder_excel.xls", test::excelWorkbook());
    files.emplace_back("builder_powerpoint.ppt", test::powerPointPresentation());
    files.emplace_back("builder_outlook.msg", test::outlookMessage());
    test::Ole2Options version4;
    version4.version = 4;
    files.emplace_back("builder_version4.doc",
                       test::compoundFile({{"", "WordDocument", test::makePattern(20000, 10)},
                                           {"Storage", "Inner", test::makePattern(3000, 11)}},
                                          version4));
    test::Ole2Options padded;
    padded.trailingFreeSectors = 3;
    files.emplace_back("builder_padded.xls",
                       test::compoundFile({{"", "Workbook", test::makePattern(9000, 31)}}, padded));
    files.emplace_back("builder_difat.doc",
                       test::compoundFile({{"", "WordDocument", test::makePattern(7'500'000, 21)}}));

    std::filesystem::create_directories(*directory);
    for (const auto& [name, bytes] : files) {
        test::writeFile(*directory / name, bytes);
    }
    // What is written must also pass the engine's own checks.
    for (const auto& [name, bytes] : files) {
        SCOPED_TRACE(name);
        const Expected* expected = expectedFor(name);
        ASSERT_NE(expected, nullptr);
        EXPECT_EQ(testing::verdictOf(formatWithId(expected->formatId), bytes).status, ValidationStatus::Valid);
    }
    std::cout << "wrote " << files.size() << " files to " << directory->string() << "\n";
}

}  // namespace
}  // namespace recovery::formats
