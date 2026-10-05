// MP4 recovery (P12): MP4 files recovered from filesystem evidence and carving
// evidence together, on generated FAT32, exFAT and NTFS volumes (on their own
// and in a partitioned disk) and on sources larger than memory. Small and
// large files; movie fragments, and files fragmented on disk whose chain or
// run list survived; moov before and after the media data; files partly
// overwritten (samples in reallocated clusters, moov lost, the start lost),
// with damaged metadata, and on bad sectors; FILESYSTEM, HYBRID and CARVING
// candidates with their structure, sample and allocation evidence; recovered
// files written, compared with the originals and validated again;
// cancellation, sink errors and invalid use.

#include "recovery/mp4_recovery.hpp"

#include "carving/content_reader.hpp"
#include "partition/partition_source.hpp"
#include "partition/partition_table.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/candidate_content.hpp"
#include "recovery/recovery_writer.hpp"
#include "support/audio_builders.hpp"
#include "support/candidate_helpers.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/mp4_builders.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <thread>

#include <optional>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace recovery {
namespace {

namespace mp4 = formats::mp4;
using Bytes = std::vector<std::byte>;
using carving::ValidationStatus;
using test::Mp4Built;
using test::Mp4MoovPlace;
using test::Mp4Options;
using test::Mp4TrackKind;
using test::Mp4TrackOptions;

constexpr std::uint32_t kClusterSize = 512;  // every builder's default geometry
constexpr std::uint64_t kNtfsRoot = test::NtfsImageBuilder::root();

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

Mp4Options seeded(std::uint64_t seed) {
    Mp4Options options;
    options.seed = seed;
    return options;
}

Mp4Options moovFirst(std::uint64_t seed) {
    Mp4Options options = seeded(seed);
    options.moov = Mp4MoovPlace::First;
    return options;
}

Mp4Options quickTime(std::uint64_t seed) {
    Mp4Options options = seeded(seed);
    options.majorBrand = "qt  ";
    options.compatibleBrands = {"qt  "};
    return options;
}

// A file of `samples` video and as many audio samples, in chunks of 10 and 20.
Mp4Options longFile(std::uint64_t seed, std::size_t samples) {
    Mp4Options options = seeded(seed);
    Mp4TrackOptions video;
    video.samples = samples;
    video.samplesPerChunk = 10;
    Mp4TrackOptions audio;
    audio.kind = Mp4TrackKind::Audio;
    audio.samples = samples;
    audio.samplesPerChunk = 20;
    options.tracks = {video, audio};
    return options;
}

std::uint32_t clustersFor(std::size_t bytes) {
    return static_cast<std::uint32_t>((bytes + kClusterSize - 1) / kClusterSize);
}

template <typename Cluster>
std::vector<Cluster> run(Cluster first, std::uint32_t count) {
    std::vector<Cluster> clusters;
    for (std::uint32_t i = 0; i < count; ++i) {
        clusters.push_back(first + i);
    }
    return clusters;
}

// The pieces of a file stored in these clusters, in this order.
template <typename Cluster>
std::size_t fragmentsOf(const std::vector<Cluster>& clusters) {
    std::size_t fragments = clusters.empty() ? 0 : 1;
    for (std::size_t i = 1; i < clusters.size(); ++i) {
        fragments += clusters[i] == clusters[i - 1] + 1 ? 0 : 1;
    }
    return fragments;
}

Bytes concat(const Bytes& first, const Bytes& second) {
    Bytes bytes = first;
    bytes.insert(bytes.end(), second.begin(), second.end());
    return bytes;
}

// The samples of `built`, per track, with a byte in file range [begin, end).
std::vector<std::uint64_t> samplesTouching(const Mp4Built& built, std::uint64_t begin, std::uint64_t end) {
    const auto touches = [&](const test::Mp4Sample& sample) {
        return sample.offset < end && sample.offset + sample.size > begin;
    };
    std::vector<std::uint64_t> counts;
    for (const std::vector<test::Mp4Sample>& track : built.samples) {
        counts.push_back(static_cast<std::uint64_t>(std::count_if(track.begin(), track.end(), touches)));
    }
    return counts;
}

std::uint64_t sampleCount(const Mp4Built& built) {
    std::uint64_t total = 0;
    for (const std::vector<test::Mp4Sample>& track : built.samples) {
        total += track.size();
    }
    return total;
}

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

std::string_view statusName(mp4::FileStatus status) {
    switch (status) {
    case mp4::FileStatus::Valid:
        return "Valid";
    case mp4::FileStatus::Truncated:
        return "Truncated";
    case mp4::FileStatus::Invalid:
        return "Invalid";
    }
    return "?";
}

// Several lines, for failure messages.
std::string describeMp4(const Mp4Candidate& candidate) {
    std::string text = std::string(toString(candidate.data.method)) + " " + test::describeCandidate(candidate.data);
    const Mp4Structure& structure = candidate.structure;
    text += "  structure " + std::string(statusName(structure.status)) + ": " + structure.detail + "\n";
    for (const mp4::Issue& issue : structure.issues) {
        text += "  issue " + issue.describe() + "\n";
    }
    for (const Mp4TrackEvidence& track : structure.tracks) {
        text += "  track " + std::to_string(track.number) + ": " + std::to_string(track.samples) + " samples, " +
                std::to_string(track.samplesIntact) + " intact, " + std::to_string(track.samplesBeyondData) +
                " beyond, " + std::to_string(track.samplesMissing) + " missing, " +
                std::to_string(track.samplesUnreadable) + " unreadable, " + std::to_string(track.samplesReallocated) +
                " reallocated, " + std::to_string(track.samplesMisframed) + " of " +
                std::to_string(track.samplesFramed) + " misframed\n";
    }
    for (const Mp4Warning warning : candidate.warnings) {
        text += "  MP4 warning " + std::string(toString(warning)) + "\n";
    }
    if (candidate.carving.has_value()) {
        text += "  carve at " + std::to_string(candidate.carving->sourceOffset) + ", " +
                std::to_string(candidate.carving->length) + " bytes, " +
                std::string(carving::toString(candidate.carving->end.status)) + ", " +
                std::string(carving::toString(candidate.carving->validation.status)) + ": " +
                candidate.carving->validation.detail + "\n";
    }
    return text;
}

std::string carvedName(std::uint64_t id, std::string_view extension) {
    std::string number = std::to_string(id);
    number.insert(0, number.size() < 6 ? 6 - number.size() : 0, '0');
    return "recovered_" + number + "." + std::string(extension);
}

// What every MP4 candidate satisfies, whatever its evidence.
void expectConsistent(const Mp4Candidate& candidate) {
    SCOPED_TRACE(describeMp4(candidate));
    const RecoveryCandidate& data = candidate.data;
    RECOVERY_EXPECT_OK(validateCandidate(data));
    // The invariants of P7's candidates hold for every method.
    EXPECT_EQ(data.hasWarning(CandidateWarning::ClustersReallocated), data.reallocatedBytes() > 0);
    EXPECT_EQ(data.hasWarning(CandidateWarning::DataMissing), data.bytes(RegionKind::Missing) > 0);
    EXPECT_EQ(data.hasWarning(CandidateWarning::LayoutGuessed),
              data.filesystemEvidence.allocation.layout == LayoutEvidence::Guessed);

    const bool carved = data.method == RecoveryMethod::Carving;
    EXPECT_EQ(candidate.filesystemCandidate.has_value(), !carved);
    EXPECT_EQ(candidate.recordedSize.has_value(), !carved);
    EXPECT_TRUE(carved || !candidate.allocation.has_value());
    if (data.method == RecoveryMethod::Filesystem) {
        EXPECT_EQ(candidate.recordedSize, data.expectedSize);
    }
    if (carved) {
        ASSERT_TRUE(candidate.carving.has_value());
        EXPECT_TRUE(data.filesystemEvidence.path.empty());
        EXPECT_EQ(data.filename, carvedName(data.id.value(), data.extension));
        EXPECT_EQ(data.sourceOffset(), candidate.carving->sourceOffset);
        EXPECT_EQ(data.expectedSize, candidate.carving->length);
        EXPECT_FALSE(data.fragmentation.known);
        std::uint64_t position = candidate.carving->sourceOffset;
        for (const SourceRegion& region : data.sourceRegions) {
            EXPECT_EQ(region.kind, RegionKind::Stored);
            EXPECT_EQ(region.sourceOffset, position);
            position += region.length;
        }
    }
    if (candidate.allocation.has_value() && candidate.allocation->complete) {
        const Mp4Allocation& allocation = *candidate.allocation;
        EXPECT_EQ(allocation.clusters,
                  allocation.freeClusters + allocation.allocatedClusters + allocation.otherClusters);
    }

    // Each sample is counted once.
    const Mp4Structure& structure = candidate.structure;
    for (const Mp4TrackEvidence& track : structure.tracks) {
        EXPECT_EQ(track.samples, track.samplesBeyondData + track.samplesMissing + track.samplesUnreadable +
                                     track.samplesReallocated + track.samplesIntact)
            << "track " << track.number;
        EXPECT_LE(track.samplesMisframed, track.samplesFramed);
        EXPECT_LE(track.samplesFramed, track.samples);
    }
    if (structure.moovFoundBySearch) {
        EXPECT_EQ(structure.status, mp4::FileStatus::Invalid);
    }

    // The warnings say what the evidence says.
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::StructureInvalid), structure.status == mp4::FileStatus::Invalid);
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::StructureTruncated), structure.status == mp4::FileStatus::Truncated);
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::NoMovie), !structure.moovOffset.has_value());
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::MovieFoundBySearch), structure.moovFoundBySearch);
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::SamplesDamaged), structure.samplesDamaged() > 0);
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::SamplesMisframed), structure.samplesMisframed() > 0);
    const bool inside = candidate.allocation.has_value() && candidate.allocation->insideActiveFile;
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::InsideActiveFile), inside);
    EXPECT_EQ(candidate.hasWarning(Mp4Warning::AllocatedClusters),
              !inside && candidate.allocation.has_value() && candidate.allocation->allocatedClusters > 0);
}

// A volume opened for recovery, with its filesystem candidates.
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

// What a run delivered, in delivery order.
struct Recovered {
    Mp4RecoveryReport report;
    std::vector<Mp4Candidate> candidates;

    // The candidate whose original path is `path`, or nullptr.
    [[nodiscard]] const Mp4Candidate* at(std::string_view path) const {
        const Mp4Candidate* found = nullptr;
        for (const Mp4Candidate& candidate : candidates) {
            if (candidate.data.filesystemEvidence.path == path) {
                EXPECT_EQ(found, nullptr) << "two candidates at " << path;
                found = &candidate;
            }
        }
        return found;
    }

    // The carving candidate starting at source offset `offset`, or nullptr.
    [[nodiscard]] const Mp4Candidate* carvedAt(std::uint64_t offset) const {
        for (const Mp4Candidate& candidate : candidates) {
            if (candidate.data.method == RecoveryMethod::Carving && candidate.data.sourceOffset() == offset) {
                return &candidate;
            }
        }
        return nullptr;
    }
};

