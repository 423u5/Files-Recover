// Fragment reconstruction (P13): files fragmented on generated FAT32 and
// exFAT volumes whose free clusters hold old data, as on a card that has been
// used. Known fragmented files (around files that still exist, around deleted
// files, across old data), missing fragments, fragments out of order,
// overwritten fragments, ambiguous layouts; filesystem and carve seeds; MP4
// placed by its sample tables; every hypothesis validated again here, and the
// reconstructions compared with the original files.

#include "recovery/fragment_recovery.hpp"

#include "carving/content_reader.hpp"
#include "diagnostics/logger.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/recovery_writer.hpp"
#include "support/audio_builders.hpp"
#include "support/candidate_helpers.hpp"
#include "support/fat32_builder.hpp"
#include "support/exfat_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/mp4_builders.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace recovery {
namespace {

using Bytes = std::vector<std::byte>;
using carving::ValidationStatus;

constexpr std::uint32_t kClusterSize = 512;  // the builders' default geometry

// ---------------------------------------------------------------------------
// Files and cards
// ---------------------------------------------------------------------------

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

// The clusters of a file in pieces: `pieces` holds each piece's first cluster
// and length; the last piece takes what is left of `total`.
std::vector<std::uint32_t> pieces(std::uint32_t total,
                                  std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> parts) {
    std::vector<std::uint32_t> clusters;
    for (const auto& [first, count] : parts) {
        for (std::uint32_t i = 0; i < count && clusters.size() < total; ++i) {
            clusters.push_back(first + i);
        }
    }
    return clusters;
}

Bytes photo(std::uint64_t seed, std::uint16_t restartInterval = 0) {
    test::JpegOptions options;
    options.width = 256;
    options.height = 192;
    options.seed = seed;
    options.restartInterval = restartInterval;
    return test::makeJpeg(options);
}

// A FAT32 card that has been used: every cluster the test does not write
// holds old data (noise), as free space does on real media.
class Card {
public:
    explicit Card(std::uint32_t clusters = 8192, std::uint64_t seed = 0xCA4D) : builder_(geometry(clusters)) {
        Bytes& image = builder_.raw();
        const Bytes noise = test::makePattern(static_cast<std::size_t>(clusters - 1) * kClusterSize, seed);
        const auto from = static_cast<std::ptrdiff_t>(builder_.clusterOffset(3));
        std::copy(noise.begin(), noise.end(), image.begin() + from);
    }

    [[nodiscard]] test::Fat32ImageBuilder& builder() noexcept { return builder_; }

    test::Fat32ImageBuilder::Entry add(std::string_view name, const Bytes& bytes,
                                       const std::vector<std::uint32_t>& clusters) {
        return builder_.addFileInClusters(test::Fat32ImageBuilder::root(), name, bytes, clusters);
    }
    // Written in these clusters, then deleted (Windows frees the chain).
    test::Fat32ImageBuilder::Entry addDeleted(std::string_view name, const Bytes& bytes,
                                              const std::vector<std::uint32_t>& clusters) {
        const test::Fat32ImageBuilder::Entry entry = add(name, bytes, clusters);
        builder_.deleteEntry(entry);
        return entry;
    }
    // Bytes left in free clusters by a file whose entry is gone.
    void plant(const std::vector<std::uint32_t>& clusters, const Bytes& bytes) {
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            const std::size_t begin = i * kClusterSize;
            const std::size_t count = std::min<std::size_t>(kClusterSize, bytes.size() - begin);
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(begin), count,
                        builder_.raw().begin() + static_cast<std::ptrdiff_t>(builder_.clusterOffset(clusters[i])));
        }
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

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

carving::FormatRegistry allFormats() {
    carving::FormatRegistry registry;
    EXPECT_TRUE(formats::registerImageFormats(registry).ok());
    EXPECT_TRUE(formats::registerAudioFormats(registry).ok());
    EXPECT_TRUE(formats::registerVideoFormats(registry).ok());
    return registry;
}

struct Volume {
    std::unique_ptr<FilesystemRecovery> recovery;
    CandidateScan scan;
};

Volume openVolume(storage::IStorageSource& volume, std::uint64_t volumeOffset = 0) {
    Volume opened;
    Result<std::unique_ptr<FilesystemRecovery>> recovery = openFilesystemRecovery(volume, volumeOffset);
    EXPECT_TRUE(recovery.ok()) << (recovery.ok() ? "" : describe(recovery.error()));
    if (!recovery.ok()) {
        return opened;
    }
    opened.recovery = std::move(recovery).value();
    Result<CandidateScan> scan = opened.recovery->findCandidates({}, {});
    EXPECT_TRUE(scan.ok()) << (scan.ok() ? "" : describe(scan.error()));
    if (scan.ok()) {
        opened.scan = std::move(scan).value();
    }
    return opened;
}

std::string describeFragments(const FragmentCandidate& candidate) {
    std::string text = std::string(toString(candidate.status)) + " " + std::string(toString(candidate.origin)) + " " +
                       candidate.formatId + ": " + candidate.reason + "\n";
    text += "  search: " + std::to_string(candidate.search.layoutsValidated) + " layouts, " +
            std::to_string(candidate.search.sampleProbes) + " probes" +
            (candidate.search.complete ? "" : ", stopped by " + candidate.search.limit) + "\n";
    for (std::size_t i = 0; i < candidate.hypotheses.size(); ++i) {
        const ReconstructionHypothesis& h = candidate.hypotheses[i];
        text += "  [" + std::to_string(i) + "] " + std::string(toString(h.source)) + " " +
                std::string(toString(h.data.method)) + " " + std::string(carving::toString(h.validation.status)) +
                " (" + std::to_string(h.validation.validBytes) + "): " + h.validation.detail + "\n";
        text += "      fragments " + std::to_string(h.evidence.fragments) + ", clusters " +
                std::to_string(h.evidence.clusters) + ", allocated " + std::to_string(h.evidence.allocatedClusters) +
                ", claimed " + std::to_string(h.evidence.claimedClusters) + ", placed " +
                std::to_string(h.evidence.placedBytes) + ", missing " + std::to_string(h.evidence.missingBytes) +
                ", unreadable " + std::to_string(h.evidence.unreadableBytes) +
                (h.evidence.dataChecked ? ", data checked" : ", data not checked") + "\n      runs:";
        for (const ClusterRun& clusters : h.clusters) {
            text += " " + std::to_string(clusters.firstCluster) + "+" + std::to_string(clusters.count);
        }
        text += "\n";
    }
    return text;
}

// What every candidate satisfies, whatever its evidence.
void expectConsistent(storage::IStorageSource& source, const carving::FormatRegistry& registry,
                      const FragmentCandidate& candidate) {
    SCOPED_TRACE(describeFragments(candidate));
    const carving::IFileFormat* format = registry.find(candidate.formatId);
    ASSERT_NE(format, nullptr);
    EXPECT_FALSE(candidate.reason.empty());
    EXPECT_LE(candidate.tied, candidate.hypotheses.size());
    switch (candidate.status) {
    case ReconstructionStatus::Ambiguous:
        EXPECT_GE(candidate.tied, 2u);
        EXPECT_EQ(candidate.reconstruction(), nullptr);
        break;
    case ReconstructionStatus::Unrecoverable:
        EXPECT_EQ(candidate.tied, 0u);
        EXPECT_EQ(candidate.reconstruction(), nullptr);
        break;
    default:
        EXPECT_EQ(candidate.tied, 0u);
        ASSERT_NE(candidate.reconstruction(), nullptr);
        EXPECT_EQ(candidate.reconstruction(), &candidate.hypotheses.front());
        break;
    }
    EXPECT_EQ(candidate.filesystemCandidate.has_value(), candidate.origin == SeedOrigin::Filesystem);
    EXPECT_EQ(candidate.recordedSize.has_value(), candidate.origin == SeedOrigin::Filesystem);
    if (candidate.origin == SeedOrigin::Carving) {
        EXPECT_TRUE(candidate.carve.has_value());
        EXPECT_EQ(candidate.name.rfind("recovered_", 0), 0u);
        EXPECT_TRUE(candidate.path.empty());
    }
    for (const ReconstructionHypothesis& hypothesis : candidate.hypotheses) {
        const RecoveryCandidate& data = hypothesis.data;
        RECOVERY_EXPECT_OK(validateCandidate(data));
        EXPECT_EQ(data.id, candidate.id);
        EXPECT_EQ(data.filename, candidate.name);
        EXPECT_EQ(data.filesystemEvidence.path, candidate.path);
        EXPECT_FALSE(data.fragmentation.known);
        EXPECT_EQ(data.fragmentation.fragmentCount, hypothesis.evidence.fragments);
        if (hypothesis.evidence.fragments > 1) {
            EXPECT_EQ(data.method, RecoveryMethod::Fragmented);
        } else {
            EXPECT_EQ(data.method, candidate.origin == SeedOrigin::Filesystem ? RecoveryMethod::Hybrid
                                                                               : RecoveryMethod::Carving);
        }
        EXPECT_EQ(data.hasWarning(CandidateWarning::DataMissing), data.bytes(RegionKind::Missing) > 0);
        EXPECT_EQ(data.hasWarning(CandidateWarning::ClustersReallocated), data.reallocatedBytes() > 0);
        EXPECT_EQ(hypothesis.evidence.placedBytes, data.bytes(RegionKind::Stored));
        EXPECT_EQ(hypothesis.evidence.missingBytes, data.bytes(RegionKind::Missing));
        std::uint64_t clusters = 0;
        for (const ClusterRun& run : hypothesis.clusters) {
            clusters += run.count;
        }
        EXPECT_EQ(clusters, hypothesis.evidence.clusters);
        EXPECT_EQ(data.bytes(RegionKind::Stored), std::min<std::uint64_t>(data.bytes(RegionKind::Stored),
                                                                          clusters * candidate.clusterSize));
        if (candidate.origin == SeedOrigin::Carving) {
            EXPECT_TRUE(data.filesystemEvidence.path.empty());
            EXPECT_EQ(data.filename.rfind("recovered_", 0), 0u);
        }
        // Independent validation: the format's validator on exactly the delivered bytes.
        Result<std::unique_ptr<CandidateContentReader>> reader = CandidateContentReader::open(source, data);
        ASSERT_TRUE(reader.ok());
        Result<carving::ValidationResult> verdict = format->validator().validate(**reader);
        ASSERT_TRUE(verdict.ok());
        EXPECT_EQ(verdict->status, hypothesis.validation.status);
        EXPECT_EQ(verdict->validBytes, hypothesis.validation.validBytes);
    }
    if (candidate.status == ReconstructionStatus::Complete) {
        const ReconstructionHypothesis& chosen = candidate.hypotheses.front();
        EXPECT_EQ(chosen.validation.status, ValidationStatus::Valid);
        EXPECT_EQ(chosen.evidence.allocatedClusters, 0u);
        EXPECT_EQ(chosen.evidence.missingBytes, 0u);
        EXPECT_EQ(chosen.evidence.unreadableBytes, 0u);
    }
    if (candidate.status == ReconstructionStatus::Partial) {
        const ReconstructionHypothesis& chosen = candidate.hypotheses.front();
        EXPECT_TRUE(chosen.evidence.missingBytes > 0 || chosen.validation.status != ValidationStatus::Valid);
    }
}

struct Recovered {
    FragmentRecoveryReport report;
    std::vector<FragmentCandidate> candidates;

