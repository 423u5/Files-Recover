// Candidate evaluation (P14) on generated FAT32 cards whose free clusters
// hold old data: files the metadata knows (active, deleted with a layout
// that holds or not), carves of the whole card, MP4 recovery candidates and
// fragment reconstructions merged into one candidate per file; valid,
// truncated, invalid and unknown content; duplicate content under other
// names and other content under one name; and for every candidate, an
// identity equal to the hash of what recovery writes, warnings that follow
// from the evidence, and an explanation.

#include "evaluation/candidate_evaluation.hpp"

#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/crc32.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/mp4_recovery.hpp"
#include "recovery/sha256.hpp"
#include "support/audio_builders.hpp"
#include "support/candidate_helpers.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/mp4_samples.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "validation/media_validator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::evaluation {
namespace {

using Bytes = std::vector<std::byte>;
using carving::ValidationStatus;
using validation::LevelStatus;

constexpr std::uint32_t kClusterSize = 512;

std::uint32_t clustersFor(std::size_t bytes) {
    return static_cast<std::uint32_t>((bytes + kClusterSize - 1) / kClusterSize);
}

std::vector<std::uint32_t> run(std::uint32_t first, std::uint32_t count) {
    std::vector<std::uint32_t> clusters;
    for (std::uint32_t i = 0; i < count; ++i) {
        clusters.push_back(first + i);
    }
    return clusters;
}

// The registrations run before the checks: MSVC 14.51 (Release) builds an
// empty registry when they run inside EXPECT_TRUE in a lambda that
// initializes a static (found 2026-10-03).
const carving::FormatRegistry& allFormats() {
    static const carving::FormatRegistry registry = [] {
        carving::FormatRegistry formats;
        const bool images = formats::registerImageFormats(formats).ok();
        const bool audio = formats::registerAudioFormats(formats).ok();
        const bool video = formats::registerVideoFormats(formats).ok();
        EXPECT_TRUE(images && audio && video);
        return formats;
    }();
    return registry;
}

const validation::MediaValidatorRegistry& allMedia() {
    static const validation::MediaValidatorRegistry registry = [] {
        validation::MediaValidatorRegistry media;
        const bool registered = validation::registerMediaValidators(media).ok();
        EXPECT_TRUE(registered);
        return media;
    }();
    return registry;
}

Bytes photo(std::uint64_t seed, std::uint16_t width = 128, std::uint16_t height = 96) {
    test::JpegOptions options;
    options.width = width;
    options.height = height;
    options.seed = seed;
    return test::makeJpeg(options);
}

// A PNG whose zlib data breaks (a deflate block of the reserved type 3)
// while every chunk CRC is right: the structure holds, the media do not.
Bytes brokenDeflatePng() {
    Bytes png = test::makePng({});
    for (std::size_t at = 8; at + 12 <= png.size();) {
        const std::size_t length = loadBe32(png, at);
        if (std::to_integer<char>(png[at + 4]) == 'I' && std::to_integer<char>(png[at + 5]) == 'D') {
            png[at + 8 + 2] |= std::byte{0x06};  // after the zlib header: BTYPE 3
            const std::uint32_t crc = crc32(std::span(png).subspan(at + 4, 4 + length));
            for (int i = 0; i < 4; ++i) {
                png[at + 8 + length + static_cast<std::size_t>(i)] =
                    static_cast<std::byte>((crc >> (24 - 8 * i)) & 0xFF);
            }
            return png;
        }
        at += 12 + length;
    }
    ADD_FAILURE() << "no IDAT chunk";
    return png;
}

// A FAT32 card that has been used: the free clusters hold old data.
class Card {
public:
    explicit Card(std::uint32_t clusters = 4096, std::uint64_t seed = 0xE7A1) : builder_(geometry(clusters)) {
        Bytes& image = builder_.raw();
        const Bytes noise = test::makePattern(static_cast<std::size_t>(clusters - 1) * kClusterSize, seed);
        std::copy(noise.begin(), noise.end(), image.begin() + static_cast<std::ptrdiff_t>(builder_.clusterOffset(3)));
    }

    [[nodiscard]] test::Fat32ImageBuilder& builder() noexcept { return builder_; }
    test::Fat32ImageBuilder::Entry add(std::string_view name, const Bytes& bytes, std::uint32_t firstCluster,
                                       std::uint32_t directory = test::Fat32ImageBuilder::root()) {
        return builder_.addFileInClusters(directory, name, bytes, run(firstCluster, clustersFor(bytes.size())));
    }
    test::Fat32ImageBuilder::Entry addDeleted(std::string_view name, const Bytes& bytes,
                                              const std::vector<std::uint32_t>& clusters) {
        const test::Fat32ImageBuilder::Entry entry =
            builder_.addFileInClusters(test::Fat32ImageBuilder::root(), name, bytes, clusters);
        builder_.deleteEntry(entry);
        return entry;
    }
    void plant(std::uint32_t firstCluster, const Bytes& bytes) {
        std::copy(bytes.begin(), bytes.end(),
                  builder_.raw().begin() + static_cast<std::ptrdiff_t>(builder_.clusterOffset(firstCluster)));
    }
    [[nodiscard]] std::uint64_t offsetOf(std::uint32_t cluster) const { return builder_.clusterOffset(cluster); }
    [[nodiscard]] Bytes image() { return builder_.build(); }

private:
    static test::Fat32BuilderOptions geometry(std::uint32_t clusters) {
        test::Fat32BuilderOptions options;
        options.clusterCount = clusters;
        return options;
    }