Recovered recover(storage::IStorageSource& source, const std::vector<Volume*>& volumes,
                  Mp4RecoveryOptions options = {}) {
    Recovered recovered;
    Mp4Recovery recovery(source, std::move(options));
    for (Volume* volume : volumes) {
        const Status added = recovery.addVolume(*volume->recovery, volume->scan);
        EXPECT_TRUE(added.ok()) << (added.ok() ? "" : describe(added.error()));
    }
    Result<Mp4RecoveryReport> report = recovery.run([&](Mp4Candidate&& candidate) -> Status {
        recovered.candidates.push_back(std::move(candidate));
        return success();
    });
    EXPECT_TRUE(report.ok()) << (report.ok() ? "" : describe(report.error()));
    if (report.ok()) {
        recovered.report = std::move(report).value();
        EXPECT_EQ(recovered.report.candidates(), recovered.candidates.size());
        EXPECT_EQ(recovered.report.valid + recovered.report.truncated + recovered.report.invalid,
                  recovered.candidates.size());
    }
    std::uint64_t id = recovery.options().firstId;
    for (const Mp4Candidate& candidate : recovered.candidates) {
        EXPECT_EQ(candidate.data.id.value(), id++);
        expectConsistent(candidate);
    }
    return recovered;
}

// The candidate's data is exactly `original`, every byte read, and its
// structure intact with every sample intact.
void expectRecovered(storage::IStorageSource& source, const Mp4Candidate* candidate, const Bytes& original) {
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeMp4(*candidate));
    EXPECT_TRUE(candidate->structure.intact());
    EXPECT_EQ(candidate->structure.samplesIntact(), candidate->structure.samples());
    EXPECT_GT(candidate->structure.samples(), 0u);
    EXPECT_EQ(candidate->structure.structureEnd, original.size());
    EXPECT_EQ(candidate->structure.kind, mp4::MediaKind::Video);
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, candidate->data);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_TRUE(rebuilt.report.allBytesRead());
    EXPECT_EQ(rebuilt.report.outputSize, original.size());
    EXPECT_TRUE(rebuilt.data == original);
}

// Copies `bytes` into the image at cluster `cluster`, outside any file.
void plant(test::Fat32ImageBuilder& builder, std::uint32_t cluster, const Bytes& bytes) {
    std::copy(bytes.begin(), bytes.end(),
              builder.raw().begin() + static_cast<std::ptrdiff_t>(builder.clusterOffset(cluster)));
}

// ===========================================================================
// Analysis
// ===========================================================================

TEST(Mp4RecoveryTest, AnalysisDescribesFilesOfEveryLayout) {
    struct Layout {
        std::string what;
        Mp4Options options;
        bool moovFirst = false;
    };
    std::vector<Layout> layouts;
    layouts.push_back({"moov after the media data", seeded(1), false});
    Mp4Options large = moovFirst(2);
    large.largeMediaData = true;
    large.co64 = true;
    layouts.push_back({"moov first, 64-bit sizes", large, true});
    Mp4Options fragments = moovFirst(3);
    fragments.fragmentSamples = 3;
    fragments.samplesInMoov = 2;
    fragments.fragmentBase = test::Mp4FragmentBase::Implicit;
    layouts.push_back({"movie fragments", fragments, true});
    Mp4Options hevc = seeded(4);
    hevc.tracks = {Mp4TrackOptions{}};
    hevc.tracks[0].hevc = true;
    hevc.tracks[0].nalLengthSize = 2;
    hevc.mediaDataBoxes = 2;
    hevc.moov = Mp4MoovPlace::Between;
    layouts.push_back({"HEVC, moov between two mdat boxes", hevc, false});

    for (const Layout& layout : layouts) {
        SCOPED_TRACE(layout.what);
        const Mp4Built built = test::makeMp4(layout.options);
        carving::MemoryContentReader content(built.bytes);
        const Result<Mp4Structure> analyzed = analyzeMp4(content);
        RECOVERY_ASSERT_OK(analyzed);
        const Mp4Structure& structure = *analyzed;
        EXPECT_EQ(structure.status, mp4::FileStatus::Valid) << structure.detail;
        EXPECT_TRUE(structure.intact());
        EXPECT_EQ(structure.issueCount, 0u);
        EXPECT_EQ(structure.kind, mp4::MediaKind::Video) << structure.kindReason;
        EXPECT_EQ(structure.majorBrand, mp4::FourCc("isom"));
        EXPECT_FALSE(structure.moovFoundBySearch);
        EXPECT_EQ(structure.moovBeforeMediaData, layout.moovFirst);
        const test::BoxPosition moov = test::findBox(test::mp4Boxes(built.bytes), "moov");
        EXPECT_EQ(structure.moovOffset, moov.offset);
        EXPECT_EQ(structure.moovSize, moov.size);
        EXPECT_EQ(structure.structureEnd, built.bytes.size());
        EXPECT_EQ(structure.dataSize, built.bytes.size());
        if (layout.options.fragmentSamples == 0) {
            EXPECT_EQ(structure.mediaDataBoxes, layout.options.mediaDataBoxes);
            EXPECT_EQ(structure.movieFragments, 0u);
        } else {
            EXPECT_EQ(structure.movieFragments, built.runs[0].size());
            EXPECT_EQ(structure.mediaDataBoxes, structure.movieFragments + 1);
        }

        ASSERT_EQ(structure.tracks.size(), built.samples.size());
        std::uint64_t begin = UINT64_MAX;
        std::uint64_t end = 0;
        std::uint64_t bytes = 0;
        for (std::size_t t = 0; t < built.samples.size(); ++t) {
            const Mp4TrackEvidence& track = structure.tracks[t];
            std::uint64_t trackBytes = 0;
            for (const test::Mp4Sample& sample : built.samples[t]) {
                trackBytes += sample.size;
                begin = std::min(begin, sample.offset);
                end = std::max(end, sample.offset + sample.size);
            }
            bytes += trackBytes;
            EXPECT_EQ(track.number, t + 1);
            EXPECT_EQ(track.samples, built.samples[t].size());
            EXPECT_EQ(track.sampleBytes, trackBytes);
            EXPECT_EQ(track.samplesIntact, track.samples);
            EXPECT_EQ(track.samplesMisframed, 0u);
            const Mp4TrackOptions& options = layout.options.tracks[t];
            if (options.kind == Mp4TrackKind::Video) {
                EXPECT_EQ(track.kind, mp4::TrackKind::Video);
                EXPECT_EQ(track.codec, options.hevc ? mp4::FourCc("hvc1") : mp4::FourCc("avc1"));
                EXPECT_EQ(track.width, options.width);
                EXPECT_EQ(track.height, options.height);
                EXPECT_EQ(track.samplesFramed, track.samples);
            } else {
                EXPECT_EQ(track.kind, mp4::TrackKind::Audio);
                EXPECT_EQ(track.codec, mp4::FourCc("mp4a"));
                EXPECT_EQ(track.channels, options.channels);
                EXPECT_EQ(track.sampleRate, options.sampleRate);
                EXPECT_EQ(track.samplesFramed, 0u);
            }
            EXPECT_GT(track.timescale, 0u);
        }
        ASSERT_TRUE(structure.media.has_value());
        EXPECT_EQ(structure.media->begin, begin);
        EXPECT_EQ(structure.media->end, end);
        EXPECT_EQ(structure.media->bytes, bytes);
        EXPECT_EQ(structure.media->samples, sampleCount(built));
    }
}

TEST(Mp4RecoveryTest, AnalysisFindsAMoovTheTopLevelBoxesDoNotLeadTo) {
    const Mp4Built built = test::makeMp4(seeded(5));  // moov last
    const test::BoxPosition moov = test::findBox(test::mp4Boxes(built.bytes), "moov");

    // Another file's data over the first sector: ftyp, mdat's header and the first samples are gone.
    Bytes damaged = built.bytes;
    const Bytes other = test::makePattern(512, 6);
    std::copy(other.begin(), other.end(), damaged.begin());
    carving::MemoryContentReader content(damaged);
    const Result<Mp4Structure> found = analyzeMp4(content);
    RECOVERY_ASSERT_OK(found);
    EXPECT_EQ(found->status, mp4::FileStatus::Invalid);
    EXPECT_TRUE(found->moovFoundBySearch);
    EXPECT_NE(found->detail.find("moov found by searching the data at offset " + std::to_string(moov.offset)),
              std::string::npos)
        << found->detail;
    EXPECT_EQ(found->moovOffset, moov.offset);
    EXPECT_FALSE(found->majorBrand.has_value());
    EXPECT_EQ(found->kind, mp4::MediaKind::Video) << found->kindReason;  // its tracks tell
    ASSERT_EQ(found->tracks.size(), 2u);
    EXPECT_EQ(found->tracks[0].samples, built.samples[0].size());
    // The samples the sector held are not their own.
    const std::vector<std::uint64_t> lost = samplesTouching(built, 0, 512);
    ASSERT_GT(lost[0], 0u);
    EXPECT_GT(found->tracks[0].samplesMisframed, 0u);
    EXPECT_LE(found->tracks[0].samplesMisframed, lost[0]);
    EXPECT_FALSE(found->intact());

    // Without the framing check, only the structure.
    formats::Mp4FormatOptions structureOnly;
    structureOnly.checkSampleFraming = false;
    carving::MemoryContentReader again(damaged);
    const Result<Mp4Structure> unframed = analyzeMp4(again, structureOnly);
    RECOVERY_ASSERT_OK(unframed);
    EXPECT_TRUE(unframed->moovFoundBySearch);
    EXPECT_EQ(unframed->tracks[0].samplesFramed, 0u);
    EXPECT_EQ(unframed->samplesMisframed(), 0u);

    // moov overwritten as well: nothing locates the samples.
    Bytes gone = built.bytes;
    const Bytes more = test::makePattern(built.bytes.size() - moov.offset, 7);
    std::copy(more.begin(), more.end(), gone.begin() + static_cast<std::ptrdiff_t>(moov.offset));
    carving::MemoryContentReader goneContent(gone);
    const Result<Mp4Structure> none = analyzeMp4(goneContent);
    RECOVERY_ASSERT_OK(none);
    EXPECT_NE(none->status, mp4::FileStatus::Valid);
    EXPECT_FALSE(none->moovOffset.has_value());
    EXPECT_FALSE(none->moovFoundBySearch);
    EXPECT_TRUE(none->tracks.empty());
    EXPECT_FALSE(none->media.has_value());
    EXPECT_EQ(none->majorBrand, mp4::FourCc("isom"));
    EXPECT_EQ(none->mediaDataBoxes, 1u);
    EXPECT_FALSE(none->intact());
}

