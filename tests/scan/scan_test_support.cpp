#include "scan_test_support.hpp"

#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "evaluation/candidate_evaluation.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/fragment_recovery.hpp"
#include "recovery/mp4_recovery.hpp"
#include "support/audio_builders.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <utility>

namespace recovery::scan::test {

namespace {

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

Bytes photo(std::uint64_t seed, std::uint16_t width, std::uint16_t height) {
    ::recovery::test::JpegOptions options;
    options.width = width;
    options.height = height;
    options.seed = seed;
    return ::recovery::test::makeJpeg(options);
}

}  // namespace

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

Bytes makeCard(std::uint32_t clusters, std::uint64_t seed) {
    ::recovery::test::Fat32BuilderOptions geometry;
    geometry.clusterCount = clusters;
    ::recovery::test::Fat32ImageBuilder builder(geometry);
    const std::uint32_t root = ::recovery::test::Fat32ImageBuilder::root();
    // Old data in every cluster; the files below overwrite theirs.
    {
        Bytes& image = builder.raw();
        const Bytes noise = ::recovery::test::makePattern(static_cast<std::size_t>(clusters - 1) * kClusterSize, seed);
        std::copy(noise.begin(), noise.end(), image.begin() + static_cast<std::ptrdiff_t>(builder.clusterOffset(3)));
    }
    const auto place = [&](std::uint32_t first, const Bytes& bytes) {
        std::copy(bytes.begin(), bytes.end(),
                  builder.raw().begin() + static_cast<std::ptrdiff_t>(builder.clusterOffset(first)));
    };
    const auto scale = [&](std::uint32_t cluster) { return cluster * (clusters / 64) / 64; };

    // Active files: images, audio, a video, and a copy under another name.
    const Bytes jpeg = photo(1, 128, 96);
    (void)builder.addFileInClusters(root, "PHOTO.JPG", jpeg, run(scale(100), clustersFor(jpeg.size())));
    const Bytes png = ::recovery::test::makePng({});
    (void)builder.addFileInClusters(root, "PICTURE.PNG", png, run(scale(300), clustersFor(png.size())));
    const Bytes mp4 = ::recovery::test::makeMp4({}).bytes;
    (void)builder.addFileInClusters(root, "CLIP.MP4", mp4, run(scale(500), clustersFor(mp4.size())));
    const Bytes wav = ::recovery::test::makeWav({});
    (void)builder.addFileInClusters(root, "SOUND.WAV", wav, run(scale(900), clustersFor(wav.size())));
    (void)builder.addFileInClusters(root, "COPY.JPG", jpeg, run(scale(2000), clustersFor(jpeg.size())));

    // A deleted file whose clusters follow each other: its guessed layout holds.
    const Bytes old = photo(2, 96, 64);
    const auto oldEntry =
        builder.addFileInClusters(root, "OLD.JPG", old, run(scale(1200), clustersFor(old.size())));
    builder.deleteEntry(oldEntry);

    // A deleted file in two fragments, 24 clusters of old data apart:
    // fragment reconstruction puts it together.
    const Bytes fragmented = photo(3, 160, 120);
    const std::uint32_t total = clustersFor(fragmented.size());
    std::vector<std::uint32_t> pieces = run(scale(1500), total / 2);
    for (const std::uint32_t cluster : run(scale(1500) + total / 2 + 24, total - total / 2)) {
        pieces.push_back(cluster);
    }
    const auto fragEntry = builder.addFileInClusters(root, "FRAG.JPG", fragmented, pieces);
    builder.deleteEntry(fragEntry);

    // Files no entry names: carving finds them.
    place(scale(2400), ::recovery::test::makeGif({}));
    place(scale(2600), ::recovery::test::makeMp3({}));
    place(scale(3000), ::recovery::test::makeMp4({}).bytes);
    return builder.build();
}

ScanUpdateSink Collector::sink() {
    return [this](const ScanUpdate& update) -> Status {
        if (hook) {
            if (Status hooked = hook(update); !hooked.ok()) {
                return hooked;
            }
        }
        if (Status applied = checkpoint.apply(update); !applied.ok()) {
            ADD_FAILURE() << "an update the checkpoint refuses: " << applied.error().message;
            return applied;
        }
        ++updates;
        for (const evaluation::EvaluatedCandidate& candidate : update.candidates) {
            candidates.push_back(candidate);
        }
        return success();
    };
}

std::vector<evaluation::EvaluatedCandidate> referenceScan(storage::IStorageSource& source, ScanMode mode) {
    std::vector<evaluation::EvaluatedCandidate> delivered;
    std::unique_ptr<FilesystemRecovery> volume;
    CandidateScan scan;
    if (Result<std::unique_ptr<FilesystemRecovery>> opened = openFilesystemRecovery(source, 0); opened.ok()) {
        volume = std::move(opened).value();
        Result<CandidateScan> found = volume->findCandidates({}, {});
        EXPECT_TRUE(found.ok());
        if (found.ok()) {
            scan = std::move(found).value();
        }
    }
    std::vector<carving::FileCandidate> carves;
    std::vector<Mp4Candidate> mp4;
    std::vector<FragmentCandidate> fragments;
    if (mode == ScanMode::Deep) {
        Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(allFormats());
        EXPECT_TRUE(scanner.ok());
        carving::FileCarver carver(source);
        EXPECT_TRUE(carver
                        .run(*scanner,
                             [&](carving::FileCandidate&& carve) {
                                 carves.push_back(std::move(carve));
                                 return success();
                             })
                        .ok());
        Mp4Recovery mp4Recovery(source);
        if (volume != nullptr) {
            EXPECT_TRUE(mp4Recovery.addVolume(*volume, scan).ok());
        }
        EXPECT_TRUE(mp4Recovery
                        .run([&](Mp4Candidate&& candidate) {
                            mp4.push_back(std::move(candidate));
                            return success();
                        })
                        .ok());
        FragmentRecovery fragmentRecovery(source, allFormats());
        if (volume != nullptr) {
            EXPECT_TRUE(fragmentRecovery.addVolume(*volume, scan).ok());
        }
        EXPECT_TRUE(fragmentRecovery
                        .run([&](FragmentCandidate&& candidate) {
                            fragments.push_back(std::move(candidate));
                            return success();
                        })
                        .ok());
    }
    evaluation::CandidateEvaluation evaluation(source, allFormats(), allMedia());
    if (volume != nullptr) {
        EXPECT_TRUE(evaluation.addVolume(*volume, scan).ok());
    }
    for (carving::FileCandidate& carve : carves) {
        EXPECT_TRUE(evaluation.addCarve(std::move(carve)).ok());
    }
    for (Mp4Candidate& candidate : mp4) {
        EXPECT_TRUE(evaluation.addMp4Candidate(std::move(candidate)).ok());
    }
    for (FragmentCandidate& candidate : fragments) {
        EXPECT_TRUE(evaluation.addFragmentCandidate(std::move(candidate)).ok());
    }
    const Result<evaluation::EvaluationReport> report =
        evaluation.run([&](evaluation::EvaluatedCandidate&& candidate) {
            delivered.push_back(std::move(candidate));
            return success();
        });
    EXPECT_TRUE(report.ok());
    return delivered;
}

std::string describe(const evaluation::EvaluatedCandidate& candidate) {
    std::ostringstream line;
    line << "#" << candidate.id.value() << " " << candidate.data.filename << " [" << candidate.formatId << "] "
         << toString(candidate.data.method) << " " << carving::toString(candidate.validationStatus())
         << " size=" << candidate.data.expectedSize << " recovered=" << candidate.identity.size;
    if (candidate.identity.sha256.has_value()) {
        line << " sha256=" << candidate.identity.sha256->hex().substr(0, 16);
    }
    line << " regions=";
    for (const SourceRegion& region : candidate.data.sourceRegions) {
        line << "(" << region.fileOffset << "+" << region.length << " " << toString(region.kind) << "@"
             << region.sourceOffset << (region.reallocated ? " realloc" : "") << ")";
    }
    if (candidate.filesystemCandidate.has_value()) {
        line << " fs=" << candidate.filesystemCandidate->value();
    }
    if (candidate.mp4Candidate.has_value()) {
        line << " mp4=" << candidate.mp4Candidate->value();
    }
    if (candidate.carve.has_value()) {
        line << " carve=" << candidate.carve->id.value() << "@" << candidate.carve->sourceOffset << "+"
             << candidate.carve->length << ":" << carving::toString(candidate.carve->validation.status);
    }
    line << " others=" << candidate.otherCarves.size();
    if (candidate.fragments.has_value()) {
        line << " fragments=" << toString(candidate.fragments->status) << "/" << candidate.fragments->alternative;
    }
    if (candidate.container.has_value()) {
        line << " in=" << candidate.container->value();
    }
    if (candidate.duplicateOf.has_value()) {
        line << " dup=" << candidate.duplicateOf->value();
    }
    line << " unreadable=" << candidate.unreadableBytes << " warnings=";
    for (const evaluation::EvaluationWarning warning : candidate.warnings) {
        line << evaluation::toString(warning) << ",";
    }
    return line.str();
}

std::vector<std::string> describe(const std::vector<evaluation::EvaluatedCandidate>& candidates) {
    std::vector<std::string> lines;
    lines.reserve(candidates.size());
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        lines.push_back(describe(candidate));
    }
    return lines;
}

ScanRunOptions testRunOptions(std::uint32_t workers) {
    ScanRunOptions options;
    options.workerThreads = workers;
    options.blockSize = 64 * kKiB;
    options.cacheBlocks = 32;
    options.checkpointBytes = 128 * kKiB;
    options.checkpointItems = 3;
    options.checkpointInterval = std::chrono::milliseconds{60'000};
    options.progressInterval = std::chrono::milliseconds{0};
    return options;
}

}  // namespace recovery::scan::test