    test::Fat32ImageBuilder builder_;
};

struct Volume {
    std::unique_ptr<FilesystemRecovery> recovery;
    CandidateScan scan;
};

Volume openVolume(storage::IStorageSource& source) {
    Volume opened;
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(source, 0);
    EXPECT_TRUE(recovery.ok());
    if (!recovery.ok()) {
        return opened;
    }
    opened.recovery = std::move(recovery).value();
    Result<CandidateScan> scan = opened.recovery->findCandidates({}, {});
    EXPECT_TRUE(scan.ok());
    if (scan.ok()) {
        opened.scan = std::move(scan).value();
    }
    return opened;
}

std::vector<carving::FileCandidate> carveAll(storage::IStorageSource& source) {
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(allFormats());
    EXPECT_TRUE(scanner.ok());
    std::vector<carving::FileCandidate> carves;
    if (!scanner.ok()) {
        return carves;
    }
    carving::FileCarver carver(source);
    Result<carving::CarveReport> report = carver.run(*scanner, [&](carving::FileCandidate&& carve) -> Status {
        carves.push_back(std::move(carve));
        return success();
    });
    EXPECT_TRUE(report.ok());
    return carves;
}

struct Inputs {
    Volume* volume = nullptr;
    std::vector<carving::FileCandidate> carves;
    std::vector<Mp4Candidate> mp4;
    std::vector<FragmentCandidate> fragments;
};

struct Evaluated {
    EvaluationReport report;
    std::vector<EvaluatedCandidate> candidates;

