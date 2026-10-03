// File carving: candidates from every test format compared with the planted
// originals; every end status (Found, Truncated by the source end or the
// maximum size, Broken, Unknown) and every rejection reason; validation and
// its failures; skipping hits inside validated candidates; unreadable and
// known-bad sectors; fatal errors; cancellation; sink errors; ids; logging;
// a format defined only in this file; disk images and a simulated disk that
// enforces sector alignment.

#include "carving/file_carver.hpp"

#include "carving/format_registry.hpp"
#include "carving/signature_scanner.hpp"
#include "recovery/byte_order.hpp"
#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"
#include "support/carving_formats.hpp"
#include "support/fake_device.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace recovery::carving {
namespace {

using test::bytesOf;
using test::scriptedDescriptor;
using test::ScriptedFormat;
using test::VirtualSource;

// A candidate's bytes, read from the source through its extents.
std::vector<std::byte> carvedBytes(storage::IStorageSource& source, const FileCandidate& candidate) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(candidate.length));
    for (const CarvedExtent& extent : candidate.extents) {
        const std::span<std::byte> target(bytes.data() + extent.fileOffset, static_cast<std::size_t>(extent.length));
        const Status read = source.readExact(ByteOffset{extent.sourceOffset}, target);
        EXPECT_TRUE(read.ok()) << describe(read.error());
    }
    return bytes;
}

std::string describeCandidate(const FileCandidate& candidate) {
    std::string text = "#" + std::to_string(candidate.id.value()) + " " + candidate.formatId + " @" +
                       std::to_string(candidate.sourceOffset) + " +" + std::to_string(candidate.length) + " end " +
                       std::string(toString(candidate.end.status)) + " (" + candidate.end.detail + ") validation " +
                       std::string(toString(candidate.validation.status)) + " (" + candidate.validation.detail + ")";
    for (const CarveWarning warning : candidate.warnings) {
        text += " " + std::string(toString(warning));
    }
    return text;
}

class FileCarverTest : public ::testing::Test {
protected:
    void add(std::shared_ptr<const IFileFormat> format) { ASSERT_TRUE(registry_.add(std::move(format)).ok()); }

    void addStandardFormats() {
        add(std::make_shared<test::SizedFormat>());
        add(std::make_shared<test::MarkerFormat>());
        add(std::make_shared<test::BoxFormat>());
    }

    // Runs a carve of the whole source and collects the candidates; a failed run is a test failure.
    CarveReport run(storage::IStorageSource& source, std::vector<FileCandidate>& candidates,
                    CarveOptions options = {}) {
        Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
        EXPECT_TRUE(scanner.ok());
        if (!scanner.ok()) {
            return {};
        }
        FileCarver carver(source, std::move(options));
        Result<CarveReport> report = carver.run(*scanner, [&](FileCandidate&& candidate) {
            const Status valid = validateFileCandidate(candidate);
            EXPECT_TRUE(valid.ok()) << describe(valid.error());
            candidates.push_back(std::move(candidate));
            return success();
        });
        EXPECT_TRUE(report.ok()) << describe(report.error());
        return report.ok() ? std::move(report).value() : CarveReport{};
    }

    SignatureHit hitFor(std::string_view id, std::uint64_t offset, std::size_t signature = 0) const {
        SignatureHit hit;
        hit.format = registry_.find(id);
        hit.formatIndex = registry_.indexOf(id).value_or(0);
        hit.signatureIndex = signature;
        hit.fileOffset = offset;
        return hit;
    }

    FormatRegistry registry_;
};