TEST(Mp4RecoveryTest, CarvedFilesAreNamedAfterTheirBrand) {
    EXPECT_EQ(mp4Extension(std::nullopt), "mp4");
    EXPECT_EQ(mp4Extension(mp4::FourCc("isom")), "mp4");
    EXPECT_EQ(mp4Extension(mp4::FourCc("mp42")), "mp4");
    EXPECT_EQ(mp4Extension(mp4::FourCc("qt  ")), "mov");
    EXPECT_EQ(mp4Extension(mp4::FourCc("M4V ")), "m4v");
    EXPECT_EQ(mp4Extension(mp4::FourCc("M4VH")), "m4v");
    EXPECT_EQ(mp4Extension(mp4::FourCc("3gp4")), "3gp");
    EXPECT_EQ(mp4Extension(mp4::FourCc("3gg6")), "3gp");
    EXPECT_EQ(mp4Extension(mp4::FourCc("3gs7")), "3gp");
    EXPECT_EQ(mp4Extension(mp4::FourCc("3g2a")), "3g2");
    for (const Mp4Warning warning :
         {Mp4Warning::StructureInvalid, Mp4Warning::StructureTruncated, Mp4Warning::NoMovie,
          Mp4Warning::MovieFoundBySearch, Mp4Warning::SamplesDamaged, Mp4Warning::SamplesMisframed,
          Mp4Warning::SizeMismatch, Mp4Warning::AllocatedClusters, Mp4Warning::InsideActiveFile}) {
        EXPECT_NE(toString(warning), "Unknown");
    }
    EXPECT_EQ(toString(RecoveryMethod::Carving), "CARVING");
    EXPECT_EQ(toString(RecoveryMethod::Hybrid), "HYBRID");
}

// ===========================================================================
// FILESYSTEM candidates
// ===========================================================================

TEST(Mp4RecoveryTest, Fat32FilesOfEveryLayoutAreFilesystemCandidates) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 16384;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    const std::uint32_t camera =
        builder.addDirectory(builder.addDirectory(root, "DCIM").clusters.front(), "100MEDIA").clusters.front();
    const std::uint32_t movies = builder.addDirectory(root, "Movies").clusters.front();

    struct File {
        std::string path;
        Bytes bytes;
        bool moovFirst = false;
        bool contiguous = true;
    };
    std::vector<File> files;
    const auto add = [&](std::uint32_t directory, const std::string& path, Bytes bytes, bool first) {
        (void)builder.addFile(directory, path.substr(path.rfind('/') + 1), bytes);
        files.push_back({path, std::move(bytes), first});
    };
    add(camera, "/DCIM/100MEDIA/CLIP0001.MP4", test::makeMp4(seeded(11)).bytes, false);
    Mp4Options large = moovFirst(12);
    large.largeMediaData = true;
    large.co64 = true;
    add(camera, "/DCIM/100MEDIA/CLIP0002.MP4", test::makeMp4(large).bytes, true);
    Mp4Options stream = moovFirst(13);
    stream.fragmentSamples = 4;
    add(camera, "/DCIM/100MEDIA/STREAM.MP4", test::makeMp4(stream).bytes, true);
    add(movies, "/Movies/Holiday.mov", test::makeMp4(quickTime(14)).bytes, false);
    add(movies, "/Movies/LONG.MP4", test::makeMp4(longFile(15, 600)).bytes, false);
    // Fragmented on disk: the FAT chain records where each piece is.
    const Bytes split = test::makeMp4(moovFirst(16)).bytes;
    std::vector<std::uint32_t> clusters;
    for (std::uint32_t i = 0; i < clustersFor(split.size()); ++i) {
        clusters.push_back(i < 5 ? 9000 + i : i < 10 ? 9500 + i : 9100 + i);
    }
    (void)builder.addFileInClusters(root, "SPLIT.MP4", split, clusters);
    files.push_back({"/SPLIT.MP4", split, true, false});
    const std::size_t splitFragments = fragmentsOf(clusters);
    (void)builder.addFile(root, "notes.txt", test::makePattern(3000, 17));

    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});
    ASSERT_EQ(recovered.candidates.size(), files.size());

    for (const File& file : files) {
        SCOPED_TRACE(file.path);
        const Mp4Candidate* candidate = recovered.at(file.path);
        ASSERT_NE(candidate, nullptr);
        EXPECT_EQ(candidate->data.method, RecoveryMethod::Filesystem);
        const RecoveryCandidate* original = test::candidateAt(volume.scan, file.path);
        ASSERT_NE(original, nullptr);
        EXPECT_EQ(candidate->filesystemCandidate, original->id);
        EXPECT_EQ(candidate->data.sourceRegions, original->sourceRegions);
        EXPECT_EQ(candidate->recordedSize, file.bytes.size());
        EXPECT_EQ(candidate->structure.moovBeforeMediaData, file.moovFirst);
        expectRecovered(source, candidate, file.bytes);
        EXPECT_TRUE(candidate->warnings.empty());
        // Carving found it too, at its first cluster: evidence, not a second candidate.
        ASSERT_TRUE(candidate->carving.has_value());
        EXPECT_EQ(candidate->carving->sourceOffset, original->sourceOffset());
        if (file.contiguous) {
            EXPECT_EQ(candidate->carving->length, file.bytes.size());
            EXPECT_EQ(candidate->carving->validation.status, ValidationStatus::Valid);
        } else {
            // Read on contiguously, the pieces are not the file: only the chain gives its layout.
            EXPECT_NE(candidate->carving->validation.status, ValidationStatus::Valid);
            EXPECT_EQ(candidate->data.fragmentation.fragmentCount, splitFragments);
        }
    }
    EXPECT_GT(recovered.at("/DCIM/100MEDIA/STREAM.MP4")->structure.movieFragments, 0u);
    EXPECT_EQ(recovered.at("/Movies/LONG.MP4")->structure.samples(), 1200u);
    EXPECT_EQ(recovered.at("/Movies/Holiday.mov")->structure.majorBrand, mp4::FourCc("qt  "));

    const Mp4RecoveryReport& report = recovered.report;
    EXPECT_EQ(report.filesystemExamined, files.size());
    EXPECT_EQ(report.filesystemMp4, files.size());
    EXPECT_EQ(report.filesystem, files.size());
    EXPECT_EQ(report.hybrid, 0u);
    EXPECT_EQ(report.carving, 0u);
    EXPECT_EQ(report.carved, files.size());
    EXPECT_EQ(report.carvesMerged, files.size());
    EXPECT_EQ(report.valid, files.size());
    EXPECT_EQ(report.scan.bytesRead, source.size());
}

TEST(Mp4RecoveryTest, DeletedFilesWhoseLayoutExFatAndNtfsRecordedAreFilesystemCandidates) {
    // exFAT: an active file, a deleted contiguous one, and a deleted fragmented one whose FAT
    // chain survived (with movie fragments too).
    test::ExFatImageBuilder exfat;
    const Bytes active = test::makeMp4(moovFirst(23)).bytes;
    (void)exfat.addFile(exfat.root(), "active.MP4", active);
    const Bytes contiguous = test::makeMp4(seeded(21)).bytes;
    exfat.deleteEntry(exfat.addFile(exfat.root(), "clip.mp4", contiguous));
    Mp4Options stream = moovFirst(22);
    stream.fragmentSamples = 3;
    const Bytes streamed = test::makeMp4(stream).bytes;
    std::vector<std::uint32_t> pieces;
    for (std::uint32_t i = 0; i < clustersFor(streamed.size()); ++i) {
        pieces.push_back(i < 6 ? 1000 + i : i < 12 ? 1300 + i : 1200 + i);
    }
    exfat.deleteEntry(exfat.addFileInClusters(exfat.root(), "stream.mp4", streamed, pieces));

    // NTFS: a deleted fragmented file (its data runs survive the deletion) and an active one.
    test::NtfsImageBuilder ntfs;
    const Bytes movie = test::makeMp4(seeded(24)).bytes;
    std::vector<std::uint64_t> runs;
    for (std::uint32_t i = 0; i < clustersFor(movie.size()); ++i) {
        runs.push_back(i < 7 ? 5000 + i : i < 14 ? 5100 + i : 5050 + i);
    }
    const auto deleted = ntfs.addFileInClusters(kNtfsRoot, "deleted movie.mp4", movie, runs);
    const Bytes film = test::makeMp4(quickTime(25)).bytes;
    (void)ntfs.addFile(kNtfsRoot, "film.mov", film);
    ntfs.deleteEntry(deleted);
    ASSERT_GT(fragmentsOf(pieces), 1u);
    ASSERT_GT(fragmentsOf(runs), 1u);

    struct Expected {
        std::string path;
        const Bytes* bytes;
        bool deleted;
        std::size_t fragments;
    };
    const std::vector<std::pair<std::vector<std::byte>, std::vector<Expected>>> volumes = {
        {exfat.build(),
         {{"/active.MP4", &active, false, 1},
          {"/clip.mp4", &contiguous, true, 1},
          {"/stream.mp4", &streamed, true, fragmentsOf(pieces)}}},
        {ntfs.build(), {{"/deleted movie.mp4", &movie, true, fragmentsOf(runs)}, {"/film.mov", &film, false, 1}}}};
    for (const auto& [image, expected] : volumes) {
        test::MemoryStorageSource source(image);
        RECOVERY_ASSERT_OK(source.open());
        Volume volume = openVolume(source);
        ASSERT_NE(volume.recovery, nullptr);
        SCOPED_TRACE(std::string(filesystem::toString(volume.scan.filesystemInfo.type)));
        const Recovered recovered = recover(source, {&volume});
        EXPECT_EQ(recovered.candidates.size(), expected.size());
        for (const Expected& file : expected) {
            SCOPED_TRACE(file.path);
            const Mp4Candidate* candidate = recovered.at(file.path);
            ASSERT_NE(candidate, nullptr);
            EXPECT_EQ(candidate->data.method, RecoveryMethod::Filesystem);
            EXPECT_EQ(candidate->data.isDeleted(), file.deleted);
            EXPECT_EQ(candidate->data.filesystemEvidence.allocation.layout, LayoutEvidence::Recorded);
            EXPECT_EQ(candidate->data.fragmentation.fragmentCount, file.fragments);
            expectRecovered(source, candidate, *file.bytes);
            EXPECT_TRUE(candidate->warnings.empty());
            ASSERT_TRUE(candidate->carving.has_value());
            const bool whole = candidate->carving->validation.status == ValidationStatus::Valid &&
                               candidate->carving->length == file.bytes->size();
            EXPECT_EQ(whole, file.fragments == 1);
        }
        // Read on contiguously, the movie-fragmented file ends at a fragment boundary after its first
        // piece: its first fragments are a valid, shorter file (the samples of the last one past the
        // piece are audio, which is not checked by content). Only the chain gives the rest.
        if (const Mp4Candidate* cutShort = recovered.at("/stream.mp4"); cutShort != nullptr) {
            ASSERT_TRUE(cutShort->carving.has_value());
            EXPECT_EQ(cutShort->carving->validation.status, ValidationStatus::Valid);
            EXPECT_LT(cutShort->carving->length, streamed.size());
        }
        EXPECT_EQ(recovered.report.carving, 0u);
        EXPECT_EQ(recovered.report.hybrid, 0u);
    }
}

// ===========================================================================
// HYBRID candidates
// ===========================================================================