    [[nodiscard]] std::vector<const EvaluatedCandidate*> named(std::string_view filename) const {
        std::vector<const EvaluatedCandidate*> found;
        for (const EvaluatedCandidate& candidate : candidates) {
            if (candidate.data.filename == filename) {
                found.push_back(&candidate);
            }
        }
        return found;
    }
    [[nodiscard]] const EvaluatedCandidate* one(std::string_view filename) const {
        const std::vector<const EvaluatedCandidate*> found = named(filename);
        EXPECT_EQ(found.size(), 1U) << filename;
        return found.size() == 1 ? found.front() : nullptr;
    }
    [[nodiscard]] const EvaluatedCandidate* startingAt(std::uint64_t offset) const {
        for (const EvaluatedCandidate& candidate : candidates) {
            if (candidate.sourceOffset() == offset) {
                return &candidate;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const EvaluatedCandidate* byId(EvaluatedCandidateId id) const {
        for (const EvaluatedCandidate& candidate : candidates) {
            if (candidate.id == id) {
                return &candidate;
            }
        }
        return nullptr;
    }
};

std::string describeEvaluated(const EvaluatedCandidate& candidate) {
    std::string text;
    for (const std::string& line : explain(candidate)) {
        text += line + "\n";
    }
    return text;
}

// What every candidate satisfies, whatever its evidence.
void expectConsistent(storage::IStorageSource& source, const Evaluated& evaluated,
                      const EvaluatedCandidate& candidate) {
    SCOPED_TRACE(describeEvaluated(candidate));
    RECOVERY_EXPECT_OK(validateCandidate(candidate.data));
    // The identity is of exactly what recovery writes.
    const test::Reconstructed written = test::reconstructToMemory(source, candidate.data);
    ASSERT_TRUE(written.ok);
    EXPECT_EQ(candidate.recoveredSize(), written.data.size());
    ASSERT_TRUE(candidate.identity.sha256.has_value());
    EXPECT_EQ(*candidate.identity.sha256, sha256(written.data));
    ASSERT_TRUE(candidate.identity.preliminary.has_value());
    EXPECT_EQ(candidate.identity.preliminary->size, written.data.size());
    EXPECT_EQ(candidate.unreadableBytes, written.report.unreadableBytes);
    // Warnings follow from the facts.
    const validation::ValidationState& state = candidate.validation;
    EXPECT_EQ(candidate.hasWarning(EvaluationWarning::StructureInvalid),
              state.structural.status == LevelStatus::Failed);
    EXPECT_EQ(candidate.hasWarning(EvaluationWarning::MediaInvalid), state.media.status == LevelStatus::Failed);
    EXPECT_EQ(candidate.hasWarning(EvaluationWarning::FormatUnknown), candidate.formatId.empty());
    EXPECT_EQ(candidate.hasWarning(EvaluationWarning::DuplicateContent), candidate.duplicateOf.has_value());
    EXPECT_EQ(candidate.formatId.empty(), state.structural.status == LevelStatus::Unsupported);
    if (candidate.duplicateOf.has_value()) {
        const EvaluatedCandidate* original = evaluated.byId(*candidate.duplicateOf);
        ASSERT_NE(original, nullptr);
        EXPECT_LT(original->id, candidate.id);
        EXPECT_EQ(original->identity.sha256, candidate.identity.sha256);
        EXPECT_GT(candidate.recoveredSize(), 0U);
    }
    // Names: the metadata's, or recovered_<id>.<ext>.
    if (!candidate.hasFilesystemEvidence()) {
        EXPECT_EQ(candidate.data.filename.rfind("recovered_", 0), 0U);
        EXPECT_NE(candidate.data.filename.find(std::to_string(candidate.id.value())), std::string::npos);
    }
    // The explanation says what was checked.
    const std::vector<std::string> lines = explain(candidate);
    ASSERT_GE(lines.size(), 5U);
    const std::string all = describeEvaluated(candidate);
    EXPECT_NE(all.find("Validation status: " + std::string(carving::toString(candidate.validationStatus()))),
              std::string::npos);
    EXPECT_NE(all.find("SHA-256 " + candidate.identity.sha256->hex()), std::string::npos);
}

Evaluated evaluateAll(storage::IStorageSource& source, Inputs inputs, EvaluationOptions options = {}) {
    Evaluated evaluated;
    CandidateEvaluation evaluation(source, allFormats(), allMedia(), options);
    if (inputs.volume != nullptr) {
        RECOVERY_EXPECT_OK(evaluation.addVolume(*inputs.volume->recovery, inputs.volume->scan));
    }
    for (carving::FileCandidate& carve : inputs.carves) {
        RECOVERY_EXPECT_OK(evaluation.addCarve(std::move(carve)));
    }
    for (Mp4Candidate& candidate : inputs.mp4) {
        RECOVERY_EXPECT_OK(evaluation.addMp4Candidate(std::move(candidate)));
    }
    for (FragmentCandidate& candidate : inputs.fragments) {
        RECOVERY_EXPECT_OK(evaluation.addFragmentCandidate(std::move(candidate)));
    }
    Result<EvaluationReport> report = evaluation.run([&](EvaluatedCandidate&& candidate) -> Status {
        evaluated.candidates.push_back(std::move(candidate));
        return success();
    });
    EXPECT_TRUE(report.ok()) << (report.ok() ? "" : describe(report.error()));
    if (report.ok()) {
        evaluated.report = *report;
        EXPECT_EQ(evaluated.report.candidates(), evaluated.candidates.size());
    }
    std::uint64_t id = options.firstId;
    for (const EvaluatedCandidate& candidate : evaluated.candidates) {
        EXPECT_EQ(candidate.id.value(), id++);
        expectConsistent(source, evaluated, candidate);
    }
    return evaluated;
}

// ===========================================================================
// Valid and invalid candidates
// ===========================================================================

TEST(CandidateEvaluationTest, ValidFilesAreValidAtEveryLevel) {
    Card card;
    const Bytes jpeg = photo(1);
    const Bytes png = test::makePng({});
    const Bytes gif = test::makeGif({});
    const Bytes bmp = test::makeBmp({});
    const Bytes wav = test::makeWav({});
    card.add("PHOTO.JPG", jpeg, 100);
    card.add("PICTURE.PNG", png, 200);
    card.add("ANIM.GIF", gif, 300);
    card.add("DRAWING.BMP", bmp, 400);
    card.add("VOICE.WAV", wav, 500);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    const Evaluated evaluated = evaluateAll(source, Inputs{&volume});
    ASSERT_EQ(evaluated.candidates.size(), 5U);
    const std::vector<std::pair<std::string_view, std::pair<std::string_view, const Bytes*>>> expected = {
        {"PHOTO.JPG", {"jpeg", &jpeg}},  {"PICTURE.PNG", {"png", &png}},  {"ANIM.GIF", {"gif", &gif}},
        {"DRAWING.BMP", {"bmp", &bmp}}, {"VOICE.WAV", {"wav", &wav}}};
    for (const auto& [name, format] : expected) {
        const EvaluatedCandidate* candidate = evaluated.one(name);
        ASSERT_NE(candidate, nullptr);
        SCOPED_TRACE(describeEvaluated(*candidate));
        EXPECT_EQ(candidate->formatId, format.first);
        EXPECT_EQ(candidate->recoveryMethod(), RecoveryMethod::Filesystem);
        EXPECT_TRUE(candidate->hasFilesystemEvidence());
        EXPECT_EQ(candidate->validationStatus(), ValidationStatus::Valid);
        EXPECT_EQ(candidate->validation.structural.status, LevelStatus::Passed);
        // Uncompressed pixels and PCM samples have nothing coded.
        const bool uncoded = format.first == "wav" || format.first == "bmp";
        EXPECT_EQ(candidate->validation.media.status, uncoded ? LevelStatus::NotApplicable : LevelStatus::Passed);
        EXPECT_EQ(candidate->validation.playability.status, LevelStatus::NotRun);
        EXPECT_EQ(candidate->identity.sha256, sha256(*format.second));
        EXPECT_EQ(candidate->recoveredSize(), format.second->size());
        EXPECT_EQ(candidate->fragmentationCount(), 1U);
        EXPECT_TRUE(candidate->warnings.empty());
        EXPECT_FALSE(candidate->duplicateOf.has_value());
    }
    EXPECT_EQ(evaluated.report.valid, 5U);
    EXPECT_EQ(evaluated.report.filesystem, 5U);
}

TEST(CandidateEvaluationTest, InvalidTruncatedAndUnknownContentIsSaid) {
    Card card;
    Bytes badCrc = test::makePng({});
    badCrc[30] ^= std::byte{0x01};  // inside IHDR: its CRC no longer matches
    const Bytes jpeg = photo(2);
    const Bytes cutJpeg(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(jpeg.size() * 3 / 5));
    card.add("BADCRC.PNG", badCrc, 100);
    card.add("BROKEN.PNG", brokenDeflatePng(), 200);
    card.add("CUT.JPG", cutJpeg, 300);
    card.add("NOTES.TXT", test::makePattern(700, 3), 400);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    const Evaluated evaluated = evaluateAll(source, Inputs{&volume});

    const EvaluatedCandidate* crc = evaluated.one("BADCRC.PNG");
    ASSERT_NE(crc, nullptr);
    EXPECT_EQ(crc->validationStatus(), ValidationStatus::Invalid);
    EXPECT_EQ(crc->validation.structural.status, LevelStatus::Failed);
    EXPECT_EQ(crc->validation.media.status, LevelStatus::NotRun);
    EXPECT_TRUE(crc->hasWarning(EvaluationWarning::StructureInvalid));

    const EvaluatedCandidate* broken = evaluated.one("BROKEN.PNG");
    ASSERT_NE(broken, nullptr);
    EXPECT_EQ(broken->validation.structural.status, LevelStatus::Passed);
    EXPECT_EQ(broken->validation.media.status, LevelStatus::Failed);
    EXPECT_EQ(broken->validationStatus(), ValidationStatus::Invalid);
    EXPECT_TRUE(broken->hasWarning(EvaluationWarning::MediaInvalid));
    EXPECT_NE(describeEvaluated(*broken).find("Media level: failed (png decoder)"), std::string::npos);

    const EvaluatedCandidate* cut = evaluated.one("CUT.JPG");
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(cut->validationStatus(), ValidationStatus::Truncated);
    EXPECT_TRUE(cut->hasWarning(EvaluationWarning::ContentTruncated));

    const EvaluatedCandidate* notes = evaluated.one("NOTES.TXT");
    ASSERT_NE(notes, nullptr);
    EXPECT_TRUE(notes->formatId.empty());
    EXPECT_EQ(notes->validationStatus(), ValidationStatus::NotValidated);
    EXPECT_TRUE(notes->hasWarning(EvaluationWarning::FormatUnknown));
    EXPECT_NE(describeEvaluated(*notes).find("(no known format)"), std::string::npos);

    EXPECT_EQ(evaluated.report.invalid, 2U);
    EXPECT_EQ(evaluated.report.truncated, 1U);
    EXPECT_EQ(evaluated.report.notValidated, 1U);
}

// ===========================================================================
// Content identity: duplicates, names
// ===========================================================================

TEST(CandidateEvaluationTest, DifferentNamesWithTheSameContentAreDuplicates) {
    Card card;
    const Bytes jpeg = photo(3);
    card.add("IMG_0001.JPG", jpeg, 100);
    card.add("COPY OF IMG_0001.JPG", jpeg, 200);
    // A deleted copy elsewhere, written in one piece (its guessed layout holds).
    card.addDeleted("Old copy.jpg", jpeg, run(300, clustersFor(jpeg.size())));
    // Empty files are never duplicates of each other.
    card.builder().addFile(test::Fat32ImageBuilder::root(), "EMPTY1.DAT", {});
    card.builder().addFile(test::Fat32ImageBuilder::root(), "EMPTY2.DAT", {});
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    const Evaluated evaluated = evaluateAll(source, Inputs{&volume});

    const EvaluatedCandidate* original = evaluated.one("IMG_0001.JPG");
    const EvaluatedCandidate* copy = evaluated.one("COPY OF IMG_0001.JPG");
    const EvaluatedCandidate* deleted = evaluated.one("Old copy.jpg");
    ASSERT_NE(original, nullptr);
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(deleted, nullptr);
    EXPECT_FALSE(original->duplicateOf.has_value());
    EXPECT_EQ(copy->duplicateOf, original->id);
    EXPECT_EQ(deleted->duplicateOf, original->id);
    EXPECT_TRUE(copy->hasWarning(EvaluationWarning::DuplicateContent));
    EXPECT_NE(describeEvaluated(*copy).find("Duplicate: the same content (SHA-256) as candidate " +
                                            std::to_string(original->id.value())),
              std::string::npos);
    // Duplicates stay candidates, valid as the original is.
    EXPECT_EQ(copy->validationStatus(), ValidationStatus::Valid);
    // A deleted file whose guessed layout its structure validates is HYBRID (P12's rule).
    EXPECT_EQ(deleted->recoveryMethod(), RecoveryMethod::Hybrid);
    for (const std::string_view name : {"EMPTY1.DAT", "EMPTY2.DAT"}) {
        const EvaluatedCandidate* empty = evaluated.one(name);
        ASSERT_NE(empty, nullptr);
        EXPECT_EQ(empty->recoveredSize(), 0U);
        EXPECT_FALSE(empty->duplicateOf.has_value());
    }
    EXPECT_EQ(evaluated.report.duplicates, 2U);
}

TEST(CandidateEvaluationTest, TheSameNameWithOtherContentIsNotADuplicate) {
    Card card;
    const test::Fat32ImageBuilder::Entry holiday =
        card.builder().addDirectory(test::Fat32ImageBuilder::root(), "HOLIDAY");
    const test::Fat32ImageBuilder::Entry work = card.builder().addDirectory(test::Fat32ImageBuilder::root(), "WORK");
    const Bytes first = photo(4);
    const Bytes second = photo(5);
    card.add("PHOTO.JPG", first, 100, holiday.clusters.front());
    card.add("PHOTO.JPG", second, 200, work.clusters.front());
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    const Evaluated evaluated = evaluateAll(source, Inputs{&volume});
    const std::vector<const EvaluatedCandidate*> photos = evaluated.named("PHOTO.JPG");
    ASSERT_EQ(photos.size(), 2U);
    EXPECT_NE(photos[0]->data.filesystemEvidence.path, photos[1]->data.filesystemEvidence.path);
    EXPECT_NE(photos[0]->identity.sha256, photos[1]->identity.sha256);
    EXPECT_FALSE(photos[0]->duplicateOf.has_value());
    EXPECT_FALSE(photos[1]->duplicateOf.has_value());
    EXPECT_EQ(evaluated.report.duplicates, 0U);
}

// ===========================================================================
// One candidate per file: carves, MP4 candidates, reconstructions
// ===========================================================================

TEST(CandidateEvaluationTest, ACarveOfAFileTheMetadataKnowsJoinsIt) {
    Card card;
    const Bytes active = photo(6);
    card.add("ACTIVE.JPG", active, 100);
    // A deleted file whose entry records a smaller size than the file has:
    // its own layout is cut short, the carve at its start is whole.
    const Bytes deleted = photo(7);
    const test::Fat32ImageBuilder::Entry entry =
        card.addDeleted("Short size.jpg", deleted, run(300, clustersFor(deleted.size())));
    const std::span<std::byte> shortEntry = card.builder().shortEntry(entry);
    const auto recorded = static_cast<std::uint32_t>(deleted.size() / 2);
    for (int i = 0; i < 4; ++i) {
        shortEntry[28 + static_cast<std::size_t>(i)] = static_cast<std::byte>((recorded >> (8 * i)) & 0xFF);
    }
    // A photo in free space that no entry names.
    const Bytes lost = test::makePng({});
    card.plant(1500, lost);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    Inputs inputs{&volume};
    inputs.carves = carveAll(source);
    const Evaluated evaluated = evaluateAll(source, std::move(inputs));

    // The active file: its layout stands, the carve is its evidence, not a candidate of its own.
    const EvaluatedCandidate* kept = evaluated.one("ACTIVE.JPG");
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->recoveryMethod(), RecoveryMethod::Filesystem);
    ASSERT_TRUE(kept->carve.has_value());
    EXPECT_EQ(kept->carve->sourceOffset, card.offsetOf(100));
    ASSERT_NE(kept->signature(), nullptr);
    EXPECT_NE(describeEvaluated(*kept).find("the metadata's layout stands"), std::string::npos);
    EXPECT_EQ(kept->identity.sha256, sha256(active));

    // The deleted file: HYBRID, the metadata's name and the carve's layout and length.
    const EvaluatedCandidate* hybrid = evaluated.one("Short size.jpg");
    ASSERT_NE(hybrid, nullptr);
    SCOPED_TRACE(describeEvaluated(*hybrid));
    EXPECT_EQ(hybrid->recoveryMethod(), RecoveryMethod::Hybrid);
    EXPECT_EQ(hybrid->data.expectedSize, deleted.size());
    EXPECT_EQ(hybrid->validationStatus(), ValidationStatus::Valid);
    EXPECT_EQ(hybrid->identity.sha256, sha256(deleted));
    EXPECT_FALSE(hybrid->data.fragmentation.known);

    // The lost photo: a candidate of its own, in free clusters.
    const EvaluatedCandidate* carved = evaluated.startingAt(card.offsetOf(1500));
    ASSERT_NE(carved, nullptr);
    EXPECT_EQ(carved->recoveryMethod(), RecoveryMethod::Carving);
    EXPECT_EQ(carved->formatId, "png");
    EXPECT_EQ(carved->validationStatus(), ValidationStatus::Valid);
    EXPECT_EQ(carved->identity.sha256, sha256(lost));
    ASSERT_TRUE(carved->allocation.has_value());
    EXPECT_EQ(carved->allocation->allocatedClusters, 0U);
    EXPECT_EQ(carved->allocation->freeClusters, clustersFor(lost.size()));
    EXPECT_FALSE(carved->hasFilesystemEvidence());

    // Nothing is listed twice.
    for (const EvaluatedCandidate& candidate : evaluated.candidates) {
        if (&candidate != kept && &candidate != hybrid) {
            EXPECT_NE(candidate.sourceOffset(), card.offsetOf(100));
            EXPECT_NE(candidate.sourceOffset(), card.offsetOf(300));
        }
    }
    EXPECT_GE(evaluated.report.carvesMerged, 2U);
    EXPECT_EQ(evaluated.report.hybrid, 1U);
}

TEST(CandidateEvaluationTest, ACarveInsideAnActiveFileIsLinkedToIt) {
    Card card;
    // A file that holds a whole photo at its middle (a motion photo, an archive).
    Bytes container = test::makePattern(4 * kClusterSize, 8);
    const Bytes inner = photo(9, 64, 48);
    container.insert(container.end(), inner.begin(), inner.end());
    const Bytes tail = test::makePattern(2 * kClusterSize, 10);
    container.insert(container.end(), tail.begin(), tail.end());
    card.add("BUNDLE.BIN", container, 100);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    Inputs inputs{&volume};
    inputs.carves = carveAll(source);
    const Evaluated evaluated = evaluateAll(source, std::move(inputs));
    const EvaluatedCandidate* bundle = evaluated.one("BUNDLE.BIN");
    ASSERT_NE(bundle, nullptr);
    const EvaluatedCandidate* part = evaluated.startingAt(card.offsetOf(100) + 4 * kClusterSize);
    ASSERT_NE(part, nullptr);
    SCOPED_TRACE(describeEvaluated(*part));
    EXPECT_EQ(part->formatId, "jpeg");
    EXPECT_EQ(part->container, bundle->id);
    EXPECT_TRUE(part->hasWarning(EvaluationWarning::InsideActiveFile));
    ASSERT_TRUE(part->allocation.has_value());
    EXPECT_TRUE(part->allocation->insideActiveFile);
    // Its clusters are the container's: not marked reallocated.
    EXPECT_EQ(part->data.reallocatedBytes(), 0U);
    EXPECT_NE(describeEvaluated(*part).find("Part of candidate " + std::to_string(bundle->id.value())),
              std::string::npos);
}

TEST(CandidateEvaluationTest, AnMp4CandidateReplacesItsFilesystemCandidateAndCarve) {
    Card card(8192);
    const Bytes video = test::mp4_samples::named("ffmpeg_h264_aac.mp4").data();
    card.add("CLIP.MP4", video, 1000);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    Inputs inputs{&volume};
    Mp4Recovery recovery(source);
    RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
    Result<Mp4RecoveryReport> report = recovery.run([&](Mp4Candidate&& candidate) -> Status {
        inputs.mp4.push_back(std::move(candidate));
        return success();
    });
    RECOVERY_ASSERT_OK(report);
    ASSERT_EQ(inputs.mp4.size(), 1U);
    inputs.carves = carveAll(source);
    const Evaluated evaluated = evaluateAll(source, std::move(inputs));
    const EvaluatedCandidate* clip = evaluated.one("CLIP.MP4");
    ASSERT_NE(clip, nullptr);
    SCOPED_TRACE(describeEvaluated(*clip));
    EXPECT_EQ(clip->formatId, "mp4");
    ASSERT_TRUE(clip->mp4.has_value());
    EXPECT_TRUE(clip->mp4Candidate.has_value());
    EXPECT_TRUE(clip->filesystemCandidate.has_value());
    EXPECT_EQ(clip->validationStatus(), ValidationStatus::Valid);
    EXPECT_EQ(clip->validation.media.status, LevelStatus::Passed);
    EXPECT_EQ(clip->identity.sha256, sha256(video));
    // The MP4 file is one candidate: no other starts where it starts.
    std::size_t atStart = 0;
    for (const EvaluatedCandidate& candidate : evaluated.candidates) {
        atStart += candidate.sourceOffset() == card.offsetOf(1000) ? 1 : 0;
    }
    EXPECT_EQ(atStart, 1U);
    EXPECT_GE(evaluated.report.superseded, 1U);
    EXPECT_NE(describeEvaluated(*clip).find("MP4: structure valid"), std::string::npos);
}

TEST(CandidateEvaluationTest, AReconstructionReplacesTheGuessedLayout) {
    Card card;
    // A deleted photo written around a file that still exists: its guessed
    // (contiguous) layout runs into the other file.
    const Bytes jpeg = photo(11, 256, 192);
    const std::uint32_t clusters = clustersFor(jpeg.size());
    card.add("KEEP.BIN", test::makePattern(6 * kClusterSize, 12), 110);
    std::vector<std::uint32_t> layout = run(100, 10);
    for (std::uint32_t i = 0; i < clusters - 10; ++i) {
        layout.push_back(116 + i);
    }
    card.addDeleted("Trip photo.jpg", jpeg, layout);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    Inputs inputs{&volume};
    FragmentRecovery recovery(source, allFormats());
    RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
    Result<FragmentRecoveryReport> report = recovery.run([&](FragmentCandidate&& candidate) -> Status {
        inputs.fragments.push_back(std::move(candidate));
        return success();
    });
    RECOVERY_ASSERT_OK(report);
    inputs.carves = carveAll(source);
    const Evaluated evaluated = evaluateAll(source, std::move(inputs));
    const EvaluatedCandidate* trip = evaluated.one("Trip photo.jpg");
    ASSERT_NE(trip, nullptr);
    SCOPED_TRACE(describeEvaluated(*trip));
    EXPECT_EQ(trip->recoveryMethod(), RecoveryMethod::Fragmented);
    EXPECT_EQ(trip->fragmentationCount(), 2U);
    ASSERT_TRUE(trip->fragments.has_value());
    EXPECT_EQ(trip->fragments->status, ReconstructionStatus::Complete);
    EXPECT_EQ(trip->validationStatus(), ValidationStatus::Valid);
    EXPECT_EQ(trip->validation.media.status, LevelStatus::Passed);
    EXPECT_EQ(trip->identity.sha256, sha256(jpeg));
    EXPECT_NE(describeEvaluated(*trip).find("Fragment reconstruction (filesystem seed): COMPLETE"), std::string::npos);
    EXPECT_EQ(describeEvaluated(*trip).find("Merged:"), std::string::npos);
}

// Synthetic reconstructions: the seed is "Trip photo.jpg"'s filesystem candidate.
struct SeededCard {
    Card card;
    Bytes jpeg = photo(13);
    std::unique_ptr<test::MemoryStorageSource> source;
    Volume volume;
    const RecoveryCandidate* seed = nullptr;

    SeededCard() {
        card.addDeleted("Trip photo.jpg", jpeg, run(100, clustersFor(jpeg.size())));
        source = std::make_unique<test::MemoryStorageSource>(card.image());
        EXPECT_TRUE(source->open().ok());
        volume = openVolume(*source);
        seed = test::candidateAt(volume.scan, "/Trip photo.jpg");
    }

    // A hypothesis of `layout` (clusters), validated here.
    [[nodiscard]] ReconstructionHypothesis hypothesis(const std::vector<std::uint32_t>& layout) const {
        ReconstructionHypothesis h;
        h.data = *seed;
        const bool split = layout.size() > 1 && layout[1] != layout[0] + 1;
        h.data.method = split ? RecoveryMethod::Fragmented : RecoveryMethod::Hybrid;
        h.data.sourceRegions.clear();
        std::uint64_t file = 0;
        for (const std::uint32_t cluster : layout) {
            const std::uint64_t length = std::min<std::uint64_t>(kClusterSize, seed->expectedSize - file);
            h.data.sourceRegions.push_back(
                SourceRegion{file, length, RegionKind::Stored, card.offsetOf(cluster), false});
            file += length;
        }
        h.data.fragmentation = FragmentationInfo{split ? 2U : 1U, false};
        h.data.warnings.clear();
        Result<std::unique_ptr<CandidateContentReader>> reader = CandidateContentReader::open(*source, h.data);
        EXPECT_TRUE(reader.ok());
        if (reader.ok()) {
            Result<carving::ValidationResult> verdict = allFormats().find("jpeg")->validator().validate(**reader);
            EXPECT_TRUE(verdict.ok());
            if (verdict.ok()) {
                h.validation = *verdict;
            }
        }
        return h;
    }
    [[nodiscard]] FragmentCandidate reconstruction(ReconstructionStatus status) const {
        FragmentCandidate fragment;
        fragment.id = CandidateId{77};
        fragment.status = status;
        fragment.reason = "synthetic";
        fragment.origin = SeedOrigin::Filesystem;
        fragment.formatId = "jpeg";
        fragment.name = seed->filename;
        fragment.path = seed->filesystemEvidence.path;
        fragment.filesystemCandidate = seed->id;
        fragment.recordedSize = seed->expectedSize;
        fragment.clusterSize = kClusterSize;
        return fragment;
    }
};

TEST(CandidateEvaluationTest, AnAmbiguousReconstructionGivesEveryTiedLayout) {
    SeededCard seeded;
    ASSERT_NE(seeded.seed, nullptr);
    FragmentCandidate ambiguous = seeded.reconstruction(ReconstructionStatus::Ambiguous);
    const std::uint32_t clusters = clustersFor(seeded.jpeg.size());
    ambiguous.hypotheses.push_back(seeded.hypothesis(run(100, clusters)));
    std::vector<std::uint32_t> other = run(100, clusters - 2);
    other.push_back(2000);
    other.push_back(2001);
    ambiguous.hypotheses.push_back(seeded.hypothesis(other));
    ambiguous.tied = 2;
    Inputs inputs{&seeded.volume};
    inputs.fragments.push_back(std::move(ambiguous));
    const Evaluated evaluated = evaluateAll(*seeded.source, std::move(inputs));
    const std::vector<const EvaluatedCandidate*> trips = evaluated.named("Trip photo.jpg");
    ASSERT_EQ(trips.size(), 2U);
    for (std::size_t i = 0; i < 2; ++i) {
        SCOPED_TRACE(describeEvaluated(*trips[i]));
        EXPECT_TRUE(trips[i]->hasWarning(EvaluationWarning::AlternativeLayout));
        ASSERT_TRUE(trips[i]->fragments.has_value());
        EXPECT_EQ(trips[i]->fragments->alternative, i + 1);
        EXPECT_EQ(trips[i]->fragments->tied, 2U);
        EXPECT_NE(describeEvaluated(*trips[i]).find("2 tied: this is " + std::to_string(i + 1) + " of 2"),
                  std::string::npos);
    }
    EXPECT_EQ(trips[0]->identity.sha256, sha256(seeded.jpeg));
    EXPECT_NE(trips[1]->identity.sha256, trips[0]->identity.sha256);
    EXPECT_EQ(evaluated.report.alternatives, 2U);
}

TEST(CandidateEvaluationTest, AnUnrecoverableReconstructionKeepsTheMetadataLayout) {
    SeededCard seeded;
    ASSERT_NE(seeded.seed, nullptr);
    Inputs inputs{&seeded.volume};
    inputs.fragments.push_back(seeded.reconstruction(ReconstructionStatus::Unrecoverable));
    const Evaluated evaluated = evaluateAll(*seeded.source, std::move(inputs));
    const EvaluatedCandidate* trip = evaluated.one("Trip photo.jpg");
    ASSERT_NE(trip, nullptr);
    SCOPED_TRACE(describeEvaluated(*trip));
    EXPECT_TRUE(trip->hasWarning(EvaluationWarning::ReconstructionFailed));
    ASSERT_TRUE(trip->fragments.has_value());
    EXPECT_EQ(trip->fragments->status, ReconstructionStatus::Unrecoverable);
    EXPECT_EQ(trip->data.sourceRegions, seeded.seed->sourceRegions);
}

TEST(CandidateEvaluationTest, ADeletedFileWhoseStartIsTakenGetsNoCarve) {
    Card card;
    // Deleted, then its clusters reused by a file that still exists.
    const Bytes old = photo(16);
    card.addDeleted("Gone photo.jpg", old, run(100, clustersFor(old.size())));
    const Bytes now = test::makePng({});
    card.add("NOW.PNG", now, 100);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    Inputs inputs{&volume};
    inputs.carves = carveAll(source);
    const Evaluated evaluated = evaluateAll(source, std::move(inputs));
    const EvaluatedCandidate* gone = evaluated.one("Gone photo.jpg");
    const EvaluatedCandidate* current = evaluated.one("NOW.PNG");
    ASSERT_NE(gone, nullptr);
    ASSERT_NE(current, nullptr);
    // The carve at cluster 100 is the new owner's.
    ASSERT_TRUE(current->carve.has_value());
    EXPECT_EQ(current->carve->formatId, "png");
    EXPECT_FALSE(gone->carve.has_value());
    EXPECT_EQ(gone->recoveryMethod(), RecoveryMethod::Filesystem);
    EXPECT_TRUE(gone->data.hasWarning(CandidateWarning::ClustersReallocated));
    EXPECT_NE(gone->validationStatus(), ValidationStatus::Valid);
}

TEST(CandidateEvaluationTest, UnreadableBytesAreZerosInTheChecksAndTheHash) {
    Card card;
    const Bytes jpeg = photo(17, 256, 192);
    card.add("DAMAGED.JPG", jpeg, 100);
    test::MemoryStorageSource source(card.image());
    // A bad sector in the middle of the photo.
    source.addBadSector((card.offsetOf(100) + jpeg.size() / 2) / kClusterSize);
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    const Evaluated evaluated = evaluateAll(source, Inputs{&volume});
    const EvaluatedCandidate* damaged = evaluated.one("DAMAGED.JPG");
    ASSERT_NE(damaged, nullptr);
    SCOPED_TRACE(describeEvaluated(*damaged));
    EXPECT_EQ(damaged->unreadableBytes, kClusterSize);
    EXPECT_TRUE(damaged->hasWarning(EvaluationWarning::DataUnreadable));
    EXPECT_NE(damaged->identity.sha256, sha256(jpeg));
    EXPECT_NE(describeEvaluated(*damaged).find("512 bytes unreadable, read as zeros"), std::string::npos);
}

// ===========================================================================
// Inputs, options, cancellation
// ===========================================================================

TEST(CandidateEvaluationTest, InputsAndOptionsAreChecked) {
    Card card;
    card.add("PHOTO.JPG", photo(14), 100);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    CandidateEvaluation evaluation(source, allFormats(), allMedia());
    RECOVERY_EXPECT_OK(evaluation.addVolume(*volume.recovery, volume.scan));
    RECOVERY_EXPECT_ERROR(evaluation.addVolume(*volume.recovery, volume.scan), ErrorCode::InvalidInput);
    CandidateScan other = volume.scan;
    other.volumeOffset = 4096;
    CandidateEvaluation another(source, allFormats(), allMedia());
    RECOVERY_EXPECT_ERROR(another.addVolume(*volume.recovery, other), ErrorCode::InvalidInput);
    carving::FileCandidate empty;
    RECOVERY_EXPECT_ERROR(evaluation.addCarve(empty), ErrorCode::InvalidInput);

    EvaluationOptions options;
    options.maxClusterChecks = 0;
    EXPECT_FALSE(validate(options).ok());
    CandidateEvaluation invalid(source, allFormats(), allMedia(), options);
    RECOVERY_EXPECT_ERROR(invalid.run([](EvaluatedCandidate&&) { return success(); }), ErrorCode::InvalidInput);

    // Cancelled before the first candidate.
    CancellationSource cancel;
    cancel.requestCancellation();
    EvaluationOptions cancelling;
    cancelling.reads.cancellation = cancel.token();
    CandidateEvaluation stopped(source, allFormats(), allMedia(), cancelling);
    RECOVERY_EXPECT_OK(stopped.addVolume(*volume.recovery, volume.scan));
    RECOVERY_EXPECT_ERROR(stopped.run([](EvaluatedCandidate&&) { return success(); }), ErrorCode::Cancelled);

    // The sink's error stops the run.
    CandidateEvaluation sinking(source, allFormats(), allMedia());
    RECOVERY_EXPECT_OK(sinking.addVolume(*volume.recovery, volume.scan));
    const auto full = [](EvaluatedCandidate&&) { return makeError(ErrorCode::DestinationError, "full"); };
    RECOVERY_EXPECT_ERROR(sinking.run(full), ErrorCode::DestinationError);
}

TEST(CandidateEvaluationTest, IdsAndHashesFollowTheOptions) {
    Card card;
    const Bytes jpeg = photo(15);
    card.add("A.JPG", jpeg, 100);
    card.add("B.JPG", jpeg, 200);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    EvaluationOptions options;
    options.firstId = 500;
    options.identity.sha256 = false;
    CandidateEvaluation evaluation(source, allFormats(), allMedia(), options);
    RECOVERY_ASSERT_OK(evaluation.addVolume(*volume.recovery, volume.scan));
    std::vector<EvaluatedCandidate> candidates;
    Result<EvaluationReport> report = evaluation.run([&](EvaluatedCandidate&& candidate) -> Status {
        candidates.push_back(std::move(candidate));
        return success();
    });
    RECOVERY_ASSERT_OK(report);
    ASSERT_EQ(candidates.size(), 2U);
    EXPECT_EQ(candidates[0].id.value(), 500U);
    EXPECT_EQ(candidates[1].id.value(), 501U);
    // Without SHA-256 there is no duplicate detection; the preliminary hashes are equal.
    EXPECT_FALSE(candidates[1].duplicateOf.has_value());
    EXPECT_FALSE(candidates[0].identity.sha256.has_value());
    ASSERT_TRUE(candidates[0].identity.preliminary.has_value());
    EXPECT_EQ(candidates[0].identity.preliminary, candidates[1].identity.preliminary);
    EXPECT_EQ(report->bytesHashed, 0U);
}

}  // namespace
}  // namespace recovery::evaluation