    // The candidate of the deleted file named `name`, or nullptr.
    [[nodiscard]] const FragmentCandidate* named(std::string_view name) const {
        const FragmentCandidate* found = nullptr;
        for (const FragmentCandidate& candidate : candidates) {
            if (candidate.origin == SeedOrigin::Filesystem && candidate.name == name) {
                EXPECT_EQ(found, nullptr) << "two candidates named " << name;
                found = &candidate;
            }
        }
        return found;
    }
    // The candidate of the carve starting at `offset`, or nullptr.
    [[nodiscard]] const FragmentCandidate* carvedAt(std::uint64_t offset) const {
        for (const FragmentCandidate& candidate : candidates) {
            if (candidate.origin == SeedOrigin::Carving && candidate.carve.has_value() &&
                candidate.carve->sourceOffset == offset) {
                return &candidate;
            }
        }
        return nullptr;
    }
};

Recovered reconstructAll(storage::IStorageSource& source, const carving::FormatRegistry& registry,
                         const std::vector<Volume*>& volumes, FragmentRecoveryOptions options = {}) {
    Recovered recovered;
    FragmentRecovery recovery(source, registry, std::move(options));
    for (Volume* volume : volumes) {
        RECOVERY_EXPECT_OK(recovery.addVolume(*volume->recovery, volume->scan));
    }
    Result<FragmentRecoveryReport> report = recovery.run([&](FragmentCandidate&& candidate) -> Status {
        recovered.candidates.push_back(std::move(candidate));
        return success();
    });
    EXPECT_TRUE(report.ok()) << (report.ok() ? "" : describe(report.error()));
    if (report.ok()) {
        recovered.report = std::move(report).value();
        EXPECT_EQ(recovered.report.candidates(), recovered.candidates.size());
    }
    std::uint64_t id = recovery.options().firstId;
    for (const FragmentCandidate& candidate : recovered.candidates) {
        EXPECT_EQ(candidate.id.value(), id++);
        expectConsistent(source, registry, candidate);
    }
    return recovered;
}

// The reconstruction is `original`, every byte placed and read.
void expectReconstructed(storage::IStorageSource& source, const FragmentCandidate* candidate, const Bytes& original,
                         std::size_t fragments) {
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeFragments(*candidate));
    EXPECT_EQ(candidate->status, ReconstructionStatus::Complete);
    const ReconstructionHypothesis* chosen = candidate->reconstruction();
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->evidence.fragments, fragments);
    EXPECT_EQ(chosen->data.method, fragments > 1 ? RecoveryMethod::Fragmented
                                                 : (candidate->origin == SeedOrigin::Filesystem
                                                        ? RecoveryMethod::Hybrid
                                                        : RecoveryMethod::Carving));
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, chosen->data);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(rebuilt.report.allBytesRead());
    EXPECT_EQ(rebuilt.data.size(), original.size());
    EXPECT_TRUE(rebuilt.data == original);
}

// ===========================================================================
// Known fragmented files
// ===========================================================================

TEST(FragmentRecoveryTest, DeletedFragmentedFilesAreReconstructed) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // Around a file that still exists: its writer skipped the clusters in use.
    const Bytes aroundActive = photo(1);
    const std::uint32_t n1 = clustersFor(aroundActive.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 11), run(1010, 9));
    card.addDeleted("Beach photo 01.jpg", aroundActive, pieces(n1, {{1000, 10}, {1019, n1}}));
    // In three pieces around deleted files whose entries survive.
    const Bytes aroundDeleted = photo(2);
    const std::uint32_t n2 = clustersFor(aroundDeleted.size());
    card.addDeleted("OLD1.BIN", test::makePattern(6 * kClusterSize, 21), run(2008, 6));
    card.addDeleted("OLD2.BIN", test::makePattern(5 * kClusterSize, 22), run(2022, 5));
    card.addDeleted("Beach photo 02.jpg", aroundDeleted, pieces(n2, {{2000, 8}, {2014, 8}, {2027, n2}}));
    // Across old data no evidence explains: found where the structure breaks.
    test::PngOptions png;
    png.width = 64;
    png.height = 48;
    png.idatSize = 1024;
    const Bytes image = test::makePng(png);
    const std::uint32_t n3 = clustersFor(image.size());
    card.addDeleted("Scanned page.png", image, pieces(n3, {{3000, 7}, {3020, n3}}));

    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    expectReconstructed(source, recovered.named("Beach photo 01.jpg"), aroundActive, 2);
    expectReconstructed(source, recovered.named("Beach photo 02.jpg"), aroundDeleted, 3);
    expectReconstructed(source, recovered.named("Scanned page.png"), image, 2);
    ASSERT_NE(recovered.named("Beach photo 01.jpg"), nullptr);
    EXPECT_EQ(recovered.named("Beach photo 01.jpg")->hypotheses.front().source, LayoutSource::SkipAllocated);
    for (const char* name : {"Beach photo 01.jpg", "Beach photo 02.jpg", "Scanned page.png"}) {
        const FragmentCandidate* candidate = recovered.named(name);
        ASSERT_NE(candidate, nullptr);
        ASSERT_NE(candidate->reconstruction(), nullptr);
        EXPECT_TRUE(candidate->reconstruction()->evidence.dataChecked) << name;
        // The metadata's name, path and evidence stay with the reconstruction.
        EXPECT_EQ(candidate->reconstruction()->data.filesystemEvidence.path, std::string("/") + name);
        EXPECT_TRUE(candidate->reconstruction()->data.hasWarning(CandidateWarning::LayoutGuessed));
        // Carving found the file's start too (and broke where the first piece ends): evidence, not a seed.
        ASSERT_TRUE(candidate->carve.has_value());
        EXPECT_EQ(candidate->carve->sourceOffset, candidate->reconstruction()->data.sourceOffset());
        EXPECT_NE(candidate->carve->end.status, carving::EndStatus::Found);
    }
    EXPECT_EQ(recovered.report.filesystemSeeds, 3u);
    EXPECT_EQ(recovered.report.filesystemSkipped, 2u);  // OLD1.BIN and OLD2.BIN: no format knows them
    EXPECT_EQ(recovered.report.complete, 3u);
    EXPECT_EQ(recovered.report.searchesLimited, 0u);
    EXPECT_EQ(recovered.report.carvingSeeds, 0u);
}