TEST(Mp4RecoveryTest, AGuessedLayoutThatTheStructureConfirmsIsHybrid) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    // Windows freed the chains: only the first cluster and the size survive.
    const Bytes whole = test::makeMp4(seeded(31)).bytes;
    builder.deleteEntry(builder.addFile(root, "Beach video 01.mp4", whole));
    // Fragmented before it was deleted: the contiguous guess runs into another file and into free clusters.
    const Mp4Built splitFile = test::makeMp4(moovFirst(32));
    const Bytes& split = splitFile.bytes;
    std::vector<std::uint32_t> clusters;
    for (std::uint32_t i = 0; i < clustersFor(split.size()); ++i) {
        clusters.push_back(i < 4 ? 3000 + i : 3100 + i);
    }
    const auto fragmented = builder.addFileInClusters(root, "Beach video 02.mp4", split, clusters);
    (void)builder.addFileInClusters(root, "GAP.BIN", test::makePattern(3 * kClusterSize, 33), {3004, 3005, 3006});
    builder.deleteEntry(fragmented);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});

    const Mp4Candidate* hybrid = recovered.at("/Beach video 01.mp4");
    ASSERT_NE(hybrid, nullptr);
    EXPECT_EQ(hybrid->data.method, RecoveryMethod::Hybrid) << describeMp4(*hybrid);
    EXPECT_TRUE(hybrid->data.isDeleted());
    // Still a guess, which the structure confirms.
    EXPECT_EQ(hybrid->data.filesystemEvidence.allocation.layout, LayoutEvidence::Guessed);
    EXPECT_TRUE(hybrid->data.hasWarning(CandidateWarning::LayoutGuessed));
    EXPECT_FALSE(hybrid->data.fragmentation.known);
    expectRecovered(source, hybrid, whole);
    EXPECT_TRUE(hybrid->warnings.empty());
    ASSERT_TRUE(hybrid->carving.has_value());
    EXPECT_EQ(hybrid->carving->validation.status, ValidationStatus::Valid);
    EXPECT_EQ(hybrid->carving->length, whole.size());

    // The guess for the fragmented file does not hold, and the structure shows it.
    const Mp4Candidate* guessed = recovered.at("/Beach video 02.mp4");
    ASSERT_NE(guessed, nullptr);
    SCOPED_TRACE(describeMp4(*guessed));
    EXPECT_EQ(guessed->data.method, RecoveryMethod::Filesystem);
    EXPECT_EQ(guessed->structure.status, mp4::FileStatus::Valid);  // moov is in the first piece
    EXPECT_FALSE(guessed->structure.intact());
    EXPECT_TRUE(guessed->hasWarning(Mp4Warning::SamplesMisframed));
    // GAP.BIN's clusters are allocated again.
    EXPECT_TRUE(guessed->hasWarning(Mp4Warning::SamplesDamaged));
    std::uint64_t reallocated = 0;
    for (const std::uint64_t count : samplesTouching(splitFile, 4 * kClusterSize, 7 * kClusterSize)) {
        reallocated += count;
    }
    EXPECT_EQ(guessed->structure.tracks[0].samplesReallocated + guessed->structure.tracks[1].samplesReallocated,
              reallocated);
    // Its carve (contiguous as well) is no better: attached, not a candidate of its own.
    ASSERT_TRUE(guessed->carving.has_value());
    EXPECT_NE(guessed->carving->validation.status, ValidationStatus::Valid);

    EXPECT_EQ(recovered.report.hybrid, 1u);
    EXPECT_EQ(recovered.report.filesystem, 1u);
    EXPECT_EQ(recovered.report.carving, 0u);
    EXPECT_EQ(recovered.report.carvesMerged, 2u);
}

TEST(Mp4RecoveryTest, ACarveAtAnEntrysFirstClusterGivesTheLayoutTheMetadataCannot) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    // The size was never written (the recording was cut off): the entry locates no data.
    const Bytes unsized = test::makeMp4(seeded(41)).bytes;
    const auto first = builder.addFile(root, "Recording 0001.mp4", unsized);
    builder.deleteEntry(first);
    storeLe32(builder.shortEntry(first), 28, 0);
    // The recorded size is short of the file's.
    const Bytes grown = test::makeMp4(seeded(42)).bytes;
    const auto second = builder.addFile(root, "Recording 0002.mp4", grown);
    builder.deleteEntry(second);
    const auto half = static_cast<std::uint32_t>(grown.size() / 2);
    storeLe32(builder.shortEntry(second), 28, half);
    // No size, and two of its clusters taken by a file written since.
    const Mp4Built reused = test::makeMp4(moovFirst(43));
    const auto third = builder.addFile(root, "Recording 0003.mp4", reused.bytes);
    builder.deleteEntry(third);
    storeLe32(builder.shortEntry(third), 28, 0);
    const std::uint32_t taken = third.clusters[6];
    (void)builder.addFileInClusters(root, "NEW.BIN", test::makePattern(2 * kClusterSize, 44), {taken, taken + 1});
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});

    const Mp4Candidate* noSize = recovered.at("/Recording 0001.mp4");
    ASSERT_NE(noSize, nullptr);
    EXPECT_EQ(noSize->data.method, RecoveryMethod::Hybrid);
    EXPECT_EQ(noSize->data.filename, "Recording 0001.mp4");
    EXPECT_EQ(noSize->data.expectedSize, unsized.size());
    EXPECT_EQ(noSize->recordedSize, 0u);
    EXPECT_EQ(noSize->data.sourceOffset(), builder.clusterOffset(first.clusters.front()));
    EXPECT_EQ(noSize->data.sourceRegions.size(), 1u);
    expectRecovered(source, noSize, unsized);
    EXPECT_EQ(noSize->warnings, std::vector<Mp4Warning>{Mp4Warning::SizeMismatch});

    const Mp4Candidate* shortSize = recovered.at("/Recording 0002.mp4");
    ASSERT_NE(shortSize, nullptr);
    EXPECT_EQ(shortSize->data.method, RecoveryMethod::Hybrid);
    EXPECT_EQ(shortSize->recordedSize, half);
    EXPECT_EQ(shortSize->data.expectedSize, grown.size());
    // The metadata's layout was a guess of the wrong length; the carve's is a guess the structure confirms.
    EXPECT_TRUE(shortSize->data.hasWarning(CandidateWarning::LayoutGuessed));
    expectRecovered(source, shortSize, grown);
    EXPECT_EQ(shortSize->warnings, std::vector<Mp4Warning>{Mp4Warning::SizeMismatch});

    const Mp4Candidate* partly = recovered.at("/Recording 0003.mp4");
    ASSERT_NE(partly, nullptr);
    SCOPED_TRACE(describeMp4(*partly));
    EXPECT_EQ(partly->data.method, RecoveryMethod::Hybrid);
    EXPECT_EQ(partly->data.expectedSize, reused.bytes.size());
    // The carve's run, split where NEW.BIN's clusters are.
    ASSERT_EQ(partly->data.sourceRegions.size(), 3u);
    EXPECT_EQ(partly->data.sourceRegions[1], (SourceRegion{6 * kClusterSize, 2 * kClusterSize, RegionKind::Stored,
                                                           builder.clusterOffset(taken), true}));
    EXPECT_TRUE(partly->data.hasWarning(CandidateWarning::ClustersReallocated));
    const std::vector<std::uint64_t> touched = samplesTouching(reused, 6 * kClusterSize, 8 * kClusterSize);
    ASSERT_EQ(partly->structure.tracks.size(), 2u);
    for (std::size_t t = 0; t < 2; ++t) {
        EXPECT_EQ(partly->structure.tracks[t].samplesReallocated, touched[t]);
        EXPECT_EQ(partly->structure.tracks[t].samplesIntact, reused.samples[t].size() - touched[t]);
    }
    EXPECT_TRUE(partly->hasWarning(Mp4Warning::SamplesDamaged));

    EXPECT_EQ(recovered.report.hybrid, 3u);
    EXPECT_EQ(recovered.report.filesystem, 0u);
    EXPECT_EQ(recovered.report.carving, 0u);
    EXPECT_EQ(recovered.report.carvesMerged, 3u);
}

TEST(Mp4RecoveryTest, AFirstClusterTakenByAnotherFileConfirmsNothing) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    // A recording that never got its size; a new video took its clusters, from the first on.
    const auto recording =
        builder.addFileInClusters(root, "Recording 0009.mp4", test::makePattern(6 * kClusterSize, 201),
                                  run<std::uint32_t>(3000, 6));
    builder.deleteEntry(recording);
    storeLe32(builder.shortEntry(recording), 28, 0);
    const Bytes clip = test::makeMp4(seeded(202)).bytes;
    (void)builder.addFileInClusters(root, "CLIP0009.MP4", clip, run<std::uint32_t>(3000, clustersFor(clip.size())));
    // A deleted video whose clusters a new video of the same size took: the guess reads a valid MP4, the new one's.
    const Bytes newer = test::makeMp4(seeded(203)).bytes;
    const std::vector<std::uint32_t> clusters = run<std::uint32_t>(4000, clustersFor(newer.size()));
    builder.deleteEntry(builder.addFileInClusters(root, "Beach video 03.mp4", test::makePattern(newer.size(), 204),
                                                  clusters));
    (void)builder.addFileInClusters(root, "CLIP0010.MP4", newer, clusters);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});

    // The new files, each with its carve.
    expectRecovered(source, recovered.at("/CLIP0009.MP4"), clip);
    expectRecovered(source, recovered.at("/CLIP0010.MP4"), newer);
    // What starts at the deleted entries' first cluster is the new owners' file: it confirms nothing about them.
    EXPECT_EQ(recovered.at("/Recording 0009.mp4"), nullptr);  // no data of its own left to recover
    const Mp4Candidate* reused = recovered.at("/Beach video 03.mp4");
    ASSERT_NE(reused, nullptr);
    SCOPED_TRACE(describeMp4(*reused));
    EXPECT_EQ(reused->data.method, RecoveryMethod::Filesystem);
    EXPECT_TRUE(reused->data.hasWarning(CandidateWarning::ClustersReallocated));
    EXPECT_EQ(reused->structure.status, mp4::FileStatus::Valid);
    std::uint64_t reallocated = 0;
    for (const Mp4TrackEvidence& track : reused->structure.tracks) {
        reallocated += track.samplesReallocated;
    }
    EXPECT_GT(reallocated, 0u);
    EXPECT_EQ(reallocated, reused->structure.samples());
    EXPECT_TRUE(reused->hasWarning(Mp4Warning::SamplesDamaged));
    ASSERT_TRUE(reused->carving.has_value());

    EXPECT_EQ(recovered.candidates.size(), 3u);
    EXPECT_EQ(recovered.report.hybrid, 0u);
    EXPECT_EQ(recovered.report.carving, 0u);
    EXPECT_EQ(recovered.report.carvesMerged, 2u);
}

// ===========================================================================
// Damage
// ===========================================================================