TEST_F(FileCarverTest, CarvesEveryTestFormatAndMatchesTheOriginals) {
    addStandardFormats();
    VirtualSource source(2 * kMiB);
    const std::vector<std::byte> sized = test::makeSizedFile(50'000);
    const std::vector<std::byte> marker = test::makeMarkerFile(300'000);
    const std::vector<std::byte> box = test::makeBoxFile(100, {1000, 70'000, 5});
    source.plant(4096, sized);
    source.plant(100'001, marker);
    source.plant(1'500'000, box);
    RECOVERY_ASSERT_OK(source.open());

    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates);
    ASSERT_EQ(candidates.size(), 3u);
    const std::vector<std::vector<std::byte>> originals = {sized, marker, box};
    const std::vector<std::uint64_t> offsets = {4096, 100'001, 1'500'000};
    const std::vector<std::string> ids = {"sized", "marker", "box"};
    const std::vector<std::string> extensions = {"szd", "mrk", "box"};
    const std::vector<EndDetectionMethod> methods = {EndDetectionMethod::SizeField, EndDetectionMethod::EndMarker,
                                                     EndDetectionMethod::StructureWalk};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const FileCandidate& candidate = candidates[i];
        SCOPED_TRACE(describeCandidate(candidate));
        EXPECT_EQ(candidate.id.value(), i + 1);
        EXPECT_EQ(candidate.formatId, ids[i]);
        EXPECT_EQ(candidate.extension, extensions[i]);
        EXPECT_EQ(candidate.sourceOffset, offsets[i]);
        EXPECT_EQ(candidate.length, originals[i].size());
        EXPECT_EQ(candidate.extraction, ExtractionStrategy::Contiguous);
        EXPECT_EQ(candidate.extents, (std::vector<CarvedExtent>{{0, offsets[i], originals[i].size()}}));
        EXPECT_EQ(candidate.end.status, EndStatus::Found);
        EXPECT_EQ(candidate.end.method, methods[i]);
        EXPECT_EQ(candidate.validation.status, ValidationStatus::Valid);
        EXPECT_EQ(candidate.validation.validBytes, originals[i].size());
        EXPECT_TRUE(candidate.warnings.empty());
        EXPECT_TRUE(candidate.unreadableRegions.empty());
        EXPECT_EQ(candidate.signature.signatureIndex, 0u);
        EXPECT_FALSE(candidate.signature.signatureName.empty());
        EXPECT_TRUE(carvedBytes(source, candidate) == originals[i]);
    }
    // The box format's signature sits 4 bytes into the file.
    EXPECT_EQ(candidates[2].signature.matchOffset, 1'500'004u);
    EXPECT_EQ(candidates[0].end.available, 1 * kMiB);  // the format's maximum size
    EXPECT_EQ(report.scan.outcome, ScanOutcome::Completed);
    EXPECT_EQ(report.scan.hits, 3u);
    EXPECT_EQ(report.candidates, 3u);
    EXPECT_EQ(report.count(ValidationStatus::Valid), 3u);
    EXPECT_EQ(report.rejectedTotal(), 0u);
    EXPECT_GE(report.carveBytesRead, sized.size() + marker.size() + box.size());
}

TEST_F(FileCarverTest, CandidatesAreDeliveredWhileTheScanIsStillReading) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(8 * kMiB);
    source.plant(100, test::makeSizedFile(1000));
    RECOVERY_ASSERT_OK(source.open());
    Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
    RECOVERY_ASSERT_OK(scanner);
    CarveOptions options;
    options.scan.blockSize = 64 * 1024;
    FileCarver carver(source, options);
    std::uint64_t readWhenDelivered = 0;
    RECOVERY_ASSERT_OK(carver.run(*scanner, [&](FileCandidate&&) {
        readWhenDelivered = source.stats().highestEnd;
        return success();
    }));
    EXPECT_GT(readWhenDelivered, 0u);
    EXPECT_LE(readWhenDelivered, 256 * 1024u);
    EXPECT_EQ(source.stats().highestEnd, 8 * kMiB);
}

TEST_F(FileCarverTest, ImplausibleHeadersAreRejected) {
    addStandardFormats();
    VirtualSource source(64 * 1024);
    std::vector<std::byte> badLength = test::makeSizedFile(100);
    storeLe32(badLength, 4, 5);  // shorter than the header
    std::vector<std::byte> badVersion = test::makeMarkerFile(100);
    badVersion[4] = std::byte{2};
    source.plant(1000, badLength);
    source.plant(9000, badVersion);
    source.plant(20'000, test::makeSizedFile(10));
    RECOVERY_ASSERT_OK(source.open());

    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].sourceOffset, 20'000u);
    EXPECT_EQ(report.count(RejectionReason::HeaderRejected), 2u);
    EXPECT_EQ(report.scan.hits, report.candidates + report.rejectedTotal() + report.skippedInsideCandidates);

    FileCarver carver(source);
    Result<CarveOutcome> outcome = carver.carve(hitFor("marker", 9000));
    RECOVERY_ASSERT_OK(outcome);
    const CarveRejection* rejection = std::get_if<CarveRejection>(&*outcome);
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->reason, RejectionReason::HeaderRejected);
    EXPECT_EQ(rejection->detail, "unsupported version");
}

TEST_F(FileCarverTest, HandMadeHitsWhoseSignatureDoesNotMatchAreRejected) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    RECOVERY_ASSERT_OK(source.open());
    FileCarver carver(source);
    Result<CarveOutcome> outcome = carver.carve(hitFor("sized", 1000));
    RECOVERY_ASSERT_OK(outcome);
    const CarveRejection* rejection = std::get_if<CarveRejection>(&*outcome);
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->reason, RejectionReason::HeaderRejected);
    EXPECT_NE(rejection->detail.find("does not match"), std::string::npos);
}

TEST_F(FileCarverTest, HitsWithTooLittleDataBeforeTheEndAreRejected) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(64 * 1024 - 8, bytesOf("SZD1"));  // 8 bytes left; the format needs 12
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates);
    EXPECT_TRUE(candidates.empty());
    EXPECT_EQ(report.count(RejectionReason::TooLittleData), 1u);

    FileCarver carver(source);
    for (const std::uint64_t offset : {64ULL * 1024, 64ULL * 1024 + 1, std::numeric_limits<std::uint64_t>::max()}) {
        Result<CarveOutcome> outcome = carver.carve(hitFor("sized", offset));
        RECOVERY_ASSERT_OK(outcome);
        const CarveRejection* rejection = std::get_if<CarveRejection>(&*outcome);
        ASSERT_NE(rejection, nullptr);
        EXPECT_EQ(rejection->reason, RejectionReason::TooLittleData);
    }
}

TEST_F(FileCarverTest, EndsBelowTheMinimumOrBeyondTheDataAreRejected) {
    struct Case {
        EndStatus status;
        std::uint64_t length;  // relative to content.size() when `relative`
        bool relative;
        RejectionReason reason;
    };
    const std::vector<Case> cases = {
        {EndStatus::Found, 3, false, RejectionReason::TooSmall},
        {EndStatus::Broken, 0, false, RejectionReason::TooSmall},
        {EndStatus::Truncated, 15, false, RejectionReason::TooSmall},
        {EndStatus::Found, 1, true, RejectionReason::InvalidEnd},
        {EndStatus::Unknown, std::numeric_limits<std::uint64_t>::max(), false, RejectionReason::InvalidEnd},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(std::string(toString(test.status)) + " " + std::to_string(test.length));
        FormatRegistry registry;
        auto format = std::make_shared<ScriptedFormat>(scriptedDescriptor("script", "SCRP"));
        format->onEnd([&](IContentReader& content) -> Result<EndDetection> {
            return EndDetection{test.status, test.relative ? content.size() + test.length : test.length, "scripted"};
        });
        RECOVERY_ASSERT_OK(registry.add(format));
        VirtualSource source(64 * 1024);
        source.plant(100, bytesOf("SCRP"));
        RECOVERY_ASSERT_OK(source.open());
        FileCarver carver(source);
        SignatureHit hit;
        hit.format = format.get();
        hit.fileOffset = 100;
        Result<CarveOutcome> outcome = carver.carve(hit);
        RECOVERY_ASSERT_OK(outcome);
        const CarveRejection* rejection = std::get_if<CarveRejection>(&*outcome);
        ASSERT_NE(rejection, nullptr);
        EXPECT_EQ(rejection->reason, test.reason);
    }
}