TEST(FragmentRecoveryTest, AContiguousDeletedFileIsConfirmedInOnePiece) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const Bytes jpeg = photo(3);
    card.addDeleted("Holiday 01.jpg", jpeg, run(1500, clustersFor(jpeg.size())));
    test::BmpOptions bmp;
    bmp.width = 64;
    bmp.height = 48;
    const Bytes bitmap = test::makeBmp(bmp);
    card.addDeleted("Drawing.bmp", bitmap, run(1600, clustersFor(bitmap.size())));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});
    ASSERT_EQ(recovered.candidates.size(), 2u);

    for (const auto& [name, bytes] : {std::pair<const char*, const Bytes*>{"Holiday 01.jpg", &jpeg},
                                      std::pair<const char*, const Bytes*>{"Drawing.bmp", &bitmap}}) {
        SCOPED_TRACE(name);
        const FragmentCandidate* candidate = recovered.named(name);
        expectReconstructed(source, candidate, *bytes, 1);
        ASSERT_NE(candidate, nullptr);
        // The guess the metadata allowed, which the structure confirms: HYBRID, as in P12.
        EXPECT_EQ(candidate->reconstruction()->source, LayoutSource::Contiguous);
        EXPECT_EQ(candidate->reconstruction()->data.method, RecoveryMethod::Hybrid);
        EXPECT_EQ(candidate->recordedSize, bytes->size());
        EXPECT_EQ(candidate->search.layoutsValidated, 1u);
    }
    // The structure of a JPEG checks its data; a BMP's pixels are not checked at all.
    EXPECT_TRUE(recovered.named("Holiday 01.jpg")->reconstruction()->evidence.dataChecked);
    EXPECT_FALSE(recovered.named("Drawing.bmp")->reconstruction()->evidence.dataChecked);
}

TEST(FragmentRecoveryTest, AMissingFragmentLeavesAPartialFile) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // The second piece was overwritten by later data that was deleted in turn: nothing of it is left.
    const Bytes jpeg = photo(4);
    const std::uint32_t n = clustersFor(jpeg.size());
    card.addDeleted("Lost half.jpg", jpeg, pieces(n, {{1000, 12}, {1040, n}}));
    card.plant(run(1040, n - 12), test::makePattern((n - 12) * kClusterSize, 41));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* candidate = recovered.named("Lost half.jpg");
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeFragments(*candidate));
    EXPECT_EQ(candidate->status, ReconstructionStatus::Partial);
    const ReconstructionHypothesis* partial = candidate->reconstruction();
    ASSERT_NE(partial, nullptr);
    // What validates of the file: its first piece, then nothing (Missing, cut at the end when written).
    EXPECT_EQ(partial->validation.status, ValidationStatus::Truncated);
    EXPECT_GE(partial->evidence.placedBytes, 12u * kClusterSize);
    EXPECT_LT(partial->evidence.placedBytes, 13u * kClusterSize);
    EXPECT_EQ(partial->evidence.placedBytes + partial->evidence.missingBytes, jpeg.size());
    EXPECT_TRUE(partial->data.hasWarning(CandidateWarning::DataMissing));
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, partial->data);
    ASSERT_TRUE(rebuilt.ok);
    ASSERT_EQ(rebuilt.data.size(), partial->evidence.placedBytes);
    EXPECT_TRUE(std::equal(jpeg.begin(), jpeg.begin() + 12 * kClusterSize, rebuilt.data.begin()));
}

TEST(FragmentRecoveryTest, FragmentsOutOfOrderAreFound) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // The writer wrapped around: the second piece lies before the first on the disk, after a file
    // that still exists.
    const Bytes jpeg = photo(5);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)card.add("EARLIER.BIN", test::makePattern(10 * kClusterSize, 51), run(790, 10));
    card.addDeleted("Wrapped.jpg", jpeg, pieces(n, {{3000, 12}, {800, n}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});
    const FragmentCandidate* candidate = recovered.named("Wrapped.jpg");
    expectReconstructed(source, candidate, jpeg, 2);
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->reconstruction()->clusters,
              (std::vector<ClusterRun>{ClusterRun{3000, 12}, ClusterRun{800, n - 12}}));

    // The same pieces in disk order are not the file: the validator rejects the layout.
    RecoveryCandidate swapped = candidate->reconstruction()->data;
    swapped.sourceRegions = {
        SourceRegion{0, (n - 12) * kClusterSize, RegionKind::Stored, card.offsetOf(800), false},
        SourceRegion{(n - 12) * kClusterSize, jpeg.size() - (n - 12) * kClusterSize, RegionKind::Stored,
                     card.offsetOf(3000), false}};
    RECOVERY_ASSERT_OK(validateCandidate(swapped));
    Result<std::unique_ptr<CandidateContentReader>> reader = CandidateContentReader::open(source, swapped);
    RECOVERY_ASSERT_OK(reader);
    Result<carving::ValidationResult> verdict = registry.find("jpeg")->validator().validate(**reader);
    RECOVERY_ASSERT_OK(verdict);
    EXPECT_EQ(verdict->status, ValidationStatus::Invalid);
}

TEST(FragmentRecoveryTest, OverwrittenFragmentsAreReportedForWhatTheyCost) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // A file written in one piece, three clusters of which a new file took over.
    const Bytes jpeg = photo(6, 4);
    const std::uint32_t n = clustersFor(jpeg.size());
    card.addDeleted("Overwritten.jpg", jpeg, run(4000, n));
    (void)card.add("NEW.BIN", test::makePattern(3 * kClusterSize, 61), run(4010, 3));
    // A file whose first cluster a new file took over.
    const Bytes other = photo(7);
    card.addDeleted("Start lost.jpg", other, run(4200, clustersFor(other.size())));
    (void)card.add("NEWER.BIN", test::makePattern(5 * kClusterSize, 62), run(4200, 5));
    // A file whose first cluster holds other data, though free.
    const Bytes wiped = photo(8);
    card.addDeleted("Wiped start.jpg", wiped, run(4400, clustersFor(wiped.size())));
    card.plant({4400}, test::makePattern(kClusterSize, 63));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    // Nothing continues the file past the overwritten clusters (skipping them misaligns it): what
    // validates is the part before them.
    const FragmentCandidate* overwritten = recovered.named("Overwritten.jpg");
    ASSERT_NE(overwritten, nullptr);
    SCOPED_TRACE(describeFragments(*overwritten));
    EXPECT_EQ(overwritten->status, ReconstructionStatus::Partial);
    ASSERT_NE(overwritten->reconstruction(), nullptr);
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, overwritten->reconstruction()->data);
    ASSERT_TRUE(rebuilt.ok);
    ASSERT_GE(rebuilt.data.size(), 10u * kClusterSize);
    EXPECT_TRUE(std::equal(jpeg.begin(), jpeg.begin() + 10 * kClusterSize, rebuilt.data.begin()));
    // The layout through the new file's clusters was tried, and its data there is the new file's.
    const bool triedThrough = std::any_of(overwritten->hypotheses.begin(), overwritten->hypotheses.end(),
                                          [](const ReconstructionHypothesis& h) {
                                              return h.source == LayoutSource::Contiguous;
                                          });
    EXPECT_TRUE(triedThrough);

    const FragmentCandidate* startLost = recovered.named("Start lost.jpg");
    ASSERT_NE(startLost, nullptr);
    EXPECT_EQ(startLost->status, ReconstructionStatus::Unrecoverable);
    EXPECT_NE(startLost->reason.find("allocated to other data"), std::string::npos) << startLost->reason;
    EXPECT_TRUE(startLost->hypotheses.empty());

    const FragmentCandidate* wipedStart = recovered.named("Wiped start.jpg");
    ASSERT_NE(wipedStart, nullptr);
    EXPECT_EQ(wipedStart->status, ReconstructionStatus::Unrecoverable);
    EXPECT_EQ(wipedStart->formatId, "jpeg");  // by its name
    EXPECT_NE(wipedStart->reason.find("does not start"), std::string::npos) << wipedStart->reason;
}