TEST(Mp4RecoveryTest, PartlyOverwrittenFilesShowWhatWasLost) {
    test::NtfsImageBuilder builder;
    // A new file takes the lowest free record: sacrificial records keep the deleted files' own.
    std::vector<test::NtfsImageBuilder::Entry> sacrificial;
    for (std::uint64_t i = 0; i < 3; ++i) {
        sacrificial.push_back(builder.addFile(kNtfsRoot, "tmp" + std::to_string(i) + ".bin", test::makePattern(10, i)));
    }
    const Mp4Built reused = test::makeMp4(seeded(51));
    const Mp4Built lostMoov = test::makeMp4(seeded(52));
    const Mp4Built lostStart = test::makeMp4(seeded(53));
    const auto a = builder.addFileInClusters(kNtfsRoot, "reused.mp4", reused.bytes,
                                             run<std::uint64_t>(7000, clustersFor(reused.bytes.size())));
    const auto b = builder.addFileInClusters(kNtfsRoot, "lost moov.mp4", lostMoov.bytes,
                                             run<std::uint64_t>(7100, clustersFor(lostMoov.bytes.size())));
    const auto c = builder.addFileInClusters(kNtfsRoot, "lost start.mp4", lostStart.bytes,
                                             run<std::uint64_t>(7200, clustersFor(lostStart.bytes.size())));
    for (const auto& entry : sacrificial) {
        builder.deleteEntry(entry);
    }
    builder.deleteEntry(a);
    builder.deleteEntry(b);
    builder.deleteEntry(c);
    // Files written since take some of their clusters: two in the media data, every cluster of moov, the first two.
    (void)builder.addFileInClusters(kNtfsRoot, "later1.bin", test::makePattern(2 * kClusterSize, 54), {7004, 7005});
    const test::BoxPosition moov = test::findBox(test::mp4Boxes(lostMoov.bytes), "moov");
    const auto moovCluster = static_cast<std::uint32_t>(moov.offset / kClusterSize);
    const std::vector<std::uint64_t> tail =
        run<std::uint64_t>(7100 + moovCluster, clustersFor(lostMoov.bytes.size()) - moovCluster);
    (void)builder.addFileInClusters(kNtfsRoot, "later2.bin", test::makePattern(tail.size() * kClusterSize, 55), tail);
    (void)builder.addFileInClusters(kNtfsRoot, "later3.bin", test::makePattern(2 * kClusterSize, 56), {7200, 7201});
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});

    // The samples in the clusters taken are counted; the others are intact.
    const Mp4Candidate* partly = recovered.at("/reused.mp4");
    ASSERT_NE(partly, nullptr);
    {
        SCOPED_TRACE(describeMp4(*partly));
        EXPECT_EQ(partly->data.method, RecoveryMethod::Filesystem);
        EXPECT_EQ(partly->structure.status, mp4::FileStatus::Valid);
        const std::vector<std::uint64_t> touched = samplesTouching(reused, 4 * kClusterSize, 6 * kClusterSize);
        EXPECT_GT(touched[0] + touched[1], 0u);
        ASSERT_EQ(partly->structure.tracks.size(), 2u);
        for (std::size_t t = 0; t < 2; ++t) {
            EXPECT_EQ(partly->structure.tracks[t].samplesReallocated, touched[t]);
            EXPECT_EQ(partly->structure.tracks[t].samplesIntact, reused.samples[t].size() - touched[t]);
        }
        EXPECT_LE(partly->structure.tracks[0].samplesMisframed, touched[0]);
        EXPECT_TRUE(partly->hasWarning(Mp4Warning::SamplesDamaged));
        // Carving reads the same clusters: its carve is attached, not a second candidate.
        ASSERT_TRUE(partly->carving.has_value());
        EXPECT_EQ(partly->carving->sourceOffset, builder.clusterOffset(7000));
    }

    // moov overwritten: nothing locates the samples any more.
    const Mp4Candidate* noMovie = recovered.at("/lost moov.mp4");
    ASSERT_NE(noMovie, nullptr);
    {
        SCOPED_TRACE(describeMp4(*noMovie));
        EXPECT_EQ(noMovie->data.method, RecoveryMethod::Filesystem);
        EXPECT_NE(noMovie->structure.status, mp4::FileStatus::Valid);
        EXPECT_TRUE(noMovie->hasWarning(Mp4Warning::NoMovie));
        EXPECT_TRUE(noMovie->structure.tracks.empty());
        EXPECT_EQ(noMovie->structure.mediaDataBoxes, 1u);
        EXPECT_TRUE(noMovie->data.hasWarning(CandidateWarning::ClustersReallocated));
    }

    // The start overwritten: moov is found by searching, and the samples at the start are counted.
    const Mp4Candidate* noStart = recovered.at("/lost start.mp4");
    ASSERT_NE(noStart, nullptr);
    {
        SCOPED_TRACE(describeMp4(*noStart));
        EXPECT_EQ(noStart->data.method, RecoveryMethod::Filesystem);
        EXPECT_TRUE(noStart->hasWarning(Mp4Warning::MovieFoundBySearch));
        EXPECT_TRUE(noStart->hasWarning(Mp4Warning::StructureInvalid));
        EXPECT_EQ(noStart->structure.moovOffset, test::findBox(test::mp4Boxes(lostStart.bytes), "moov").offset);
        EXPECT_FALSE(noStart->structure.majorBrand.has_value());
        EXPECT_EQ(noStart->structure.kind, mp4::MediaKind::Video);
        const std::vector<std::uint64_t> lost = samplesTouching(lostStart, 0, 2 * kClusterSize);
        ASSERT_EQ(noStart->structure.tracks.size(), 2u);
        for (std::size_t t = 0; t < 2; ++t) {
            EXPECT_EQ(noStart->structure.tracks[t].samplesReallocated, lost[t]);
            EXPECT_EQ(noStart->structure.tracks[t].samplesIntact, lostStart.samples[t].size() - lost[t]);
        }
        // No ftyp is left for carving to find.
        EXPECT_FALSE(noStart->carving.has_value());
    }

    EXPECT_EQ(recovered.report.filesystem, 3u);
    EXPECT_EQ(recovered.report.carving, 0u);
    EXPECT_EQ(recovered.report.valid, 1u);
}

TEST(Mp4RecoveryTest, DamagedMetadataIsReported) {
    const Mp4Built built = test::makeMp4(seeded(61));
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(built.bytes);
    const std::string stbl = "moov/trak/mdia/minf/stbl/";
    // The video track's sample sizes lost.
    Bytes noSizes = built.bytes;
    std::memcpy(noSizes.data() + test::findBox(boxes, stbl + "stsz").offset + 4, "xxxx", 4);
    // The first video chunk beyond the end of the file.
    Bytes beyond = built.bytes;
    storeBe32(beyond, test::findBox(boxes, stbl + "stco").offset + 16, static_cast<std::uint32_t>(beyond.size()));
    // An mvhd of a version that does not exist.
    Bytes version = built.bytes;
    version[test::findBox(boxes, "moov/mvhd").offset + 8] = std::byte{2};
    test::Fat32ImageBuilder builder;
    const auto root = test::Fat32ImageBuilder::root();
    (void)builder.addFile(root, "no sizes.mp4", noSizes);
    (void)builder.addFile(root, "beyond.mp4", beyond);
    (void)builder.addFile(root, "version.mp4", version);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});
    // Each is a candidate: its name says MP4, its structure says what is wrong.
    ASSERT_EQ(recovered.candidates.size(), 3u);

    for (const std::string_view path : {"/no sizes.mp4", "/version.mp4"}) {
        const Mp4Candidate* candidate = recovered.at(path);
        ASSERT_NE(candidate, nullptr) << path;
        SCOPED_TRACE(describeMp4(*candidate));
        EXPECT_EQ(candidate->data.method, RecoveryMethod::Filesystem);
        EXPECT_EQ(candidate->structure.status, mp4::FileStatus::Invalid);
        EXPECT_TRUE(candidate->hasWarning(Mp4Warning::StructureInvalid));
        ASSERT_FALSE(candidate->structure.issues.empty());
        EXPECT_GE(candidate->structure.issueCount, candidate->structure.issues.size());
        EXPECT_FALSE(candidate->structure.issues.front().detail.empty());
        EXPECT_FALSE(candidate->structure.intact());
    }

    const Mp4Candidate* far = recovered.at("/beyond.mp4");
    ASSERT_NE(far, nullptr);
    SCOPED_TRACE(describeMp4(*far));
    EXPECT_EQ(far->structure.status, mp4::FileStatus::Truncated);
    EXPECT_EQ(far->structure.tracks[0].samplesBeyondData, built.chunks[0][0].sampleCount);
    EXPECT_EQ(far->structure.tracks[1].samplesBeyondData, 0u);
    // The structure needs data past the recorded size.
    EXPECT_GT(far->structure.structureEnd, built.bytes.size());
    EXPECT_TRUE(far->hasWarning(Mp4Warning::StructureTruncated));
    EXPECT_TRUE(far->hasWarning(Mp4Warning::SamplesDamaged));
    EXPECT_TRUE(far->hasWarning(Mp4Warning::SizeMismatch));

    EXPECT_EQ(recovered.report.invalid, 2u);
    EXPECT_EQ(recovered.report.truncated, 1u);
    EXPECT_EQ(recovered.report.carving, 0u);
}

TEST(Mp4RecoveryTest, BadSectorsMakeSamplesUnreadable) {
    test::Fat32ImageBuilder builder;
    const Mp4Built built = test::makeMp4(seeded(71));
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "CLIP0001.MP4", built.bytes);
    // The sector holding the start of a video sample, whose NAL unit header the analysis reads.
    const std::uint64_t sector = built.samples[0][5].offset / 512 * 512;
    test::MemoryStorageSource source(builder.build());
    source.addBadSector((builder.clusterOffset(entry.clusters.front()) + sector) / 512);
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});

    const Mp4Candidate* candidate = recovered.at("/CLIP0001.MP4");
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeMp4(*candidate));
    EXPECT_EQ(candidate->data.method, RecoveryMethod::Filesystem);
    EXPECT_EQ(candidate->structure.status, mp4::FileStatus::Valid);  // the boxes are elsewhere
    const std::vector<std::uint64_t> touched = samplesTouching(built, sector, sector + 512);
    ASSERT_EQ(candidate->structure.tracks.size(), 2u);
    for (std::size_t t = 0; t < 2; ++t) {
        EXPECT_EQ(candidate->structure.tracks[t].samplesUnreadable, touched[t]);
        EXPECT_EQ(candidate->structure.tracks[t].samplesIntact, built.samples[t].size() - touched[t]);
    }
    EXPECT_TRUE(candidate->hasWarning(Mp4Warning::SamplesDamaged));
    // The sample reads as zeros: its first NAL unit has length 0.
    EXPECT_GE(candidate->structure.tracks[0].samplesMisframed, 1u);

    // The recovered file has zeros there, and its report says so.
    const test::Reconstructed rebuilt = test::reconstructToMemory(source, candidate->data);
    ASSERT_TRUE(rebuilt.ok);
    EXPECT_EQ(rebuilt.report.unreadableBytes, 512u);
    Bytes expected = built.bytes;
    std::fill(expected.begin() + static_cast<std::ptrdiff_t>(sector),
              expected.begin() + static_cast<std::ptrdiff_t>(sector + 512), std::byte{0});
    EXPECT_TRUE(rebuilt.data == expected);
}

// ===========================================================================
// CARVING candidates
// ===========================================================================