TEST_F(FileCarverTest, FormatFailuresRejectTheHitButTheRunContinues) {
    const std::vector<std::function<void(ScriptedFormat&)>> failures = {
        [](ScriptedFormat& f) {
            f.onEnd([](IContentReader&) -> Result<EndDetection> {
                return makeError(ErrorCode::InvalidFormat, "the format gave up");
            });
        },
        [](ScriptedFormat& f) {
            // Reads beyond its content: the reader refuses.
            f.onEnd([](IContentReader& content) -> Result<EndDetection> {
                Result<std::span<const std::byte>> beyond = content.read(content.size(), 1);
                if (!beyond.ok()) {
                    return beyond.error();
                }
                return EndDetection{EndStatus::Found, content.size(), ""};
            });
        },
        [](ScriptedFormat& f) {
            f.onEnd([](IContentReader&) -> Result<EndDetection> {
                return EndDetection{static_cast<EndStatus>(42), 100, ""};
            });
        },
        [](ScriptedFormat& f) {
            f.onValidate([](IContentReader&) -> Result<ValidationResult> {
                return makeError(ErrorCode::InternalError, "validator bug");
            });
        },
        [](ScriptedFormat& f) {
            f.onValidate([](IContentReader&) -> Result<ValidationResult> { return ValidationResult{}; });
        },
        [](ScriptedFormat& f) {
            f.onValidate([](IContentReader&) -> Result<ValidationResult> {
                return ValidationResult{static_cast<ValidationStatus>(9), 0, ""};
            });
        },
    };
    for (std::size_t i = 0; i < failures.size(); ++i) {
        SCOPED_TRACE("failure " + std::to_string(i));
        registry_ = FormatRegistry{};
        auto format = std::make_shared<ScriptedFormat>(scriptedDescriptor("script", "SCRP", 1024));
        failures[i](*format);
        add(format);
        add(std::make_shared<test::SizedFormat>());
        VirtualSource source(64 * 1024);
        source.plant(100, bytesOf("SCRP"));
        source.plant(5000, test::makeSizedFile(10));
        RECOVERY_ASSERT_OK(source.open());
        std::vector<FileCandidate> candidates;
        const CarveReport report = run(source, candidates);
        ASSERT_EQ(candidates.size(), 1u);
        EXPECT_EQ(candidates[0].formatId, "sized");
        EXPECT_EQ(report.count(RejectionReason::FormatFailed), 1u);
    }
}