TEST(FragmentRecoveryTest, AmbiguousLayoutsAreReportedWithEveryAlternative) {
    const carving::FormatRegistry registry = allFormats();
    const Bytes jpeg = photo(9);
    const std::uint32_t n = clustersFor(jpeg.size());
    const Bytes tail(jpeg.begin() + 12 * kClusterSize, jpeg.end());
    // Another tail that validates just as well: entropy-coded data that differs in one byte.
    Bytes other = tail;
    std::size_t changed = 100;
    while (other[changed] == std::byte{0xFF} || other[changed - 1] == std::byte{0xFF} ||
           other[changed + 1] == std::byte{0xFF} || other[changed] == std::byte{0xFE}) {
        ++changed;
    }
    other[changed] = other[changed] ^ std::byte{0x01};

    for (const bool duplicate : {false, true}) {
        SCOPED_TRACE(duplicate ? "an identical copy" : "a different tail");
        Card card;
        card.addDeleted("Twin.jpg", jpeg, pieces(n, {{5000, 12}, {5030, n}}));
        card.plant(run(5060, n - 12), duplicate ? tail : other);
        test::MemoryStorageSource source(card.image());
        RECOVERY_ASSERT_OK(source.open());
        Volume volume = openVolume(source);
        ASSERT_NE(volume.recovery, nullptr);
        const Recovered recovered = reconstructAll(source, registry, {&volume});
        const FragmentCandidate* candidate = recovered.named("Twin.jpg");
        ASSERT_NE(candidate, nullptr);
        SCOPED_TRACE(describeFragments(*candidate));
        if (duplicate) {
            // The same bytes either way: nothing to choose between.
            expectReconstructed(source, candidate, jpeg, 2);
            continue;
        }
        EXPECT_EQ(candidate->status, ReconstructionStatus::Ambiguous);
        ASSERT_EQ(candidate->tied, 2u);
        EXPECT_EQ(candidate->reconstruction(), nullptr);
        std::vector<Bytes> alternatives;
        for (std::size_t i = 0; i < candidate->tied; ++i) {
            const ReconstructionHypothesis& hypothesis = candidate->hypotheses[i];
            EXPECT_EQ(hypothesis.validation.status, ValidationStatus::Valid);
            EXPECT_EQ(hypothesis.evidence.fragments, 2u);
            const test::Reconstructed rebuilt = test::reconstructToMemory(source, hypothesis.data);
            ASSERT_TRUE(rebuilt.ok);
            alternatives.push_back(rebuilt.data);
        }
        Bytes expectedOther(jpeg.begin(), jpeg.begin() + 12 * kClusterSize);
        expectedOther.insert(expectedOther.end(), other.begin(), other.end());
        EXPECT_TRUE(std::find(alternatives.begin(), alternatives.end(), jpeg) != alternatives.end());
        EXPECT_TRUE(std::find(alternatives.begin(), alternatives.end(), expectedOther) != alternatives.end());
        EXPECT_EQ(recovered.report.ambiguous, 1u);
    }
}

TEST(FragmentRecoveryTest, AllocationEvidenceDecidesWhereTheStructureCannot) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // A bitmap written around a file that still exists. Its pixels have no structure: read through
    // that file's clusters it validates as well, but those clusters are that file's now.
    test::BmpOptions bmp;
    bmp.width = 64;
    bmp.height = 48;
    const Bytes bitmap = test::makeBmp(bmp);
    const std::uint32_t n = clustersFor(bitmap.size());
    (void)card.add("KEEP.BIN", test::makePattern(5 * kClusterSize, 71), run(6009, 5));
    card.addDeleted("Sketch.bmp", bitmap, pieces(n, {{6000, 9}, {6014, n}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* candidate = recovered.named("Sketch.bmp");
    expectReconstructed(source, candidate, bitmap, 2);
    ASSERT_NE(candidate, nullptr);
    const ReconstructionHypothesis& chosen = *candidate->reconstruction();
    EXPECT_EQ(chosen.source, LayoutSource::SkipAllocated);
    EXPECT_FALSE(chosen.evidence.dataChecked);
    // The alternative through the other file's clusters validates too, and lost on the allocation.
    ASSERT_GE(candidate->hypotheses.size(), 2u);
    const ReconstructionHypothesis& through = candidate->hypotheses[1];
    EXPECT_EQ(through.source, LayoutSource::Contiguous);
    EXPECT_EQ(through.validation.status, ValidationStatus::Valid);
    EXPECT_EQ(through.evidence.allocatedClusters, 5u);
    EXPECT_TRUE(through.data.hasWarning(CandidateWarning::ClustersReallocated));
}

TEST(FragmentRecoveryTest, CarvedFilesWithoutMetadataAreReconstructed) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // A photo whose directory entry is gone, written around a file that still exists.
    const Bytes jpeg = photo(10);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)card.add("KEEP.BIN", test::makePattern(8 * kClusterSize, 81), run(7012, 8));
    card.plant(pieces(n, {{7000, 12}, {7020, n}}), jpeg);
    // An image across old data.
    test::PngOptions options;
    options.width = 64;
    options.height = 48;
    options.idatSize = 1024;
    const Bytes png = test::makePng(options);
    const std::uint32_t m = clustersFor(png.size());
    card.plant(pieces(m, {{7100, 9}, {7121, m}}), png);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* photoCarve = recovered.carvedAt(card.offsetOf(7000));
    expectReconstructed(source, photoCarve, jpeg, 2);
    ASSERT_NE(photoCarve, nullptr);
    EXPECT_EQ(photoCarve->formatId, "jpeg");
    EXPECT_EQ(photoCarve->carve->end.status, carving::EndStatus::Broken);
    const std::string number = std::to_string(photoCarve->id.value());
    EXPECT_EQ(photoCarve->reconstruction()->data.filename,
              "recovered_" + std::string(6 - number.size(), '0') + number + ".jpg");
    const FragmentCandidate* pngCarve = recovered.carvedAt(card.offsetOf(7100));
    expectReconstructed(source, pngCarve, png, 2);
    EXPECT_EQ(recovered.report.carvingSeeds, 2u);
    EXPECT_EQ(recovered.report.filesystemSeeds, 0u);
}

TEST(FragmentRecoveryTest, FilesOfNoKnownFormatAreLeftAlone) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    card.addDeleted("notes.txt", test::makePattern(3 * kClusterSize, 91), run(1200, 3));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});
    EXPECT_TRUE(recovered.candidates.empty());
    EXPECT_EQ(recovered.report.filesystemSkipped, 1u);
    EXPECT_EQ(recovered.report.filesystemSeeds, 0u);
}

TEST(FragmentRecoveryTest, SearchLimitsAreReported) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    test::PngOptions options;
    options.width = 64;
    options.height = 48;
    options.idatSize = 1024;
    const Bytes png = test::makePng(options);
    const std::uint32_t n = clustersFor(png.size());
    card.addDeleted("Far away.png", png, pieces(n, {{3000, 7}, {3020, n}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);

    FragmentRecoveryOptions few;
    few.limits.maxContinuations = 4;
    const Recovered cut = reconstructAll(source, registry, {&volume}, few);
    const FragmentCandidate* candidate = cut.named("Far away.png");
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->status, ReconstructionStatus::Partial) << describeFragments(*candidate);
    EXPECT_FALSE(candidate->search.complete);
    EXPECT_EQ(candidate->search.limit, "maxContinuations");
    EXPECT_EQ(cut.report.searchesLimited, 1u);

    FragmentRecoveryOptions once;
    once.limits.maxValidations = 1;
    const Recovered stopped = reconstructAll(source, registry, {&volume}, once);
    ASSERT_NE(stopped.named("Far away.png"), nullptr);
    EXPECT_EQ(stopped.named("Far away.png")->search.limit, "maxValidations");
    EXPECT_EQ(stopped.named("Far away.png")->search.layoutsValidated, 1u);
}

// ===========================================================================
// MP4: the sample tables
// ===========================================================================

// A clip of 60 video and 60 audio samples (about 90 clusters), moov after the media data unless `first`.
test::Mp4Built clip(std::uint64_t seed, bool first = false) {
    test::Mp4Options options;
    options.seed = seed;
    options.moov = first ? test::Mp4MoovPlace::First : test::Mp4MoovPlace::Last;
    test::Mp4TrackOptions video;
    video.samples = 60;
    video.samplesPerChunk = 5;
    test::Mp4TrackOptions audio;
    audio.kind = test::Mp4TrackKind::Audio;
    audio.samples = 60;
    audio.samplesPerChunk = 10;
    options.tracks = {video, audio};
    return test::makeMp4(options);
}

// The samples of `built`, over every track, with a byte in file range [begin, end).
std::uint64_t samplesTouching(const test::Mp4Built& built, std::uint64_t begin, std::uint64_t end) {
    std::uint64_t count = 0;
    for (const std::vector<test::Mp4Sample>& track : built.samples) {
        for (const test::Mp4Sample& sample : track) {
            count += sample.offset < end && sample.offset + sample.size > begin ? 1 : 0;
        }
    }
    return count;
}

// The file clusters where a NAL unit of a video sample starts: the clusters whose place the samples check.
std::set<std::uint64_t> checkedClusters(const test::Mp4Built& built) {
    std::set<std::uint64_t> clusters;
    for (const test::Mp4Sample& sample : built.samples.front()) {
        std::uint64_t position = sample.offset;
        const std::uint64_t end = sample.offset + sample.size;
        while (position + 5 <= end) {
            clusters.insert(position / kClusterSize);
            position += 4 + loadBe32(built.bytes, static_cast<std::size_t>(position));
        }
    }
    return clusters;
}