TEST(Mp4RecoveryTest, CarvedFilesCarryWhereTheyLieInTheAllocation) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    // In free clusters, with no entry left: files deleted long ago.
    const Bytes free = test::makeMp4(seeded(81)).bytes;
    plant(builder, 4000, free);
    const Bytes movie = test::makeMp4(quickTime(82)).bytes;
    plant(builder, 4200, movie);
    // Two of its clusters taken by a file written since.
    const Mp4Built taken = test::makeMp4(moovFirst(83));
    plant(builder, 4400, taken.bytes);
    (void)builder.addFileInClusters(root, "NEW.BIN", test::makePattern(2 * kClusterSize, 84), {4406, 4407});
    // Inside an active file: the video of a motion photo, after the picture.
    const Bytes jpeg = test::makeJpeg();
    const Bytes video = test::makeMp4(seeded(85)).bytes;
    const auto photo = builder.addFile(root, "MVIMG_0001.JPG", concat(jpeg, video));
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    Mp4RecoveryOptions options;
    options.firstId = 41;
    const Recovered recovered = recover(source, {&volume}, options);
    ASSERT_EQ(recovered.candidates.size(), 4u);
    EXPECT_EQ(recovered.report.carving, 4u);
    EXPECT_EQ(recovered.report.filesystemExamined, 0u);

    // In source order: the motion photo's clusters come first.
    const Mp4Candidate* motion = recovered.carvedAt(builder.clusterOffset(photo.clusters.front()) + jpeg.size());
    ASSERT_NE(motion, nullptr);
    {
        SCOPED_TRACE(describeMp4(*motion));
        EXPECT_EQ(motion->data.filename, "recovered_000041.mp4");
        EXPECT_EQ(motion->warnings, std::vector<Mp4Warning>{Mp4Warning::InsideActiveFile});
        ASSERT_TRUE(motion->allocation.has_value());
        EXPECT_TRUE(motion->allocation->insideActiveFile);
        EXPECT_EQ(motion->allocation->activeFiles, std::vector<std::string>{"/MVIMG_0001.JPG"});
        EXPECT_EQ(motion->allocation->freeClusters, 0u);
        EXPECT_GT(motion->allocation->allocatedClusters, 0u);
        // Its clusters are the photo's, and so are its bytes: nothing is reallocated.
        EXPECT_EQ(motion->data.sourceRegions.size(), 1u);
        expectRecovered(source, motion, video);
    }

    const Mp4Candidate* unallocated = recovered.carvedAt(builder.clusterOffset(4000));
    ASSERT_NE(unallocated, nullptr);
    {
        SCOPED_TRACE(describeMp4(*unallocated));
        EXPECT_EQ(unallocated->data.filename, "recovered_000042.mp4");
        EXPECT_EQ(unallocated->data.extension, "mp4");
        EXPECT_TRUE(unallocated->warnings.empty());
        ASSERT_TRUE(unallocated->allocation.has_value());
        EXPECT_EQ(unallocated->allocation->filesystem, filesystem::FilesystemType::Fat32);
        EXPECT_EQ(unallocated->allocation->volumeOffset, 0u);
        EXPECT_EQ(unallocated->allocation->clusters, clustersFor(free.size()));
        EXPECT_EQ(unallocated->allocation->freeClusters, clustersFor(free.size()));
        EXPECT_TRUE(unallocated->allocation->activeFiles.empty());
        EXPECT_TRUE(unallocated->allocation->complete);
        EXPECT_TRUE(unallocated->data.filesystemEvidence.path.empty());
        expectRecovered(source, unallocated, free);
    }

    const Mp4Candidate* quicktime = recovered.carvedAt(builder.clusterOffset(4200));
    ASSERT_NE(quicktime, nullptr);
    EXPECT_EQ(quicktime->data.filename, "recovered_000043.mov");
    expectRecovered(source, quicktime, movie);

    const Mp4Candidate* partly = recovered.carvedAt(builder.clusterOffset(4400));
    ASSERT_NE(partly, nullptr);
    {
        SCOPED_TRACE(describeMp4(*partly));
        EXPECT_EQ(partly->data.filename, "recovered_000044.mp4");
        EXPECT_EQ(partly->data.expectedSize, taken.bytes.size());
        ASSERT_TRUE(partly->allocation.has_value());
        EXPECT_EQ(partly->allocation->allocatedClusters, 2u);
        EXPECT_EQ(partly->allocation->freeClusters, clustersFor(taken.bytes.size()) - 2);
        EXPECT_EQ(partly->allocation->activeFiles, std::vector<std::string>{"/NEW.BIN"});
        EXPECT_FALSE(partly->allocation->insideActiveFile);
        EXPECT_TRUE(partly->hasWarning(Mp4Warning::AllocatedClusters));
        ASSERT_EQ(partly->data.sourceRegions.size(), 3u);
        EXPECT_EQ(partly->data.sourceRegions[1], (SourceRegion{6 * kClusterSize, 2 * kClusterSize,
                                                               RegionKind::Stored, builder.clusterOffset(4406), true}));
        const std::vector<std::uint64_t> touched = samplesTouching(taken, 6 * kClusterSize, 8 * kClusterSize);
        for (std::size_t t = 0; t < 2; ++t) {
            EXPECT_EQ(partly->structure.tracks[t].samplesReallocated, touched[t]);
        }
        EXPECT_TRUE(partly->hasWarning(Mp4Warning::SamplesDamaged));
    }
}

TEST(Mp4RecoveryTest, FilesAreExaminedByTheirNameOrByTheirContent) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    const Bytes video = test::makeMp4(seeded(91)).bytes;
    (void)builder.addFile(root, "VIDEO.BIN", video);
    // A generic brand with sound only: an audio file, which M4A recovery takes.
    Mp4Options sound = seeded(92);
    sound.tracks = {Mp4TrackOptions{Mp4TrackKind::Audio}};
    (void)builder.addFile(root, "SOUND.BIN", test::makeMp4(sound).bytes);
    (void)builder.addFile(root, "SONG.M4A", test::makeM4a());
    (void)builder.addFile(root, "PHOTO.JPG", test::makeJpeg());
    // Named MP4, holding something else.
    (void)builder.addFile(root, "clip.mp4", test::makeJpeg());
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});

    EXPECT_EQ(recovered.report.filesystemExamined, 3u);  // VIDEO.BIN, SOUND.BIN, clip.mp4
    EXPECT_EQ(recovered.report.filesystemMp4, 2u);
    ASSERT_EQ(recovered.candidates.size(), 2u);
    const Mp4Candidate* byContent = recovered.at("/VIDEO.BIN");
    ASSERT_NE(byContent, nullptr);
    EXPECT_EQ(byContent->data.method, RecoveryMethod::Filesystem);
    expectRecovered(source, byContent, video);
    const Mp4Candidate* byName = recovered.at("/clip.mp4");
    ASSERT_NE(byName, nullptr);
    EXPECT_TRUE(byName->hasWarning(Mp4Warning::NoMovie)) << describeMp4(*byName);
    EXPECT_TRUE(byName->hasWarning(Mp4Warning::StructureInvalid));
    EXPECT_FALSE(byName->carving.has_value());
    // Neither audio file is carved as video.
    EXPECT_EQ(recovered.report.carving, 0u);
    EXPECT_EQ(recovered.report.carved, 1u);
    EXPECT_EQ(recovered.report.carvesMerged, 1u);
}

TEST(Mp4RecoveryTest, FilesystemEvidenceAndCarvingCanBeUsedAlone) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const Bytes clip = test::makeMp4(seeded(101)).bytes;
    const auto entry = builder.addFile(test::Fat32ImageBuilder::root(), "CLIP0001.MP4", clip);
    const Bytes orphan = test::makeMp4(moovFirst(102)).bytes;
    plant(builder, 5000, orphan);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);

    Mp4RecoveryOptions filesystemOnly;
    filesystemOnly.carve = false;
    const Recovered metadata = recover(source, {&volume}, filesystemOnly);
    ASSERT_EQ(metadata.candidates.size(), 1u);
    EXPECT_EQ(metadata.candidates[0].data.method, RecoveryMethod::Filesystem);
    EXPECT_FALSE(metadata.candidates[0].carving.has_value());
    EXPECT_EQ(metadata.report.scan.bytesRead, 0u);
    EXPECT_EQ(metadata.report.carved, 0u);

    Mp4RecoveryOptions carvingOnly;
    carvingOnly.useFilesystem = false;
    const Recovered carved = recover(source, {&volume}, carvingOnly);
    ASSERT_EQ(carved.candidates.size(), 2u);
    EXPECT_EQ(carved.report.filesystemExamined, 0u);
    // The active file, carved: it lies in that file's data, which says what it is.
    const Mp4Candidate* active = carved.carvedAt(builder.clusterOffset(entry.clusters.front()));
    ASSERT_NE(active, nullptr);
    ASSERT_TRUE(active->allocation.has_value());
    EXPECT_EQ(active->allocation->activeFiles, std::vector<std::string>{"/CLIP0001.MP4"});
    EXPECT_TRUE(active->hasWarning(Mp4Warning::InsideActiveFile));
    expectRecovered(source, active, clip);
    expectRecovered(source, carved.carvedAt(builder.clusterOffset(5000)), orphan);
}

// ===========================================================================
// Output, disks and large sources
// ===========================================================================

TEST(Mp4RecoveryTest, RecoveredFilesAreWrittenWholeAndValidateAgain) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    const Bytes active = test::makeMp4(seeded(111)).bytes;
    (void)builder.addFile(builder.addDirectory(root, "DCIM").clusters.front(), "CLIP0001.MP4", active);
    const Bytes unsized = test::makeMp4(moovFirst(112)).bytes;
    const auto entry = builder.addFile(root, "Recording.mp4", unsized);
    builder.deleteEntry(entry);
    storeLe32(builder.shortEntry(entry), 28, 0);
    const Bytes orphan = test::makeMp4(seeded(113)).bytes;
    plant(builder, 5000, orphan);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered recovered = recover(source, {&volume});
    ASSERT_EQ(recovered.candidates.size(), 3u);
    EXPECT_EQ(recovered.report.filesystem, 1u);
    EXPECT_EQ(recovered.report.hybrid, 1u);
    EXPECT_EQ(recovered.report.carving, 1u);

    const test::TempDir directory;
    Result<RecoveryWriter> writer = RecoveryWriter::create(source, directory / "out");
    RECOVERY_ASSERT_OK(writer);
    for (const Mp4Candidate& candidate : recovered.candidates) {
        const Result<RecoveredFile> file = writer->recover(candidate.data);
        RECOVERY_ASSERT_OK(file);
        EXPECT_EQ(file->candidate, candidate.data.id);
        EXPECT_TRUE(file->report.allBytesRead());
    }
    const std::filesystem::path out = directory / "out";
    const std::vector<std::pair<std::filesystem::path, const Bytes*>> expected = {
        {out / "DCIM" / "CLIP0001.MP4", &active},
        {out / "Recording.mp4", &unsized},
        {out / carvedName(3, "mp4"), &orphan}};
    for (const auto& [path, original] : expected) {
        SCOPED_TRACE(path.string());
        ASSERT_TRUE(std::filesystem::exists(path));
        const Bytes bytes = test::readFile(path);
        EXPECT_TRUE(bytes == *original);
        // The written file, on its own, is a valid MP4 file.
        carving::MemoryContentReader content(bytes);
        const Result<Mp4Structure> structure = analyzeMp4(content);
        RECOVERY_ASSERT_OK(structure);
        EXPECT_TRUE(structure->intact()) << structure->detail;
        EXPECT_EQ(structure->samplesIntact(), structure->samples());
        EXPECT_GT(structure->samples(), 0u);
    }
}