TEST_F(FileCarverTest, FilesCutByTheEndOfTheSourceAreTruncated) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    const std::vector<std::byte> file = test::makeSizedFile(30'000);
    source.plant(50'000, file);  // declares 30'012 bytes, 15'536 are left
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const FileCandidate& candidate = candidates[0];
    SCOPED_TRACE(describeCandidate(candidate));
    EXPECT_EQ(candidate.length, 15'536u);
    EXPECT_EQ(candidate.end.status, EndStatus::Truncated);
    EXPECT_EQ(candidate.end.available, 15'536u);
    EXPECT_EQ(candidate.warnings, (std::vector<CarveWarning>{CarveWarning::TruncatedBySourceEnd}));
    EXPECT_EQ(candidate.validation.status, ValidationStatus::Truncated);
    EXPECT_TRUE(carvedBytes(source, candidate) ==
                std::vector<std::byte>(file.begin(), file.begin() + 15'536));
}

TEST_F(FileCarverTest, FilesLongerThanTheMaximumSizeAreTruncated) {
    add(std::make_shared<test::SizedFormat>(4096));
    VirtualSource source(64 * 1024);
    source.plant(1000, test::makeSizedFile(10'000));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].length, 4096u);
    EXPECT_EQ(candidates[0].end.available, 4096u);
    EXPECT_EQ(candidates[0].warnings, (std::vector<CarveWarning>{CarveWarning::TruncatedByMaximumSize}));
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Truncated);
}

TEST_F(FileCarverTest, AMissingEndMarkerMeansTruncated) {
    add(std::make_shared<test::MarkerFormat>(8192));
    VirtualSource source(64 * 1024);
    std::vector<std::byte> file = test::makeMarkerFile(20'000);
    file.resize(file.size() - 5);  // no end marker
    source.plant(100, file);
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].length, 8192u);
    EXPECT_EQ(candidates[0].end.status, EndStatus::Truncated);
    EXPECT_EQ(candidates[0].warnings, (std::vector<CarveWarning>{CarveWarning::TruncatedByMaximumSize}));
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Truncated);
}

TEST_F(FileCarverTest, ABrokenStructureKeepsTheConsistentPart) {
    add(std::make_shared<test::BoxFormat>());
    VirtualSource source(64 * 1024);
    std::vector<std::byte> file = test::makeBoxFile(10, {100, 200, 300});
    // Boxes: tbox (18 bytes), data (108), data (208), ... Damage the size of the second data box.
    const std::size_t broken = 18 + 108;
    storeLe32(file, broken, 3);
    source.plant(2000, file);
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const FileCandidate& candidate = candidates[0];
    SCOPED_TRACE(describeCandidate(candidate));
    EXPECT_EQ(candidate.end.status, EndStatus::Broken);
    EXPECT_EQ(candidate.length, broken);
    EXPECT_EQ(candidate.warnings, (std::vector<CarveWarning>{CarveWarning::StructureBroken}));
    // What is left is consistent, but the structure does not end there.
    EXPECT_EQ(candidate.validation.status, ValidationStatus::Truncated);
    EXPECT_EQ(candidate.validation.validBytes, broken);
}

TEST_F(FileCarverTest, FormatsWithoutEndInformationGiveAnEstimate) {
    add(std::make_shared<test::SyncFormat>());
    VirtualSource source(64 * 1024);
    source.plant(1000, {std::byte{0xFF}, std::byte{0xFB}});
    source.plant(62'000, {std::byte{0xFF}, std::byte{0xE0}});  // 3536 bytes left: less than the estimate
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].length, test::SyncFormat::kEstimate);
    EXPECT_EQ(candidates[0].end.status, EndStatus::Unknown);
    EXPECT_EQ(candidates[0].end.method, EndDetectionMethod::None);
    EXPECT_EQ(candidates[0].warnings, (std::vector<CarveWarning>{CarveWarning::EndUnknown}));
    EXPECT_EQ(candidates[1].length, 3536u);
}

TEST_F(FileCarverTest, StructuralValidationCatchesDamagedContent) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    std::vector<std::byte> file = test::makeSizedFile(1000);
    file[500] ^= std::byte{0x01};
    source.plant(100, file);
    RECOVERY_ASSERT_OK(source.open());
    diagnostics::Logger logger(diagnostics::LogLevel::Info);
    auto sink = std::make_shared<diagnostics::MemorySink>();
    logger.addSink(sink);
    CarveOptions options;
    options.scan.logger = &logger;
    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates, options);
    ASSERT_EQ(candidates.size(), 1u);
    // The header and the recorded size are fine: the end is found, but the content is not valid.
    EXPECT_EQ(candidates[0].end.status, EndStatus::Found);
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Invalid);
    EXPECT_EQ(candidates[0].validation.validBytes, test::SizedFormat::kHeaderSize);
    EXPECT_EQ(candidates[0].validation.detail, "payload CRC mismatch");
    EXPECT_EQ(report.count(ValidationStatus::Invalid), 1u);
    const std::vector<diagnostics::LogRecord> records = sink->records();
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const diagnostics::LogRecord& record) {
        return record.message == "carved candidate failed validation";
    }));
}

TEST_F(FileCarverTest, ValidationCanBeTurnedOff) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(100, test::makeSizedFile(1000));
    RECOVERY_ASSERT_OK(source.open());
    CarveOptions options;
    options.validate = false;
    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates, options);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::NotValidated);
    EXPECT_EQ(report.count(ValidationStatus::NotValidated), 1u);
}

TEST_F(FileCarverTest, HitsInsideAValidatedFileAreSkipped) {
    addStandardFormats();
    VirtualSource source(64 * 1024);
    // An embedded file (a thumbnail, say) inside a valid outer file.
    std::vector<std::byte> inner = test::makeMarkerFile(500);
    inner.resize(inner.size() + 100, std::byte{7});
    const std::vector<std::byte> outer = test::makeSizedFileAround(inner);
    source.plant(1000, outer);
    source.plant(10'000, test::makeBoxFile(0, {}));  // after the outer file: carved
    RECOVERY_ASSERT_OK(source.open());

    std::vector<FileCandidate> candidates;
    CarveReport report = run(source, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].formatId, "sized");
    EXPECT_EQ(candidates[1].formatId, "box");
    EXPECT_EQ(report.skippedInsideCandidates, 1u);
    EXPECT_EQ(report.scan.hits, 3u);

    candidates.clear();
    CarveOptions options;
    options.skipHitsInsideValidCandidates = false;
    report = run(source, candidates, options);
    ASSERT_EQ(candidates.size(), 3u);
    EXPECT_EQ(candidates[1].formatId, "marker");
    EXPECT_EQ(candidates[1].sourceOffset, 1000 + test::SizedFormat::kHeaderSize);
    EXPECT_EQ(candidates[1].validation.status, ValidationStatus::Valid);
    EXPECT_EQ(report.skippedInsideCandidates, 0u);
}

TEST_F(FileCarverTest, HitsInsideUntrustedCandidatesAreCarved) {
    addStandardFormats();
    add(std::make_shared<test::SyncFormat>());
    const std::vector<std::byte> inner = test::makeMarkerFile(200);
    std::vector<std::byte> damaged = test::makeSizedFileAround(inner);
    damaged[8] ^= std::byte{0x01};  // CRC mismatch: Invalid

    VirtualSource source(64 * 1024);
    source.plant(1000, damaged);
    // A candidate with an unknown end (valid, but not trusted) around a sized file.
    source.plant(20'000, {std::byte{0xFF}, std::byte{0xE0}});
    source.plant(20'500, test::makeSizedFile(50));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    CarveReport report = run(source, candidates);
    std::vector<std::string> formats;
    for (const FileCandidate& candidate : candidates) {
        formats.push_back(candidate.formatId);
    }
    EXPECT_EQ(formats, (std::vector<std::string>{"sized", "marker", "sync", "sized"}));
    EXPECT_EQ(report.skippedInsideCandidates, 0u);

    // Without validation nothing is trusted, so nothing is skipped either.
    VirtualSource valid(64 * 1024);
    valid.plant(1000, test::makeSizedFileAround(inner));
    RECOVERY_ASSERT_OK(valid.open());
    candidates.clear();
    CarveOptions options;
    options.validate = false;
    report = run(valid, candidates, options);
    EXPECT_EQ(candidates.size(), 2u);
    EXPECT_EQ(report.skippedInsideCandidates, 0u);
}

TEST_F(FileCarverTest, HitsAtTheStartOfAValidatedFileAreCarved) {
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("first", "SAME")));
    add(std::make_shared<ScriptedFormat>(scriptedDescriptor("second", "SAME")));
    VirtualSource source(64 * 1024);
    source.plant(64, bytesOf("SAME"));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].formatId, "first");
    EXPECT_EQ(candidates[1].formatId, "second");
    EXPECT_EQ(report.skippedInsideCandidates, 0u);
}