// The number of clusters nearest `wanted` after which the samples tell exactly where a piece ends: the
// clusters on both sides of the boundary are checked.
std::uint32_t checkedSplit(const test::Mp4Built& built, std::uint32_t wanted) {
    const std::set<std::uint64_t> checked = checkedClusters(built);
    for (std::uint32_t distance = 0; distance < wanted; ++distance) {
        for (const std::uint32_t k : {wanted - distance, wanted + distance}) {
            if (checked.contains(k - 1) && checked.contains(k)) {
                return k;
            }
        }
    }
    ADD_FAILURE() << "no checked boundary near " << wanted;
    return wanted;
}

TEST(FragmentRecoveryTest, Mp4IsPlacedSampleBySampleWithItsMoovWhereTheBoxesPutIt) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // moov after the media data: in the last piece, found where the first mdat's size puts it. The pieces
    // end where the samples can tell (not inside an audio chunk: see AudioInMp4...).
    const test::Mp4Built two = clip(101);
    const std::uint32_t n2 = clustersFor(two.bytes.size());
    const std::uint32_t split2 = checkedSplit(two, 40);
    card.addDeleted("Clip 0001.mp4", two.bytes, pieces(n2, {{1000, split2}, {1100, n2}}));
    const test::Mp4Built three = clip(102);
    const std::uint32_t n3 = clustersFor(three.bytes.size());
    const std::uint32_t first3 = checkedSplit(three, 30);
    const std::uint32_t second3 = checkedSplit(three, 60) - first3;
    card.addDeleted("Clip 0002.mp4", three.bytes, pieces(n3, {{2000, first3}, {2060, second3}, {2150, n3}}));
    // moov before the media data: in the first piece.
    const test::Mp4Built first = clip(103, true);
    const std::uint32_t n4 = clustersFor(first.bytes.size());
    const std::uint32_t split4 = checkedSplit(first, 45);
    card.addDeleted("Clip 0003.mp4", first.bytes, pieces(n4, {{3000, split4}, {3080, n4}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const std::vector<std::tuple<const char*, const test::Mp4Built*, std::size_t>> files = {
        {"Clip 0001.mp4", &two, 2}, {"Clip 0002.mp4", &three, 3}, {"Clip 0003.mp4", &first, 2}};
    for (const auto& [name, built, fragments] : files) {
        SCOPED_TRACE(name);
        const FragmentCandidate* candidate = recovered.named(name);
        expectReconstructed(source, candidate, built->bytes, fragments);
        ASSERT_NE(candidate, nullptr);
        ASSERT_NE(candidate->reconstruction(), nullptr);
        const ReconstructionHypothesis& chosen = *candidate->reconstruction();
        EXPECT_EQ(chosen.source, LayoutSource::SampleTables);
        EXPECT_TRUE(chosen.evidence.dataChecked);
        EXPECT_GT(candidate->search.sampleProbes, 0u);
        // P12's structure evidence: every sample there and framed.
        ASSERT_TRUE(chosen.mp4.has_value());
        EXPECT_TRUE(chosen.mp4->intact());
        EXPECT_EQ(chosen.mp4->samples(), 120u);
        EXPECT_EQ(chosen.mp4->moovBeforeMediaData, built == &first);
    }
    EXPECT_EQ(recovered.report.complete, 3u);
}

TEST(FragmentRecoveryTest, Mp4WithAMissingPieceIsPartialWithTheRestPlaced) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const test::Mp4Built built = clip(111);
    const std::uint32_t n = clustersFor(built.bytes.size());
    card.addDeleted("Clip 0011.mp4", built.bytes, pieces(n, {{1000, 30}, {1060, 30}, {1150, n}}));
    card.plant(run(1060, 30), test::makePattern(30 * kClusterSize, 112));  // the middle piece is gone
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* candidate = recovered.named("Clip 0011.mp4");
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeFragments(*candidate));
    EXPECT_EQ(candidate->status, ReconstructionStatus::Partial);
    const ReconstructionHypothesis* partial = candidate->reconstruction();
    ASSERT_NE(partial, nullptr);
    EXPECT_EQ(partial->source, LayoutSource::SampleTables);
    // The first and last pieces placed where they are; the middle Missing (zeros when written).
    EXPECT_EQ(partial->data.expectedSize, built.bytes.size());
    ASSERT_TRUE(partial->mp4.has_value());
    const Mp4Structure& structure = *partial->mp4;
    EXPECT_TRUE(structure.moovOffset.has_value());
    EXPECT_GT(structure.samplesIntact(), 0u);
    EXPECT_GE(structure.samplesDamaged(), samplesTouching(built, 30 * kClusterSize, 60 * kClusterSize));
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, partial->data);
    ASSERT_TRUE(rebuilt.ok);
    ASSERT_EQ(rebuilt.data.size(), built.bytes.size());
    EXPECT_TRUE(std::equal(built.bytes.begin(), built.bytes.begin() + 29 * kClusterSize, rebuilt.data.begin()));
    const auto lastPiece = static_cast<std::ptrdiff_t>((n - 61) * kClusterSize);
    EXPECT_TRUE(std::equal(built.bytes.end() - lastPiece, built.bytes.end(), rebuilt.data.end() - lastPiece));
}

TEST(FragmentRecoveryTest, Mp4OverwrittenWhereItLiesIsCorrupted) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const test::Mp4Built built = clip(121);
    const std::uint32_t n = clustersFor(built.bytes.size());
    card.addDeleted("Clip 0021.mp4", built.bytes, run(4000, n));
    (void)card.add("NEW.BIN", test::makePattern(3 * kClusterSize, 122), run(4030, 3));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* candidate = recovered.named("Clip 0021.mp4");
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeFragments(*candidate));
    EXPECT_EQ(candidate->status, ReconstructionStatus::Corrupted);
    const ReconstructionHypothesis* chosen = candidate->reconstruction();
    ASSERT_NE(chosen, nullptr);
    // The whole file where it lay, three of its clusters another file's now.
    EXPECT_EQ(chosen->clusters, (std::vector<ClusterRun>{ClusterRun{4000, n}}));
    EXPECT_EQ(chosen->evidence.allocatedClusters, 3u);
    EXPECT_TRUE(chosen->data.hasWarning(CandidateWarning::ClustersReallocated));
    ASSERT_TRUE(chosen->mp4.has_value());
    EXPECT_EQ(chosen->mp4->samplesDamaged(), samplesTouching(built, 30 * kClusterSize, 33 * kClusterSize));
    EXPECT_EQ(chosen->mp4->samplesIntact() + chosen->mp4->samplesDamaged(), 120u);
}