TEST(Mp4RecoveryTest, VolumesOfAPartitionedDiskUseDiskOffsets) {
    test::Fat32ImageBuilder fat32;
    const auto root = test::Fat32ImageBuilder::root();
    const Bytes fatClip = test::makeMp4(seeded(121)).bytes;
    (void)fat32.addFile(root, "CLIP0001.MP4", fatClip);
    const Bytes fatDeleted = test::makeMp4(moovFirst(122)).bytes;
    fat32.deleteEntry(fat32.addFile(root, "Deleted clip.mp4", fatDeleted));
    const std::vector<std::byte> fatVolume = fat32.build();

    test::NtfsImageBuilder ntfs;
    const Bytes ntfsMovie = test::makeMp4(seeded(123)).bytes;
    std::vector<std::uint64_t> runs;
    for (std::uint32_t i = 0; i < clustersFor(ntfsMovie.size()); ++i) {
        runs.push_back(i < 8 ? 5000 + i : 5200 + i);
    }
    ntfs.deleteEntry(ntfs.addFileInClusters(kNtfsRoot, "movie.mp4", ntfsMovie, runs));
    std::vector<std::byte> ntfsVolume = ntfs.build();
    const Bytes carvedMovie = test::makeMp4(seeded(124)).bytes;
    std::copy(carvedMovie.begin(), carvedMovie.end(),
              ntfsVolume.begin() + static_cast<std::ptrdiff_t>(ntfs.clusterOffset(6000)));

    constexpr std::uint32_t kFirst = 2048;
    const auto fatSectors = static_cast<std::uint32_t>(fatVolume.size() / 512);
    const std::uint32_t second = kFirst + fatSectors + 2048;
    const auto ntfsSectors = static_cast<std::uint32_t>(ntfsVolume.size() / 512);
    std::vector<std::byte> disk((static_cast<std::size_t>(second) + ntfsSectors + 64) * 512);
    std::copy(fatVolume.begin(), fatVolume.end(), disk.begin() + std::size_t{kFirst} * 512);
    std::copy(ntfsVolume.begin(), ntfsVolume.end(), disk.begin() + std::size_t{second} * 512);
    test::writeMbrSector(disk, 512, 0, {{0x00, 0x0C, kFirst, fatSectors}, {0x00, 0x07, second, ntfsSectors}});

    test::MemoryStorageSource device(disk, 512);
    RECOVERY_ASSERT_OK(device.open());
    const Result<partition::PartitionTable> table = partition::readPartitionTable(device);
    RECOVERY_ASSERT_OK(table);
    ASSERT_EQ(table->partitions.size(), 2u);
    std::vector<std::unique_ptr<partition::PartitionSource>> partitions;
    std::vector<Volume> volumes;
    volumes.reserve(2);
    for (const partition::Partition& p : table->partitions) {
        partitions.push_back(std::make_unique<partition::PartitionSource>(device, p));
        RECOVERY_ASSERT_OK(partitions.back()->open());
        volumes.push_back(openVolume(*partitions.back(), p.offset));
        ASSERT_NE(volumes.back().recovery, nullptr);
    }
    const Recovered recovered = recover(device, {&volumes[0], &volumes[1]});
    ASSERT_EQ(recovered.candidates.size(), 4u);

    const std::uint64_t fatOffset = std::uint64_t{kFirst} * 512;
    const std::uint64_t ntfsOffset = std::uint64_t{second} * 512;
    const Mp4Candidate* clip = recovered.at("/CLIP0001.MP4");
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->data.method, RecoveryMethod::Filesystem);
    EXPECT_GE(clip->data.sourceOffset().value_or(0), fatOffset);
    expectRecovered(device, clip, fatClip);
    const Mp4Candidate* deletedClip = recovered.at("/Deleted clip.mp4");
    ASSERT_NE(deletedClip, nullptr);
    EXPECT_EQ(deletedClip->data.method, RecoveryMethod::Hybrid);
    expectRecovered(device, deletedClip, fatDeleted);
    const Mp4Candidate* movie = recovered.at("/movie.mp4");
    ASSERT_NE(movie, nullptr);
    EXPECT_EQ(movie->data.method, RecoveryMethod::Filesystem);
    EXPECT_EQ(movie->data.filesystemEvidence.volumeOffset, ntfsOffset);
    expectRecovered(device, movie, ntfsMovie);
    // Carved in the NTFS volume's free space: disk offsets, and the NTFS volume's allocation.
    const Mp4Candidate* carved = recovered.carvedAt(ntfsOffset + ntfs.clusterOffset(6000));
    ASSERT_NE(carved, nullptr);
    ASSERT_TRUE(carved->allocation.has_value());
    EXPECT_EQ(carved->allocation->filesystem, filesystem::FilesystemType::Ntfs);
    EXPECT_EQ(carved->allocation->volumeOffset, ntfsOffset);
    EXPECT_EQ(carved->allocation->freeClusters, clustersFor(carvedMovie.size()));
    expectRecovered(device, carved, carvedMovie);
}

TEST(Mp4RecoveryTest, AFileLargerThan4GiBWhoseRunsTheFilesystemRecorded) {
    Mp4Options options = moovFirst(131);
    options.largeMediaData = true;
    options.co64 = true;
    const Mp4Built built = test::makeMp4(options);
    const test::SpreadMp4 spread = test::spreadMp4(built, 5 * kGiB);
    test::Fat32ImageBuilder builder;
    const std::uint64_t head = builder.clusterOffset(1000);
    const std::uint64_t gap = 6 * kGiB;
    const std::uint64_t media = 12 * kGiB;
    test::VirtualSource source(media + spread.media.size() + kMiB);
    source.plant(0, builder.build());
    source.plant(head, spread.head);
    source.plant(media, spread.media);
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    // What the run list of a file this size gives: the head in the volume, the rest in two runs beyond 4 GiB.
    RecoveryCandidate big;
    big.id = CandidateId{500};
    big.filename = "big.mp4";
    big.extension = "mp4";
    big.expectedSize = spread.size();
    big.sourceRegions = {SourceRegion{0, spread.head.size(), RegionKind::Stored, head, false},
                         SourceRegion{spread.head.size(), spread.gap, RegionKind::Stored, gap, false},
                         SourceRegion{spread.mediaOffset(), spread.media.size(), RegionKind::Stored, media, false}};
    big.fragmentation = FragmentationInfo{3, true};
    big.filesystemEvidence.path = "/big.mp4";
    big.filesystemEvidence.allocation.layout = LayoutEvidence::Recorded;
    volume.scan.candidates.push_back(big);

    Mp4RecoveryOptions recoveryOptions;
    recoveryOptions.carving.scan.endOffset = 4 * kMiB;  // the volume, not the whole source
    source.resetStats();
    const Recovered recovered = recover(source, {&volume}, recoveryOptions);
    const Mp4Candidate* candidate = recovered.at("/big.mp4");
    ASSERT_NE(candidate, nullptr);
    SCOPED_TRACE(describeMp4(*candidate));
    EXPECT_EQ(candidate->data.method, RecoveryMethod::Filesystem);
    EXPECT_EQ(candidate->data.expectedSize, spread.size());
    EXPECT_GT(candidate->data.expectedSize, 4 * kGiB);
    EXPECT_TRUE(candidate->structure.intact());
    EXPECT_EQ(candidate->structure.samplesIntact(), sampleCount(built));
    EXPECT_EQ(candidate->structure.structureEnd, spread.size());
    EXPECT_EQ(candidate->structure.tracks[0].samplesFramed, built.samples[0].size());
    // Carving from the head reads on contiguously, and finds other data where the samples should be.
    ASSERT_TRUE(candidate->carving.has_value());
    EXPECT_NE(candidate->carving->validation.status, ValidationStatus::Valid);
    EXPECT_EQ(recovered.report.carving, 0u);
    // The boxes and the samples were read, not the gap.
    EXPECT_LT(source.stats().bytes, 64 * kMiB);
}

TEST(Mp4RecoveryTest, AFileLargerThan4GiBIsCarvedBeyond4GiB) {
    Mp4Options options = moovFirst(141);
    options.largeMediaData = true;
    options.co64 = true;
    options.largeMoov = true;
    const Mp4Built built = test::makeMp4(options);
    const test::SpreadMp4 spread = test::spreadMp4(built, 5 * kGiB);
    const std::uint64_t base = 4 * kGiB + 777 * 512;
    test::VirtualSource source(base + spread.size() + kMiB);
    source.setNoise(5);
    source.plant(base, spread.head);
    source.plant(base + spread.mediaOffset(), spread.media);
    RECOVERY_ASSERT_OK(source.open());
    Mp4RecoveryOptions recoveryOptions;
    recoveryOptions.carving.scan.startOffset = base - kMiB;
    recoveryOptions.carving.scan.endOffset = base + kMiB;
    source.resetStats();
    const Recovered recovered = recover(source, {}, recoveryOptions);
    ASSERT_EQ(recovered.candidates.size(), 1u);
    const Mp4Candidate& candidate = recovered.candidates.front();
    SCOPED_TRACE(describeMp4(candidate));
    EXPECT_EQ(candidate.data.method, RecoveryMethod::Carving);
    EXPECT_EQ(candidate.data.filename, "recovered_000001.mp4");
    EXPECT_EQ(candidate.data.sourceOffset(), base);
    EXPECT_EQ(candidate.data.expectedSize, spread.size());
    EXPECT_FALSE(candidate.allocation.has_value());  // no volume holds it
    EXPECT_TRUE(candidate.structure.intact());
    EXPECT_EQ(candidate.structure.samplesIntact(), sampleCount(built));
    EXPECT_TRUE(candidate.warnings.empty());
    EXPECT_EQ(candidate.carving->validation.status, ValidationStatus::Valid);
    EXPECT_LT(source.stats().bytes, 64 * kMiB);
    // The samples, read through the candidate, are the originals.
    Result<std::unique_ptr<CandidateContentReader>> content = CandidateContentReader::open(source, candidate.data);
    RECOVERY_ASSERT_OK(content);
    for (std::size_t t = 0; t < spread.samples.size(); ++t) {
        for (std::size_t s = 0; s < spread.samples[t].size(); s += 3) {
            const test::Mp4Sample& original = built.samples[t][s];
            const Result<std::span<const std::byte>> bytes =
                (*content)->read(spread.samples[t][s].offset, spread.samples[t][s].size);
            RECOVERY_ASSERT_OK(bytes);
            EXPECT_TRUE(std::equal(bytes->begin(), bytes->end(),
                                   built.bytes.begin() + static_cast<std::ptrdiff_t>(original.offset)));
        }
    }
}

// ===========================================================================
// Cancellation, errors, invalid use, logging
// ===========================================================================