TEST_F(FileCarverTest, OwnHitsInsideASelfSynchronizingCandidateAreSkippedWhateverItsVerdict) {
    // A stream of 100-byte frames that each start with "FRM!", so every frame
    // is a hit. Its candidates end where the frames stop and never validate:
    // under the ordinary rule every frame would be carved again.
    constexpr std::uint64_t kFrame = 100;
    const auto frames = [](IContentReader& content) -> Result<EndDetection> {
        std::uint64_t length = 0;
        while (content.size() - length >= 4) {
            Result<std::span<const std::byte>> magic = content.read(length, 4);
            if (!magic.ok()) {
                return magic.error();
            }
            if (!std::equal(magic->begin(), magic->end(), bytesOf("FRM!").begin())) {
                break;
            }
            length = std::min(length + kFrame, content.size());
        }
        return EndDetection{EndStatus::Broken, length, "the frames stop"};
    };
    const auto invalid = [](IContentReader& content) -> Result<ValidationResult> {
        return ValidationResult{ValidationStatus::Invalid, content.size(), "scripted"};
    };
    FormatDescriptor streamDescriptor = scriptedDescriptor("stream", "FRM!");
    streamDescriptor.selfSynchronizing = true;
    auto stream = std::make_shared<ScriptedFormat>(streamDescriptor);
    stream->onEnd(frames).onValidate(invalid);
    // The same format without the flag, for comparison.
    auto plain = std::make_shared<ScriptedFormat>(scriptedDescriptor("plain", "PLN!"));
    plain->onEnd([](IContentReader& content) -> Result<EndDetection> {
        return EndDetection{EndStatus::Found, std::min<std::uint64_t>(content.size(), 3 * kFrame), "scripted"};
    });
    plain->onValidate(invalid);
    add(stream);
    add(plain);

    VirtualSource source(64 * 1024);
    const auto plantFrames = [&](std::uint64_t offset, int count, std::string_view magic) {
        for (int i = 0; i < count; ++i) {
            source.plant(offset + static_cast<std::uint64_t>(i) * kFrame, bytesOf(magic));
        }
    };
    plantFrames(1000, 10, "FRM!");  // a stream of 10 frames: one candidate
    source.plant(1000 + 5 * kFrame + 50, bytesOf("PLN!"));  // another format inside it: carved
    plantFrames(5000, 3, "FRM!");  // a second stream after the first: carved
    plantFrames(9000, 3, "PLN!");  // hits inside an Invalid candidate of a plain format: carved
    RECOVERY_ASSERT_OK(source.open());

    std::vector<FileCandidate> candidates;
    CarveReport report = run(source, candidates);
    std::vector<std::string> found;
    for (const FileCandidate& candidate : candidates) {
        found.push_back(candidate.formatId + "@" + std::to_string(candidate.sourceOffset));
    }
    EXPECT_EQ(found, (std::vector<std::string>{"stream@1000", "plain@1550", "stream@5000", "plain@9000",
                                               "plain@9100", "plain@9200"}));
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates[0].length, 10 * kFrame);
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Invalid);
    EXPECT_EQ(report.skippedInsideCandidates, 9u + 2u);
    EXPECT_EQ(report.scan.hits, report.candidates + report.rejectedTotal() + report.skippedInsideCandidates);

    // Turning skipping off carves every frame.
    candidates.clear();
    CarveOptions options;
    options.skipHitsInsideValidCandidates = false;
    report = run(source, candidates, options);
    EXPECT_EQ(report.skippedInsideCandidates, 0u);
    EXPECT_EQ(report.candidates, report.scan.hits);
}