TEST(FragmentRecoveryTest, AudioInMp4IsPlacedByTheAllocationOrReportedAmbiguous) {
    const carving::FormatRegistry registry = allFormats();
    test::M4aOptions options;
    options.audio.frames = 200;
    const Bytes song = test::makeM4a(options);
    const std::uint32_t n = clustersFor(song.size());
    Card card;
    // Around a file that still exists: the allocation says where the second piece starts.
    (void)card.add("KEEP.BIN", test::makePattern(10 * kClusterSize, 131), run(1040, 10));
    card.addDeleted("Song 01.m4a", song, pieces(n, {{1000, 40}, {1050, n}}));
    // Across old data: audio samples have no structure to check, so the samples cannot tell where
    // the moov's piece starts, and nothing else does either.
    card.addDeleted("Song 02.m4a", song, pieces(n, {{2000, 40}, {2070, n}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* decided = recovered.named("Song 01.m4a");
    expectReconstructed(source, decided, song, 2);
    ASSERT_NE(decided, nullptr);
    EXPECT_EQ(decided->formatId, "m4a");
    EXPECT_FALSE(decided->reconstruction()->evidence.dataChecked);

    const FragmentCandidate* open = recovered.named("Song 02.m4a");
    ASSERT_NE(open, nullptr);
    SCOPED_TRACE(describeFragments(*open));
    EXPECT_EQ(open->status, ReconstructionStatus::Ambiguous);
    // Every cluster boundary between the last one the first piece surely holds and the moov: more than
    // are delivered.
    EXPECT_EQ(open->tied, FragmentSearchLimits{}.maxAlternatives);
    EXPECT_NE(open->reason.find("64 layouts"), std::string::npos) << open->reason;
    bool restsOnAllocation = false;
    for (std::size_t i = 0; i < open->tied; ++i) {
        EXPECT_EQ(open->hypotheses[i].validation.status, ValidationStatus::Valid);
        restsOnAllocation = restsOnAllocation || !open->hypotheses[i].evidence.dataChecked;
    }
    EXPECT_TRUE(restsOnAllocation);
}

TEST(FragmentRecoveryTest, CarvedMp4WithoutMetadataIsReconstructed) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const test::Mp4Built built = clip(141);
    const std::uint32_t n = clustersFor(built.bytes.size());
    card.plant(pieces(n, {{5000, 35}, {5090, n}}), built.bytes);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* candidate = recovered.carvedAt(card.offsetOf(5000));
    expectReconstructed(source, candidate, built.bytes, 2);
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->formatId, "mp4");
    EXPECT_EQ(candidate->reconstruction()->source, LayoutSource::SampleTables);
    EXPECT_EQ(candidate->name.substr(candidate->name.size() - 4), ".mp4");
}

// ===========================================================================
// Audio streams, exFAT, output, damage, partitions
// ===========================================================================

TEST(FragmentRecoveryTest, AudioStreamsAreReconstructed) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    // Frames chain to the end of the stream: across old data, the frames say where the next piece is.
    test::Mp3Options mp3;
    mp3.frames = 30;
    const Bytes song = test::makeMp3(mp3);
    const std::uint32_t n = clustersFor(song.size());
    card.addDeleted("Song.mp3", song, pieces(n, {{1000, 9}, {1030, n}}));
    // PCM samples have no structure: around a file that still exists, the allocation decides.
    test::WavOptions wav;
    wav.frames = 3000;
    const Bytes voice = test::makeWav(wav);
    const std::uint32_t m = clustersFor(voice.size());
    (void)card.add("KEEP.BIN", test::makePattern(6 * kClusterSize, 151), run(2010, 6));
    card.addDeleted("Voice memo.wav", voice, pieces(m, {{2000, 10}, {2016, m}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    expectReconstructed(source, recovered.named("Song.mp3"), song, 2);
    expectReconstructed(source, recovered.named("Voice memo.wav"), voice, 2);
    ASSERT_NE(recovered.named("Song.mp3"), nullptr);
    EXPECT_TRUE(recovered.named("Song.mp3")->reconstruction()->evidence.dataChecked);
    ASSERT_NE(recovered.named("Voice memo.wav"), nullptr);
    EXPECT_FALSE(recovered.named("Voice memo.wav")->reconstruction()->evidence.dataChecked);
}

TEST(FragmentRecoveryTest, ExFatFilesWhoseChainWasClearedAreReconstructed) {
    const carving::FormatRegistry registry = allFormats();
    test::ExFatBuilderOptions options;
    options.clearFatOnDelete = true;
    test::ExFatImageBuilder builder(options);
    // Old data in every cluster after the system ones.
    const std::uint32_t firstFree = builder.root() + 1;
    const auto freeClusters = static_cast<std::size_t>(builder.lastCluster() + 1 - firstFree);
    const Bytes noise = test::makePattern(freeClusters * kClusterSize, 0xE7);
    std::copy(noise.begin(), noise.end(),
              builder.raw().begin() + static_cast<std::ptrdiff_t>(builder.clusterOffset(firstFree)));
    const Bytes jpeg = photo(161);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)builder.addFileInClusters(builder.root(), "KEEP.BIN", test::makePattern(7 * kClusterSize, 162), run(1011, 7));
    builder.deleteEntry(builder.addFileInClusters(builder.root(), "Cleared chain.jpg", jpeg,
                                                  pieces(n, {{1000, 11}, {1018, n}})));
    // A file stored in one run (NoFatChain) keeps its layout when deleted: nothing to reconstruct.
    builder.deleteEntry(builder.addFile(builder.root(), "One run.jpg", photo(163)));
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    ASSERT_EQ(volume.scan.filesystemInfo.type, filesystem::FilesystemType::ExFat);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    ASSERT_EQ(recovered.candidates.size(), 1u);
    const FragmentCandidate* candidate = recovered.named("Cleared chain.jpg");
    expectReconstructed(source, candidate, jpeg, 2);
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->filesystem, filesystem::FilesystemType::ExFat);
    EXPECT_EQ(candidate->reconstruction()->source, LayoutSource::SkipAllocated);
}

TEST(FragmentRecoveryTest, ReconstructionsAreWrittenAndValidateAgain) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const Bytes fragmented = photo(171);
    const std::uint32_t n = clustersFor(fragmented.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 172), run(1010, 9));
    card.addDeleted("Fragmented.jpg", fragmented, pieces(n, {{1000, 10}, {1019, n}}));
    const Bytes whole = photo(173);
    card.addDeleted("Whole.jpg", whole, run(2000, clustersFor(whole.size())));
    const Bytes carved = photo(174);
    const std::uint32_t c = clustersFor(carved.size());
    (void)card.add("KEEP2.BIN", test::makePattern(5 * kClusterSize, 175), run(3012, 5));
    card.plant(pieces(c, {{3000, 12}, {3017, c}}), carved);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});
    ASSERT_EQ(recovered.candidates.size(), 3u);

    const test::TempDir directory;
    Result<RecoveryWriter> writer = RecoveryWriter::create(source, directory / "out");
    RECOVERY_ASSERT_OK(writer);
    std::vector<std::pair<std::filesystem::path, const Bytes*>> expected;
    for (const FragmentCandidate& candidate : recovered.candidates) {
        ASSERT_EQ(candidate.status, ReconstructionStatus::Complete) << describeFragments(candidate);
        const Result<RecoveredFile> file = writer->recover(candidate.reconstruction()->data);
        RECOVERY_ASSERT_OK(file);
        EXPECT_TRUE(file->report.allBytesRead());
        const Bytes* original = candidate.name == "Fragmented.jpg" ? &fragmented
                                : candidate.name == "Whole.jpg"    ? &whole
                                                                   : &carved;
        expected.emplace_back(file->path, original);
    }
    EXPECT_EQ(recovered.candidates[0].reconstruction()->data.method, RecoveryMethod::Fragmented);
    EXPECT_EQ(recovered.candidates[1].reconstruction()->data.method, RecoveryMethod::Hybrid);
    EXPECT_EQ(recovered.candidates[2].reconstruction()->data.method, RecoveryMethod::Fragmented);
    for (const auto& [path, original] : expected) {
        SCOPED_TRACE(path.string());
        const Bytes bytes = test::readFile(path);
        EXPECT_TRUE(bytes == *original);
        // The written file, on its own, validates.
        carving::MemoryContentReader content(bytes);
        const Result<carving::ValidationResult> verdict = registry.find("jpeg")->validator().validate(content);
        RECOVERY_ASSERT_OK(verdict);
        EXPECT_EQ(verdict->status, ValidationStatus::Valid);
    }
}

TEST(FragmentRecoveryTest, UnreadableSectorsMakeAPlacedFileCorrupted) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const Bytes jpeg = photo(181);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 182), run(1010, 9));
    card.addDeleted("Bad sector.jpg", jpeg, pieces(n, {{1000, 10}, {1019, n}}));
    test::MemoryStorageSource source(card.image());
    source.addBadSector(card.offsetOf(1022) / 512);
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(source, registry, {&volume});

    const FragmentCandidate* candidate = recovered.named("Bad sector.jpg");
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeFragments(*candidate));
    // Zeros in entropy-coded data do not break its structure: the layout is found, one sector of it unread.
    EXPECT_EQ(candidate->status, ReconstructionStatus::Corrupted);
    ASSERT_NE(candidate->reconstruction(), nullptr);
    EXPECT_EQ(candidate->reconstruction()->evidence.unreadableBytes, 512u);
    EXPECT_EQ(candidate->reconstruction()->evidence.fragments, 2u);
}

TEST(FragmentRecoveryTest, VolumesOfAPartitionedDiskUseDiskOffsets) {
    const carving::FormatRegistry registry = allFormats();
    Card card(4096);
    const Bytes jpeg = photo(191);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 192), run(1010, 9));
    card.addDeleted("Partitioned.jpg", jpeg, pieces(n, {{1000, 10}, {1019, n}}));
    const Bytes volumeImage = card.image();

    constexpr std::uint32_t kFirst = 4096;
    const auto sectors = static_cast<std::uint32_t>(volumeImage.size() / 512);
    Bytes disk((static_cast<std::size_t>(kFirst) + sectors + 64) * 512);
    std::copy(volumeImage.begin(), volumeImage.end(), disk.begin() + std::size_t{kFirst} * 512);
    test::writeMbrSector(disk, 512, 0, {{0x00, 0x0C, kFirst, sectors}});
    test::MemoryStorageSource device(disk, 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    ASSERT_EQ(table->partitions.size(), 1u);
    partition::PartitionSource volumeSource(device, table->partitions.front());
    RECOVERY_ASSERT_OK(volumeSource.open());
    Volume volume = openVolume(volumeSource, table->partitions.front().offset);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = reconstructAll(device, registry, {&volume});

    const FragmentCandidate* candidate = recovered.named("Partitioned.jpg");
    expectReconstructed(device, candidate, jpeg, 2);
    ASSERT_NE(candidate, nullptr);
    const std::uint64_t volumeOffset = std::uint64_t{kFirst} * 512;
    EXPECT_EQ(candidate->volumeOffset, volumeOffset);
    EXPECT_EQ(candidate->reconstruction()->data.sourceOffset(), volumeOffset + card.offsetOf(1000));
    EXPECT_EQ(candidate->reconstruction()->clusters.front(), (ClusterRun{1000, 10}));
}