TEST(Mp4RecoveryTest, CancellationStopsTheRun) {
    test::Fat32ImageBuilder builder;
    (void)builder.addFile(test::Fat32ImageBuilder::root(), "CLIP0001.MP4", test::makeMp4(seeded(151)).bytes);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    std::size_t delivered = 0;
    const Mp4CandidateSink count = [&](Mp4Candidate&&) -> Status {
        ++delivered;
        return success();
    };
    CancellationSource cancel;
    cancel.requestCancellation();
    Mp4RecoveryOptions options;
    options.carving.scan.reads.cancellation = cancel.token();
    Mp4Recovery recovery(source, options);
    RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
    RECOVERY_EXPECT_ERROR(recovery.run(count), ErrorCode::Cancelled);

    // During the scan of a large source.
    test::VirtualSource large(256 * kMiB);
    large.setNoise(3);
    large.plant(200 * kMiB, test::makeMp4(seeded(152)).bytes);
    CancellationSource later;
    large.setReadHook([&](std::uint64_t offset, std::size_t) {
        if (offset >= 64 * kMiB) {
            later.requestCancellation();
        }
    });
    RECOVERY_ASSERT_OK(large.open());
    Mp4RecoveryOptions scanOptions;
    scanOptions.carving.scan.reads.cancellation = later.token();
    Mp4Recovery scanning(large, scanOptions);
    RECOVERY_EXPECT_ERROR(scanning.run(count), ErrorCode::Cancelled);
    EXPECT_LT(large.stats().highestEnd, 128 * kMiB);
    EXPECT_EQ(delivered, 0u);
}

TEST(Mp4RecoveryTest, ASinkErrorStopsTheRun) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    for (std::uint64_t i = 0; i < 3; ++i) {
        (void)builder.addFile(test::Fat32ImageBuilder::root(), "CLIP000" + std::to_string(i) + ".MP4",
                              test::makeMp4(seeded(160 + i)).bytes);
    }
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    Mp4Recovery recovery(source);
    RECOVERY_ASSERT_OK(recovery.addVolume(*volume.recovery, volume.scan));
    int calls = 0;
    const Result<Mp4RecoveryReport> result = recovery.run([&](Mp4Candidate&&) -> Status {
        if (++calls == 2) {
            return makeError(ErrorCode::DestinationError, "destination full");
        }
        return success();
    });
    RECOVERY_EXPECT_ERROR(result, ErrorCode::DestinationError);
    EXPECT_EQ(calls, 2);
}

TEST(Mp4RecoveryTest, InvalidUseIsRefused) {
    const Mp4CandidateSink sink = [](Mp4Candidate&&) { return success(); };
    test::MemoryStorageSource closed(Bytes(64 * kKiB), 512);
    RECOVERY_EXPECT_ERROR(Mp4Recovery(closed).run(sink), ErrorCode::InvalidInput);

    test::MemoryStorageSource source(Bytes(64 * kKiB), 512);
    RECOVERY_ASSERT_OK(source.open());
    RECOVERY_EXPECT_ERROR(Mp4Recovery(source).run({}), ErrorCode::InvalidInput);
    const auto refused = [&](Mp4RecoveryOptions options, std::string_view what) {
        SCOPED_TRACE(what);
        RECOVERY_EXPECT_ERROR(Mp4Recovery(source, std::move(options)).run(sink), ErrorCode::InvalidInput);
    };
    Mp4RecoveryOptions options;
    options.carving.readCacheSize = 16;
    refused(options, "a cache too small");
    options = {};
    options.carving.readCacheSize = carving::IContentReader::kMaxReadLength + 1;
    refused(options, "a cache too large");
    options = {};
    options.carving.scan.reads.sectorRetryCount = carving::SourceReadOptions::kMaxSectorRetries + 1;
    refused(options, "too many retries");
    options = {};
    options.format.limits.maxTracks = 0;
    refused(options, "a limit of 0");
    options = {};
    options.maxClusterChecks = 0;
    refused(options, "no cluster checks");

    // A scan of another volume.
    test::Fat32ImageBuilder builder;
    test::MemoryStorageSource image(builder.build());
    RECOVERY_ASSERT_OK(image.open());
    Volume volume = openVolume(image);
    ASSERT_NE(volume.recovery, nullptr);
    CandidateScan other = volume.scan;
    other.volumeOffset = 1 * kMiB;
    Mp4Recovery recovery(image);
    RECOVERY_EXPECT_ERROR(recovery.addVolume(*volume.recovery, other), ErrorCode::InvalidInput);
}

TEST(Mp4RecoveryTest, LogsTheRun) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    (void)builder.addFile(test::Fat32ImageBuilder::root(), "CLIP0001.MP4", test::makeMp4(seeded(171)).bytes);
    plant(builder, 5000, test::makeMp4(seeded(172)).bytes);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(diagnostics::LogLevel::Debug);
    logger.addSink(sink);
    Mp4RecoveryOptions options;
    options.carving.scan.logger = &logger;
    const Recovered recovered = recover(source, {&volume}, options);
    ASSERT_EQ(recovered.candidates.size(), 2u);

    const auto records = sink->records();
    const auto named = [&](std::string_view message) {
        return std::count_if(records.begin(), records.end(), [&](const auto& r) {
            return r.component == "mp4_recovery" && r.message == message;
        });
    };
    EXPECT_EQ(named("MP4 recovery started"), 1);
    EXPECT_EQ(named("MP4 candidate"), 2);
    const auto ended = std::find_if(records.begin(), records.end(),
                                    [](const auto& r) { return r.message == "MP4 recovery ended"; });
    ASSERT_NE(ended, records.end());
    const auto has = [&](std::string_view key, std::string_view value) {
        return std::any_of(ended->fields.begin(), ended->fields.end(),
                           [&](const auto& f) { return f.key == key && f.value == value; });
    };
    EXPECT_TRUE(has("filesystem", "1"));
    EXPECT_TRUE(has("carving", "1"));
    EXPECT_TRUE(has("merged", "1"));
    EXPECT_TRUE(has("valid", "2"));
}


// ===========================================================================
// Recovery in steps (P15)
// ===========================================================================

// The steps deliver what run() delivers, with the candidates examined in any
// order, the hits prepared ahead on threads of their own, and the state saved
// half-way through the hits and restored into new steps that go on.
TEST(Mp4RecoveryTest, TheStepsDeliverWhatRunDelivers) {
    test::Fat32BuilderOptions geometry;
    geometry.clusterCount = 8192;
    test::Fat32ImageBuilder builder(geometry);
    const auto root = test::Fat32ImageBuilder::root();
    (void)builder.addFile(builder.addDirectory(root, "DCIM").clusters.front(), "CLIP0001.MP4",
                          test::makeMp4(seeded(121)).bytes);
    const auto entry = builder.addFile(root, "Recording.mp4", test::makeMp4(moovFirst(122)).bytes);
    builder.deleteEntry(entry);
    storeLe32(builder.shortEntry(entry), 28, 0);
    plant(builder, 5000, test::makeMp4(seeded(123)).bytes);
    plant(builder, 6000, test::makeMp4(quickTime(124)).bytes);
    test::MemoryStorageSource source(builder.build());
    RECOVERY_ASSERT_OK(source.open());
    Volume volume = openVolume(source);
    ASSERT_NE(volume.recovery, nullptr);
    const Recovered expected = recover(source, {&volume});
    ASSERT_GE(expected.candidates.size(), 4u);

    const auto make = [&] {
        Result<std::unique_ptr<Mp4RecoverySteps>> steps = Mp4RecoverySteps::create(source, {});
        EXPECT_TRUE(steps.ok());
        EXPECT_TRUE((*steps)->addVolume(*volume.recovery, volume.scan).ok());
        return std::move(steps).value();
    };
    std::unique_ptr<Mp4RecoverySteps> first = make();
    // Step 1: examined from the last candidate to the first, added in scan order.
    std::vector<Mp4Examination> examinations(volume.scan.candidates.size());
    for (std::size_t i = examinations.size(); i-- > 0;) {
        Result<Mp4Examination> examined = first->examine(0, i);
        RECOVERY_ASSERT_OK(examined);
        examinations[i] = std::move(*examined);
    }
    for (Mp4Examination& examination : examinations) {
        RECOVERY_ASSERT_OK(first->addExamination(std::move(examination)));
    }
    RECOVERY_EXPECT_ERROR(first->addExamination(Mp4Examination{}), ErrorCode::InvalidInput);

    // Step 2: the hits, prepared on threads of their own, then committed in order.
    carving::FormatRegistry registry;
    RECOVERY_ASSERT_OK(registry.add(std::make_shared<formats::Mp4Format>()));
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry);
    RECOVERY_ASSERT_OK(scanner);
    std::vector<carving::SignatureHit> hits;
    RECOVERY_ASSERT_OK(scanner->scan(source, [&](const carving::SignatureHit& hit) {
        hits.push_back(hit);
        return success();
    }));
    ASSERT_GE(hits.size(), 4u);
    std::vector<std::optional<Result<Mp4HitWork>>> works(hits.size());
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        threads.emplace_back([&, i] { works[i].emplace(first->prepare(hits[i])); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::size_t half = hits.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
        RECOVERY_ASSERT_OK(*works[i]);
        RECOVERY_ASSERT_OK(first->commit(hits[i], &works[i]->value()));
    }
    // Saved half-way and restored: new steps go on (some hits without prepared work).
    std::unique_ptr<Mp4RecoverySteps> second = make();
    RECOVERY_ASSERT_OK(second->restore(first->state()));
    for (std::size_t i = half; i < hits.size(); ++i) {
        RECOVERY_ASSERT_OK(*works[i]);
        RECOVERY_ASSERT_OK(second->commit(hits[i], i % 2 == 0 ? &works[i]->value() : nullptr));
    }
    std::vector<Mp4Candidate> delivered;
    Mp4RecoveryReport report;
    RECOVERY_ASSERT_OK(second->deliver(
        [&](Mp4Candidate&& candidate) {
            delivered.push_back(std::move(candidate));
            return success();
        },
        report));
    ASSERT_EQ(delivered.size(), expected.candidates.size());
    for (std::size_t i = 0; i < delivered.size(); ++i) {
        EXPECT_EQ(describeMp4(delivered[i]), describeMp4(expected.candidates[i]));
        ASSERT_EQ(delivered[i].carving.has_value(), expected.candidates[i].carving.has_value());
        if (delivered[i].carving.has_value()) {
            EXPECT_EQ(delivered[i].carving->id, expected.candidates[i].carving->id);
        }
    }
    EXPECT_EQ(report.carved, expected.report.carved);
    EXPECT_EQ(report.carvesMerged, expected.report.carvesMerged);
    EXPECT_EQ(report.carvesRejected, expected.report.carvesRejected);
    EXPECT_EQ(report.hitsSkipped, expected.report.hitsSkipped);
    EXPECT_EQ(report.filesystemExamined, expected.report.filesystemExamined);
    EXPECT_EQ(report.filesystemMp4, expected.report.filesystemMp4);
    // Spent once delivered.
    RECOVERY_EXPECT_ERROR(second->deliver([](Mp4Candidate&&) { return success(); }, report),
                          ErrorCode::InvalidInput);
}

}  // namespace
}  // namespace recovery