TEST_F(FileCarverTest, UnreadableSectorsInsideACandidateAreZerosAndRecorded) {
    add(std::make_shared<test::MarkerFormat>());
    VirtualSource source(256 * 1024);
    source.plant(10'000, test::makeMarkerFile(40'000));
    source.addBadSector(60, 23);  // [30'720, 31'232): inside the body
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    const CarveReport report = run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const FileCandidate& candidate = candidates[0];
    SCOPED_TRACE(describeCandidate(candidate));
    // The end marker lies beyond the bad sector, so the end is still found...
    EXPECT_EQ(candidate.end.status, EndStatus::Found);
    EXPECT_EQ(candidate.length, 40'010u);
    // ...but the zeros read for it break the structure.
    EXPECT_EQ(candidate.validation.status, ValidationStatus::Invalid);
    EXPECT_EQ(candidate.validation.validBytes, 30'720u - 10'000u);
    EXPECT_EQ(candidate.warnings, (std::vector<CarveWarning>{CarveWarning::UnreadableData}));
    EXPECT_EQ(candidate.unreadableRegions, (std::vector<storage::BadRegion>{{30'720, 512, 23}}));
    EXPECT_EQ(report.scan.unreadableRegions, (std::vector<storage::BadRegion>{{30'720, 512, 23}}));
}

TEST_F(FileCarverTest, UnreadableSectorsNextToACandidateAreNotItsConcern) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(10'000, test::makeSizedFile(100));  // ends at 10'112
    source.addBadSector(20);                         // [10'240, 10'752): read by the carve, outside the file
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_TRUE(candidates[0].unreadableRegions.empty());
    EXPECT_TRUE(candidates[0].warnings.empty());
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Valid);
}

TEST_F(FileCarverTest, KnownBadRegionsApplyToCarving) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(1000, test::makeSizedFile(5000));
    RECOVERY_ASSERT_OK(source.open());
    storage::BadRegionMap known;
    RECOVERY_ASSERT_OK(known.add({3000, 100, 1117}));
    CarveOptions options;
    options.scan.reads.knownBadRegions = &known;
    std::vector<FileCandidate> candidates;
    run(source, candidates, options);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].unreadableRegions, (std::vector<storage::BadRegion>{{3000, 100, 1117}}));
    EXPECT_EQ(candidates[0].warnings, (std::vector<CarveWarning>{CarveWarning::UnreadableData}));
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Invalid);
}

TEST_F(FileCarverTest, FatalReadErrorsStopTheRun) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(1 * kMiB);
    source.plant(1000, test::makeSizedFile(100'000));
    source.addFatalSector(80'000 / 512);  // inside the file, beyond the first scan block
    RECOVERY_ASSERT_OK(source.open());
    Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
    RECOVERY_ASSERT_OK(scanner);
    CarveOptions options;
    options.scan.blockSize = 4096;
    FileCarver carver(source, options);
    int delivered = 0;
    const Result<CarveReport> report = carver.run(*scanner, [&](FileCandidate&&) {
        ++delivered;
        return success();
    });
    ASSERT_FALSE(report.ok());
    EXPECT_NE(report.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(delivered, 0);
    const Result<CarveOutcome> outcome = carver.carve(hitFor("sized", 1000));
    EXPECT_FALSE(outcome.ok());
}

TEST_F(FileCarverTest, CancellationDuringALongCarveEndsTheRunPromptly) {
    add(std::make_shared<test::MarkerFormat>(64 * kMiB));
    VirtualSource source(128 * kMiB);
    std::vector<std::byte> header = bytesOf("<MK>");
    header.push_back(std::byte{1});
    source.plant(1000, header);  // no end marker: end detection searches 64 MiB
    RECOVERY_ASSERT_OK(source.open());
    CancellationSource cancel;
    source.setReadHook([&](std::uint64_t offset, std::size_t) {
        if (offset >= 8 * kMiB) {
            cancel.requestCancellation();
        }
    });
    Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
    RECOVERY_ASSERT_OK(scanner);
    CarveOptions options;
    options.scan.blockSize = 64 * 1024;
    options.scan.reads.cancellation = cancel.token();
    FileCarver carver(source, options);
    int delivered = 0;
    const Result<CarveReport> report = carver.run(*scanner, [&](FileCandidate&&) {
        ++delivered;
        return success();
    });
    RECOVERY_ASSERT_OK(report);
    EXPECT_EQ(report->scan.outcome, ScanOutcome::Cancelled);
    // The hit being carved was not finished: a later run starts again at it.
    EXPECT_EQ(report->scan.nextOffset, 1000u);
    EXPECT_EQ(delivered, 0);
    EXPECT_LE(source.stats().highestEnd, 8 * kMiB + 1 * kMiB);
}

TEST_F(FileCarverTest, TheCandidateSinkCanCancelOrFail) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(100, test::makeSizedFile(10));
    source.plant(5000, test::makeSizedFile(10));
    RECOVERY_ASSERT_OK(source.open());
    Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
    RECOVERY_ASSERT_OK(scanner);

    FileCarver carver(source);
    const Result<CarveReport> cancelled = carver.run(*scanner, [](FileCandidate&& candidate) {
        return candidate.sourceOffset == 5000 ? Status(makeError(ErrorCode::Cancelled, "enough")) : success();
    });
    RECOVERY_ASSERT_OK(cancelled);
    EXPECT_EQ(cancelled->scan.outcome, ScanOutcome::Cancelled);
    EXPECT_EQ(cancelled->scan.nextOffset, 5000u);
    EXPECT_EQ(cancelled->candidates, 2u);

    const Result<CarveReport> failed = carver.run(
        *scanner, [](FileCandidate&&) { return Status(makeError(ErrorCode::DestinationError, "disk full")); });
    RECOVERY_EXPECT_ERROR(failed, ErrorCode::DestinationError);
}

TEST_F(FileCarverTest, IdsStartAtFirstIdAndRejectionsUseNone) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(100, test::makeSizedFile(10));
    source.plant(5000, test::makeSizedFile(10));
    RECOVERY_ASSERT_OK(source.open());
    CarveOptions options;
    options.firstId = 100;
    FileCarver carver(source, options);
    const auto idOf = [&](std::uint64_t offset) -> std::uint64_t {
        Result<CarveOutcome> outcome = carver.carve(hitFor("sized", offset));
        EXPECT_TRUE(outcome.ok());
        const FileCandidate* candidate = outcome.ok() ? std::get_if<FileCandidate>(&*outcome) : nullptr;
        return candidate == nullptr ? 0 : candidate->id.value();
    };
    EXPECT_EQ(idOf(100), 100u);
    EXPECT_EQ(idOf(3000), 0u);  // rejected
    EXPECT_EQ(idOf(5000), 101u);
}

TEST_F(FileCarverTest, RejectsInvalidCallsAndOptions) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(100, test::makeSizedFile(10));
    FileCarver closed(source);
    RECOVERY_EXPECT_ERROR(closed.carve(hitFor("sized", 100)), ErrorCode::InvalidInput);
    RECOVERY_ASSERT_OK(source.open());

    FileCarver carver(source);
    RECOVERY_EXPECT_ERROR(carver.carve(SignatureHit{}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(carver.carve(hitFor("sized", 100, 1)), ErrorCode::InvalidInput);
    // A format no registry has checked, with a descriptor that breaks a rule.
    FormatDescriptor broken = scriptedDescriptor("broken", "BRKN");
    broken.headerSize = 0;
    const ScriptedFormat unregistered(broken);
    SignatureHit brokenHit;
    brokenHit.format = &unregistered;
    brokenHit.fileOffset = 100;
    RECOVERY_EXPECT_ERROR(carver.carve(brokenHit), ErrorCode::InvalidInput);
    Result<SignatureScanner> scanner = SignatureScanner::create(registry_);
    RECOVERY_ASSERT_OK(scanner);
    RECOVERY_EXPECT_ERROR(carver.run(*scanner, FileCandidateSink{}), ErrorCode::InvalidInput);

    CarveOptions small;
    small.readCacheSize = SourceContentReader::kMinCacheSize - 1;
    RECOVERY_EXPECT_ERROR(FileCarver(source, small).carve(hitFor("sized", 100)), ErrorCode::InvalidInput);
    CarveOptions retries;
    retries.scan.reads.sectorRetryCount = SourceReadOptions::kMaxSectorRetries + 1;
    RECOVERY_EXPECT_ERROR(FileCarver(source, retries).carve(hitFor("sized", 100)), ErrorCode::InvalidInput);
}

TEST_F(FileCarverTest, AHugeMaximumSizeIsBoundedByTheSource) {
    auto format = std::make_shared<ScriptedFormat>(
        scriptedDescriptor("huge", "HUGE", FormatDescriptor::kMaxMaximumSize));
    std::uint64_t seen = 0;
    format->onEnd([&](IContentReader& content) -> Result<EndDetection> {
        seen = content.size();
        // The last byte is readable, nothing beyond it.
        Result<std::span<const std::byte>> last = content.read(content.size() - 1, 1);
        if (!last.ok()) {
            return last.error();
        }
        return EndDetection{EndStatus::Unknown, content.size(), "all of it"};
    });
    add(format);
    VirtualSource source(64 * 1024);
    source.plant(4096, bytesOf("HUGE"));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(seen, 60u * 1024u);
    EXPECT_EQ(candidates[0].length, 60u * 1024u);
    EXPECT_EQ(candidates[0].end.available, 60u * 1024u);
}

TEST_F(FileCarverTest, LogsTheRunCandidatesAndRejections) {
    add(std::make_shared<test::SizedFormat>());
    VirtualSource source(64 * 1024);
    source.plant(100, test::makeSizedFile(10));
    source.plant(5000, bytesOf("SZD1"));  // declared length 0: rejected
    RECOVERY_ASSERT_OK(source.open());
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    auto sink = std::make_shared<diagnostics::MemorySink>();
    logger.addSink(sink);
    CarveOptions options;
    options.scan.logger = &logger;
    std::vector<FileCandidate> candidates;
    run(source, candidates, options);
    std::vector<std::string> messages;
    for (const diagnostics::LogRecord& record : sink->records()) {
        messages.push_back(record.message);
    }
    EXPECT_EQ(messages, (std::vector<std::string>{"carving started", "signature scan started",
                                                  "file candidate carved", "signature hit rejected",
                                                  "signature scan ended", "carving ended"}));
}

// A format that exists only in this test: lines of text between a "#!RCV"
// line and a "#END" line. Adding it takes nothing but this class and a
// registry.add() call: the scanner and the carver are unchanged.
class LineFormat final : public IFileFormat, public FormatValidator {
public:
    LineFormat() {
        descriptor_.id = "lines";
        descriptor_.name = "Line test format";
        descriptor_.extension = "txt";
        descriptor_.signatures = {textSignature("header line", "#!RCV\n")};
        descriptor_.minimumSize = 11;  // header line + "#END\n"
        descriptor_.maximumSize = 64 * 1024;
        descriptor_.headerSize = 6;
        descriptor_.endDetection = EndDetectionMethod::EndMarker;
    }

    const FormatDescriptor& descriptor() const noexcept override { return descriptor_; }

    HeaderCheck checkHeader(std::span<const std::byte> /*header*/) const override { return HeaderCheck::accept(); }

    Result<EndDetection> findEnd(IContentReader& content) const override {
        const std::vector<std::byte> end = bytesOf("\n#END\n");
        Result<std::optional<std::uint64_t>> found = findPattern(content, end, 5, content.size());
        if (!found.ok()) {
            return found.error();
        }
        if (!found->has_value()) {
            return EndDetection{EndStatus::Truncated, content.size(), "no end line"};
        }
        return EndDetection{EndStatus::Found, **found + end.size(), "end line"};
    }

    const FormatValidator& validator() const noexcept override { return *this; }

    Result<ValidationResult> validate(IContentReader& content) const override {
        Result<std::span<const std::byte>> text = content.read(0, static_cast<std::size_t>(content.size()));
        if (!text.ok()) {
            return text.error();
        }
        for (std::size_t i = 0; i < text->size(); ++i) {
            const auto c = static_cast<unsigned char>((*text)[i]);
            if (c != '\n' && (c < 0x20 || c > 0x7E)) {
                return ValidationResult{ValidationStatus::Invalid, i, "not printable text"};
            }
        }
        return ValidationResult{ValidationStatus::Valid, content.size(), "printable text"};
    }

private:
    FormatDescriptor descriptor_;
};

TEST_F(FileCarverTest, ANewFormatNeedsNoChangeToTheScannerOrTheCarver) {
    addStandardFormats();
    add(std::make_shared<LineFormat>());
    VirtualSource source(64 * 1024);
    const std::vector<std::byte> text = bytesOf("#!RCV\nfirst line\nsecond line\n#END\n");
    source.plant(3000, test::makeSizedFile(100));
    source.plant(9000, text);
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[1].formatId, "lines");
    EXPECT_EQ(candidates[1].extension, "txt");
    EXPECT_EQ(candidates[1].sourceOffset, 9000u);
    EXPECT_EQ(candidates[1].length, text.size());
    EXPECT_EQ(candidates[1].validation.status, ValidationStatus::Valid);
    EXPECT_TRUE(carvedBytes(source, candidates[1]) == text);
}

TEST_F(FileCarverTest, CarvesFromADiskImageFile) {
    addStandardFormats();
    const test::TempDir dir;
    std::vector<std::byte> image = test::makePattern(2 * kMiB, 77);
    // The pattern must not hold a signature by chance: overwrite the first byte of every test signature.
    std::replace(image.begin(), image.end(), std::byte{'S'}, std::byte{'R'});
    std::replace(image.begin(), image.end(), std::byte{'<'}, std::byte{';'});
    std::replace(image.begin(), image.end(), std::byte{'t'}, std::byte{'s'});
    const std::vector<std::byte> sized = test::makeSizedFile(300'000);
    const std::vector<std::byte> box = test::makeBoxFile(1, {2, 3});
    std::copy(sized.begin(), sized.end(), image.begin() + 12'345);
    std::copy(box.begin(), box.end(), image.begin() + 1'000'000);
    test::writeFile(dir / "disk.img", image);

    storage::DiskImageSource source(dir / "disk.img");
    RECOVERY_ASSERT_OK(source.open());
    std::vector<FileCandidate> candidates;
    run(source, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].sourceOffset, 12'345u);
    EXPECT_TRUE(carvedBytes(source, candidates[0]) == sized);
    EXPECT_EQ(candidates[1].sourceOffset, 1'000'000u);
    EXPECT_TRUE(carvedBytes(source, candidates[1]) == box);
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Valid);
    EXPECT_EQ(candidates[1].validation.status, ValidationStatus::Valid);
}

TEST_F(FileCarverTest, CarvesFromADiskThatEnforcesSectorAlignment) {
    addStandardFormats();
    auto config = std::make_shared<test::FakeDeviceConfig>();
    config->data.resize(1 * kMiB);
    const std::vector<std::byte> marker = test::makeMarkerFile(70'000);
    const std::vector<std::byte> sized = test::makeSizedFile(9'999);
    std::copy(marker.begin(), marker.end(), config->data.begin() + 4097);
    std::copy(sized.begin(), sized.end(), config->data.begin() + 500'003);
    config->geometry = test::diskGeometry(config->data.size(), 4096);
    auto opener = std::make_shared<test::MockDeviceOpener>(config);
    storage::PhysicalDiskSource disk(1, opener);
    RECOVERY_ASSERT_OK(disk.open());
    std::vector<FileCandidate> candidates;
    run(disk, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_TRUE(carvedBytes(disk, candidates[0]) == marker);
    EXPECT_TRUE(carvedBytes(disk, candidates[1]) == sized);
    EXPECT_EQ(candidates[0].validation.status, ValidationStatus::Valid);
    EXPECT_EQ(candidates[1].validation.status, ValidationStatus::Valid);
    EXPECT_EQ(opener->stats()->alignmentViolations.load(), 0u);
}

TEST(FileCandidateTest, ValidationRejectsMalformedCandidates) {
    FileCandidate good;
    good.id = FileCandidateId{1};
    good.sourceOffset = 1000;
    good.length = 500;
    good.extents = {{0, 1000, 500}};
    RECOVERY_ASSERT_OK(validateFileCandidate(good));

    const std::vector<std::pair<std::string, std::function<void(FileCandidate&)>>> cases = {
        {"empty", [](FileCandidate& c) {
             c.length = 0;
             c.extents.clear();
         }},
        {"no extents", [](FileCandidate& c) { c.extents.clear(); }},
        {"short extents", [](FileCandidate& c) { c.extents = {{0, 1000, 499}}; }},
        {"gap", [](FileCandidate& c) { c.extents = {{0, 1000, 200}, {201, 1201, 299}}; }},
        {"empty extent", [](FileCandidate& c) { c.extents = {{0, 1000, 500}, {500, 1500, 0}}; }},
        {"two contiguous extents", [](FileCandidate& c) { c.extents = {{0, 1000, 200}, {200, 1200, 300}}; }},
        {"extent elsewhere", [](FileCandidate& c) { c.extents = {{0, 2000, 500}}; }},
        {"source overflow",
         [](FileCandidate& c) {
             c.sourceOffset = std::numeric_limits<std::uint64_t>::max() - 10;
             c.extents = {{0, c.sourceOffset, 500}};
         }},
        {"unknown extraction", [](FileCandidate& c) { c.extraction = static_cast<ExtractionStrategy>(7); }},
    };
    for (const auto& [name, change] : cases) {
        SCOPED_TRACE(name);
        FileCandidate candidate = good;
        change(candidate);
        RECOVERY_EXPECT_ERROR(validateFileCandidate(candidate), ErrorCode::InvalidInput);
    }
}

}  // namespace
}  // namespace recovery::carving