// ===========================================================================
// Hostile data, cancellation, errors
// ===========================================================================

TEST(FragmentRecoveryFuzzTest, MutatedVolumesStayBounded) {
    const carving::FormatRegistry registry = allFormats();
    Card card(4096, 0xF22);
    const Bytes jpeg = photo(201);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 202), run(1010, 9));
    card.addDeleted("One.jpg", jpeg, pieces(n, {{1000, 10}, {1019, n}}));
    const test::Mp4Built movie = clip(203);
    const std::uint32_t m = clustersFor(movie.bytes.size());
    card.addDeleted("Two.mp4", movie.bytes, pieces(m, {{2000, 30}, {2100, m}}));
    card.plant(pieces(n, {{3000, 12}, {3030, n}}), photo(204));
    const Bytes pristine = card.image();
    const std::uint64_t fatBegin = card.builder().fatOffset(0);
    const std::uint64_t dataBegin = card.builder().clusterOffset(2);

    FragmentRecoveryOptions options;
    options.limits.maxValidations = 48;
    options.limits.maxContinuations = 8;
    options.limits.maxSampleProbes = 2000;
    std::mt19937_64 random(0xF7A6);
    int searched = 0;
    for (int round = 0; round < 40; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        Bytes image = pristine;
        const int mutations = 1 + static_cast<int>(random() % 24);
        for (int i = 0; i < mutations; ++i) {
            // The FATs, the root directory and the data of the files.
            const std::uint64_t region = random() % 3;
            const std::uint64_t offset = region == 0   ? fatBegin + random() % (dataBegin - fatBegin)
                                         : region == 1 ? dataBegin + random() % kClusterSize
                                                       : card.offsetOf(1000) + random() % (2200ULL * kClusterSize);
            image[static_cast<std::size_t>(offset)] = static_cast<std::byte>(random() & 0xFF);
        }
        test::MemoryStorageSource source(std::move(image));
        RECOVERY_ASSERT_OK(source.open());
        Result<std::unique_ptr<FilesystemRecovery>> opened = openFilesystemRecovery(source, 0);
        if (!opened.ok()) {
            continue;
        }
        Volume volume;
        volume.recovery = std::move(opened).value();
        Result<CandidateScan> scan = volume.recovery->findCandidates({}, {});
        if (!scan.ok()) {
            continue;
        }
        volume.scan = std::move(scan).value();
        const Recovered recovered = reconstructAll(source, registry, {&volume}, options);
        for (const FragmentCandidate& candidate : recovered.candidates) {
            EXPECT_LE(candidate.search.layoutsValidated, options.limits.maxValidations);
            for (const ReconstructionHypothesis& hypothesis : candidate.hypotheses) {
                for (const SourceRegion& region : hypothesis.data.sourceRegions) {
                    if (region.kind == RegionKind::Stored) {
                        EXPECT_GE(region.sourceOffset, dataBegin);
                        EXPECT_LE(region.sourceOffset + region.length, source.size());
                    }
                }
            }
        }
        ++searched;
    }
    EXPECT_GT(searched, 20);
}

TEST(FragmentRecoveryTest, CancellationStopsTheRun) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const Bytes jpeg = photo(211);
    const std::uint32_t n = clustersFor(jpeg.size());
    card.addDeleted("Cancelled.jpg", jpeg, pieces(n, {{1000, 10}, {1040, n}}));
    const Bytes image = card.image();
    std::size_t delivered = 0;
    const FragmentCandidateSink count = [&](FragmentCandidate&&) -> Status {
        ++delivered;
        return success();
    };

    // Before the run.
    {
        test::MemoryStorageSource source(image);
        RECOVERY_ASSERT_OK(source.open());
        Volume volume = openVolume(source);
        ASSERT_NE(volume.recovery, nullptr);
        CancellationSource cancel;
        cancel.requestCancellation();
        FragmentRecoveryOptions options;
        options.carving.scan.reads.cancellation = cancel.token();
        FragmentRecovery recovery(source, registry, options);
        RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
        RECOVERY_EXPECT_ERROR(recovery.run(count), ErrorCode::Cancelled);
    }
    // During a search: when it reads beyond the first piece.
    {
        test::VirtualSource source(image.size());
        source.plant(0, image);
        CancellationSource cancel;
        const std::uint64_t beyond = card.offsetOf(1010);
        source.setReadHook([&](std::uint64_t offset, std::size_t length) {
            if (offset + length > beyond && offset < card.offsetOf(8000)) {
                cancel.requestCancellation();
            }
        });
        RECOVERY_ASSERT_OK(source.open());
        Volume volume = openVolume(source);
        ASSERT_NE(volume.recovery, nullptr);
        FragmentRecoveryOptions options;
        options.carving.scan.reads.cancellation = cancel.token();
        options.carve = false;
        FragmentRecovery recovery(source, registry, options);
        RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
        RECOVERY_EXPECT_ERROR(recovery.run(count), ErrorCode::Cancelled);
    }
    // During the scan of a large source.
    {
        test::VirtualSource source(256 * kMiB);
        source.setNoise(3);
        source.plant(0, image);
        CancellationSource cancel;
        source.setReadHook([&](std::uint64_t offset, std::size_t) {
            if (offset >= 64 * kMiB) {
                cancel.requestCancellation();
            }
        });
        RECOVERY_ASSERT_OK(source.open());
        Volume volume = openVolume(source);
        ASSERT_NE(volume.recovery, nullptr);
        FragmentRecoveryOptions options;
        options.carving.scan.reads.cancellation = cancel.token();
        FragmentRecovery recovery(source, registry, options);
        RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
        RECOVERY_EXPECT_ERROR(recovery.run(count), ErrorCode::Cancelled);
        EXPECT_LT(source.stats().highestEnd, 128 * kMiB);
    }
    EXPECT_EQ(delivered, 0u);
}

TEST(FragmentRecoveryTest, ASinkErrorStopsTheRun) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    for (std::uint32_t i = 0; i < 3; ++i) {
        const Bytes jpeg = photo(220 + i);
        card.addDeleted("Photo " + std::to_string(i) + ".jpg", jpeg, run(1000 + 100 * i, clustersFor(jpeg.size())));
    }
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    FragmentRecovery recovery(source, registry);
    RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
    int calls = 0;
    const Result<FragmentRecoveryReport> result = recovery.run([&](FragmentCandidate&&) -> Status {
        if (++calls == 2) {
            return makeError(ErrorCode::DestinationError, "destination full");
        }
        return success();
    });
    RECOVERY_EXPECT_ERROR(result, ErrorCode::DestinationError);
    EXPECT_EQ(calls, 2);
}

TEST(FragmentRecoveryTest, InvalidUseIsRefused) {
    const carving::FormatRegistry registry = allFormats();
    const FragmentCandidateSink sink = [](FragmentCandidate&&) { return success(); };
    test::MemoryStorageSource closed(Bytes(64 * kKiB), 512);
    RECOVERY_EXPECT_ERROR(FragmentRecovery(closed, registry).run(sink), ErrorCode::InvalidInput);

    test::MemoryStorageSource source(Bytes(64 * kKiB), 512);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(FragmentRecovery(source, registry).run({}), ErrorCode::InvalidInput);
    const carving::FormatRegistry empty;
    RECOVERY_EXPECT_ERROR(FragmentRecovery(source, empty).run(sink), ErrorCode::InvalidInput);
    const auto refused = [&](FragmentRecoveryOptions options, std::string_view what) {
        SCOPED_TRACE(what);
        RECOVERY_EXPECT_ERROR(FragmentRecovery(source, registry, std::move(options)).run(sink),
                              ErrorCode::InvalidInput);
    };
    FragmentRecoveryOptions options;
    options.carving.readCacheSize = 16;
    refused(options, "a cache too small");
    options = {};
    options.carving.scan.reads.sectorRetryCount = carving::SourceReadOptions::kMaxSectorRetries + 1;
    refused(options, "too many retries");
    options = {};
    options.mp4.limits.maxTracks = 0;
    refused(options, "an MP4 limit of 0");
    for (const auto& [name, clear] : std::vector<std::pair<std::string, std::function<void(FragmentSearchLimits&)>>>{
             {"maxFragments", [](FragmentSearchLimits& l) { l.maxFragments = 0; }},
             {"maxContinuations", [](FragmentSearchLimits& l) { l.maxContinuations = 0; }},
             {"minimumContinuation", [](FragmentSearchLimits& l) { l.minimumContinuation = 0; }},
             {"maxValidations", [](FragmentSearchLimits& l) { l.maxValidations = 0; }},
             {"maxAlternatives", [](FragmentSearchLimits& l) { l.maxAlternatives = 0; }}}) {
        options = {};
        clear(options.limits);
        refused(options, name);
    }

    // A scan of another volume.
    Card card(1024);
    test::MemoryStorageSource image(card.image());
    RECOVERY_ASSERT_OK(image.open());
    Volume volume = openVolume(image);
    ASSERT_NE(volume.recovery, nullptr);
    CandidateScan other = volume.scan;
    other.volumeOffset = 1 * kMiB;
    FragmentRecovery recovery(image, registry);
    RECOVERY_EXPECT_ERROR(recovery.addVolume(*volume.recovery, other), ErrorCode::InvalidInput);
}

TEST(FragmentRecoveryTest, LogsTheRun) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const Bytes jpeg = photo(231);
    const std::uint32_t n = clustersFor(jpeg.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 232), run(1010, 9));
    card.addDeleted("Logged.jpg", jpeg, pieces(n, {{1000, 10}, {1019, n}}));
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    logger.addSink(sink);
    FragmentRecoveryOptions options;
    options.carving.scan.logger = &logger;
    const Recovered recovered = reconstructAll(source, registry, {&volume}, options);
    ASSERT_EQ(recovered.candidates.size(), 1u);

    const auto records = sink->records();
    const auto named = [&](std::string_view message) {
        return std::count_if(records.begin(), records.end(), [&](const auto& r) {
            return r.component == "fragments" && r.message == message;
        });
    };
    EXPECT_EQ(named("fragment reconstruction started"), 1);
    EXPECT_EQ(named("fragment candidate"), 1);
    const auto ended = std::find_if(records.begin(), records.end(),
                                    [](const auto& r) { return r.message == "fragment reconstruction ended"; });
    ASSERT_NE(ended, records.end());
    const auto has = [&](std::string_view key, std::string_view value) {
        return std::any_of(ended->fields.begin(), ended->fields.end(),
                           [&](const auto& f) { return f.key == key && f.value == value; });
    };
    EXPECT_TRUE(has("filesystem_seeds", "1"));
    EXPECT_TRUE(has("complete", "1"));
    // No file content in any record.
    for (const auto& record : records) {
        for (const auto& fieldValue : record.fields) {
            EXPECT_LT(fieldValue.value.size(), 200u);
        }
    }
}

TEST(FragmentRecoveryTest, NamesOfStatusesOriginsAndSources) {
    EXPECT_EQ(toString(ReconstructionStatus::Complete), "COMPLETE");
    EXPECT_EQ(toString(ReconstructionStatus::Partial), "PARTIAL");
    EXPECT_EQ(toString(ReconstructionStatus::Corrupted), "CORRUPTED");
    EXPECT_EQ(toString(ReconstructionStatus::Ambiguous), "AMBIGUOUS");
    EXPECT_EQ(toString(ReconstructionStatus::Unrecoverable), "UNRECOVERABLE");
    EXPECT_EQ(toString(SeedOrigin::Filesystem), "filesystem");
    EXPECT_EQ(toString(SeedOrigin::Carving), "carving");
    EXPECT_EQ(toString(LayoutSource::Contiguous), "contiguous");
    EXPECT_EQ(toString(LayoutSource::SkipAllocated), "skip-allocated");
    EXPECT_EQ(toString(LayoutSource::SkipClaimed), "skip-claimed");
    EXPECT_EQ(toString(LayoutSource::GapSearch), "gap-search");
    EXPECT_EQ(toString(LayoutSource::SampleTables), "sample-tables");
    EXPECT_EQ(toString(RecoveryMethod::Fragmented), "FRAGMENTED");
    RECOVERY_EXPECT_OK(validate(FragmentSearchLimits{}));
}


// ===========================================================================
// Reconstruction in steps (P15)
// ===========================================================================

// The steps deliver what run() delivers; new steps restored from the first
// ones' seed examinations, pass events and first reconstruction go on to the
// same end without a pass of their own.
TEST(FragmentRecoveryTest, TheStepsRestoreFromTheirEventsAndReconstructions) {
    const carving::FormatRegistry registry = allFormats();
    Card card;
    const Bytes deleted = photo(31);
    const std::uint32_t n1 = clustersFor(deleted.size());
    (void)card.add("KEEP.BIN", test::makePattern(9 * kClusterSize, 31), run(1010, 9));
    card.addDeleted("Beach photo 31.jpg", deleted, pieces(n1, {{1000, 10}, {1019, n1}}));
    const Bytes orphan = photo(32);
    const std::uint32_t n2 = clustersFor(orphan.size());
    (void)card.add("KEEP2.BIN", test::makePattern(8 * kClusterSize, 32), run(7012, 8));
    card.plant(pieces(n2, {{7000, 12}, {7020, n2}}), orphan);
    test::MemoryStorageSource source(card.image());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered expected = reconstructAll(source, registry, {&volume});
    ASSERT_GE(expected.candidates.size(), 2u);

    const auto make = [&] {
        Result<std::unique_ptr<FragmentRecoverySteps>> steps = FragmentRecoverySteps::create(source, registry, {});
        EXPECT_TRUE(steps.ok());
        EXPECT_TRUE((*steps)->addVolume(*volume.recovery, volume.scan).ok());
        EXPECT_TRUE((*steps)->begin().ok());
        return std::move(steps).value();
    };
    std::unique_ptr<FragmentRecoverySteps> first = make();
    RECOVERY_EXPECT_ERROR(first->begin(), ErrorCode::InvalidInput);
    std::vector<FragmentSeedExamination> examinations;
    for (std::size_t i = 0; i < volume.scan.candidates.size(); ++i) {
        Result<FragmentSeedExamination> examined = first->examineSeed(0, i);
        RECOVERY_ASSERT_OK(examined);
        examinations.push_back(*examined);
        RECOVERY_ASSERT_OK(first->addSeedExamination(std::move(*examined)));
    }
    // The pass, with the registry's formats and the steps' own.
    carving::FormatRegistry passFormats;
    for (const std::shared_ptr<const carving::IFileFormat>& format : registry.formats()) {
        RECOVERY_ASSERT_OK(passFormats.add(format));
    }
    for (const std::shared_ptr<const carving::IFileFormat>& format : first->extraFormats()) {
        RECOVERY_ASSERT_OK(passFormats.add(format));
    }
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(passFormats);
    RECOVERY_ASSERT_OK(scanner);
    first->recordEvents(true);
    RECOVERY_ASSERT_OK(scanner->scan(
        source, [&](const carving::SignatureHit& hit) { return first->commit(hit, nullptr); }));
    const std::vector<FragmentPassEvent> events = first->takeEvents();
    EXPECT_FALSE(events.empty());
    EXPECT_TRUE(first->takeEvents().empty());
    std::vector<FragmentCandidate> delivered;
    Result<std::optional<FragmentCandidate>> next = first->reconstructNext();
    RECOVERY_ASSERT_OK(next);
    ASSERT_TRUE(next->has_value());
    delivered.push_back(std::move(**next));

    // New steps: the examinations, the pass's events and the reconstruction again.
    std::unique_ptr<FragmentRecoverySteps> second = make();
    for (const FragmentSeedExamination& examination : examinations) {
        RECOVERY_ASSERT_OK(second->addSeedExamination(examination));
    }
    RECOVERY_ASSERT_OK(second->replay(events));
    RECOVERY_ASSERT_OK(second->replayReconstruction(delivered.front()));
    while (second->nextSeed() < second->seedCount()) {
        Result<std::optional<FragmentCandidate>> more = second->reconstructNext();
        RECOVERY_ASSERT_OK(more);
        if (more->has_value()) {
            delivered.push_back(std::move(**more));
        }
    }
    ASSERT_EQ(delivered.size(), expected.candidates.size());
    for (std::size_t i = 0; i < delivered.size(); ++i) {
        EXPECT_EQ(describeFragments(delivered[i]), describeFragments(expected.candidates[i]));
    }
    const FragmentRecoveryReport report = second->report();
    EXPECT_EQ(report.filesystemSeeds, expected.report.filesystemSeeds);
    EXPECT_EQ(report.filesystemSkipped, expected.report.filesystemSkipped);
    EXPECT_EQ(report.carved, expected.report.carved);
    EXPECT_EQ(report.carvesValid, expected.report.carvesValid);
    EXPECT_EQ(report.carvingSeeds, expected.report.carvingSeeds);
    EXPECT_EQ(report.complete, expected.report.complete);
    // Nothing is left to reconstruct, and what is not the next seed's is refused.
    RECOVERY_EXPECT_ERROR(second->reconstructNext(), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(second->replayReconstruction(delivered.front()), ErrorCode::InvalidInput);
    // A saved hit of a format the registry does not have.
    std::unique_ptr<FragmentRecoverySteps> third = make();
    std::vector<FragmentPassEvent> unknown = {events.front()};
    unknown.front().formatId = "no-such-format";
    RECOVERY_EXPECT_ERROR(third->replay(unknown), ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery
