#pragma once

// MP4 recovery (P12): MP4 videos (MP4, MOV, M4V, 3GP), deleted or not, from
// filesystem evidence and carving evidence together.
//
//   filesystem candidates (P7) ──► MP4 by name or content? ──► structure ──┐
//                                                                          ├─► Mp4Candidate: FILESYSTEM,
//   the source ──► carving with the MP4 format ──► carves ──► same start? ─┘   HYBRID or CARVING
//
// FILESYSTEM: the metadata locates the data (a recorded chain, run list or
// contiguous run) and the MP4 structure of that data is the evidence of what
// it holds. A carve of the same file is attached to it, not listed again.
// HYBRID: the metadata names the file and says where it starts, and the MP4
// structure gives or confirms its layout and length: a deleted file whose
// guessed contiguous layout validates, or a carve that starts where a
// filesystem candidate starts (its first cluster) and validates where the
// metadata's own layout does not, or where the metadata locates no data. A
// deleted entry whose first cluster another file has taken since is never
// confirmed: the structure that starts there is the new owner's.
// CARVING: a carved MP4 no metadata names, with where it lies relative to the
// volumes' allocation (free or allocated clusters, inside an active file).
//
// Every candidate carries the structure of its data (the parser of P11 and
// the analysis of formats/mp4_analysis.hpp: ftyp, moov and mdat discovery,
// movie fragments, sample tables, sample framing) and the sample-table
// analysis against its source regions: which samples lie in data that is
// missing, unreadable, or in clusters allocated again since the deletion.
// The candidate's data is a RecoveryCandidate, so reconstructCandidate() and
// RecoveryWriter take it as it is. P14 folds this into the unified
// candidate model; P13 reconstructs the files whose layout the evidence here
// does not settle.
//
// The source is only read. See docs/recovery/mp4_recovery.md.

#include "carving/file_candidate.hpp"
#include "carving/file_carver.hpp"
#include "carving/signature_scanner.hpp"
#include "filesystem/filesystem.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_format.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/recovery_candidate.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recovery {

// ---------------------------------------------------------------------------
// Structure evidence
// ---------------------------------------------------------------------------

// A track of the movie, and what the sample-table analysis found for its
// samples. A sample counts once, under the first of: beyond the data,
// missing, unreadable, reallocated, intact.
struct Mp4TrackEvidence {
    // 1-based, in moov order.
    std::uint32_t number = 0;
    formats::mp4::TrackKind kind = formats::mp4::TrackKind::Other;
    // The first sample description's format ('avc1', 'hvc1', 'mp4a', ...).
    formats::mp4::FourCc codec;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    // The media's duration in its time scale's units, and the time scale.
    std::uint64_t duration = 0;
    std::uint32_t timescale = 0;
    // Samples located (sample tables and movie fragments), and their bytes.
    std::uint64_t samples = 0;
    std::uint64_t sampleBytes = 0;
    // Samples ending beyond the file's data.
    std::uint64_t samplesBeyondData = 0;
    // Samples with bytes the file's layout does not locate (Missing), or
    // knows to be zeros that were never written (Zeros).
    std::uint64_t samplesMissing = 0;
    // Samples with bytes that could not be read (bad sectors, a truncated
    // image), as far as the analysis read them: it reads the boxes and the
    // NAL unit headers of AVC and HEVC samples, each with the cache block
    // around it, not every byte of every sample (RecoveryWriter's report
    // counts every unreadable byte of the file it writes).
    std::uint64_t samplesUnreadable = 0;
    // Deleted files: samples with bytes in clusters allocated to other data
    // since the deletion. They may have been overwritten.
    std::uint64_t samplesReallocated = 0;
    // Samples with every byte located, none found unreadable, and none in
    // clusters allocated again.
    std::uint64_t samplesIntact = 0;
    // AVC and HEVC tracks: samples whose NAL units were walked, and those
    // they do not fill (mp4::checkSampleFraming).
    std::uint64_t samplesFramed = 0;
    std::uint64_t samplesMisframed = 0;
};

struct Mp4Structure {
    // The parser's verdict on the file's data (parseFile): Valid, Truncated
    // or Invalid. A file whose moov was only found by searching is Invalid.
    formats::mp4::FileStatus status = formats::mp4::FileStatus::Invalid;
    // Why, or a summary (never file content).
    std::string detail;
    // The first issues the parser recorded, and how many there were.
    std::vector<formats::mp4::Issue> issues;
    std::uint64_t issueCount = 0;
    // Audio, video or neither (mp4::classify).
    formats::mp4::MediaKind kind = formats::mp4::MediaKind::Neither;
    std::string kindReason;
    std::optional<formats::mp4::FourCc> majorBrand;
    // The movie (moov), in file offsets, and whether it was only found by
    // searching the data (moov discovery: the top-level boxes before it are
    // damaged or overwritten).
    std::optional<std::uint64_t> moovOffset;
    std::uint64_t moovSize = 0;
    bool moovFoundBySearch = false;
    bool moovBeforeMediaData = false;
    // mdat boxes found by the top-level boxes, and movie fragments (moof).
    std::size_t mediaDataBoxes = 0;
    std::size_t movieFragments = 0;
    // Where the sample tables and fragments put the media data (mdat
    // discovery): file offsets, whatever the mdat boxes say.
    std::optional<formats::mp4::MediaExtent> media;
    // Where the file's structure ends: the end of its last top-level box, or
    // of its media data when that is further.
    std::uint64_t structureEnd = 0;
    // The file's data: its length, as analysed.
    std::uint64_t dataSize = 0;
    std::vector<Mp4TrackEvidence> tracks;

    [[nodiscard]] std::uint64_t samples() const noexcept;
    [[nodiscard]] std::uint64_t samplesIntact() const noexcept;
    [[nodiscard]] std::uint64_t samplesMisframed() const noexcept;
    // Samples beyond the data, missing, unreadable or reallocated.
    [[nodiscard]] std::uint64_t samplesDamaged() const noexcept;
    // Valid, and every sample intact and framed as it should be.
    [[nodiscard]] bool intact() const noexcept;
};

// How a carved file lies in the volumes' allocation.
struct Mp4Allocation {
    // The volume holding the file's first byte.
    filesystem::FilesystemType filesystem = filesystem::FilesystemType::Fat32;
    std::uint64_t volumeOffset = 0;
    // The volume's clusters the file covers, by their state now. Bytes
    // outside the volume's cluster area are not counted.
    std::uint64_t clusters = 0;
    std::uint64_t freeClusters = 0;
    std::uint64_t allocatedClusters = 0;
    // Bad, invalid or unreadable in the allocation table or bitmap.
    std::uint64_t otherClusters = 0;
    // False when only the first Mp4RecoveryOptions::maxClusterChecks were checked.
    bool complete = true;
    // Paths of active files whose data overlaps the file (the first eight).
    std::vector<std::string> activeFiles;
    // The file lies entirely inside one active file's data (an embedded
    // video, such as the one at the end of a motion photo).
    bool insideActiveFile = false;
};

enum class Mp4Warning : std::uint8_t {
    // The structure is Invalid (see Mp4Structure::detail and issues).
    StructureInvalid,
    // The data ends before the structure does (Truncated).
    StructureTruncated,
    // No moov was found, not even by searching: the samples cannot be
    // located, and the file cannot be played as it is.
    NoMovie,
    // moov was found only by searching: the boxes before it are damaged.
    MovieFoundBySearch,
    // Some samples are beyond the data, missing, unreadable or reallocated.
    SamplesDamaged,
    // The NAL units of some AVC or HEVC samples do not fill them: their
    // bytes are not the samples the tables describe.
    SamplesMisframed,
    // Filesystem and hybrid candidates: the structure ends before or after
    // the size the metadata records (Mp4Candidate::recordedSize).
    SizeMismatch,
    // Carving: some of the file's clusters are allocated to other data now.
    AllocatedClusters,
    // Carving: the file lies inside an active file's data (instead of
    // AllocatedClusters: its clusters are that file's).
    InsideActiveFile,
};

[[nodiscard]] std::string_view toString(Mp4Warning warning) noexcept;

// An MP4 recovery candidate.
struct Mp4Candidate {
    // The file's data, as a recovery candidate: data.method is Filesystem,
    // Carving or Hybrid; data.id is this candidate's id. Filesystem and
    // hybrid candidates keep the name, path, times and evidence of the
    // filesystem candidate; carving candidates have empty filesystem evidence
    // and the name "recovered_<id>.<ext>" (mp4, mov, m4v, 3gp or 3g2 after
    // the brand). reconstructCandidate() and RecoveryWriter take it as it is.
    //
    // A layout taken from a carve (carving candidates, and hybrid ones whose
    // metadata's layout did not validate) is one run from the carve's start
    // to its end: contiguity is assumed, so data.fragmentation.known is
    // false. Its bytes in clusters allocated now (outside the active file
    // the carve lies in, if any) are marked reallocated, with the warning
    // ClustersReallocated. The metadata's LayoutGuessed warning stays: the
    // layout is still a guess, which the structure confirms.
    RecoveryCandidate data;
    // Filesystem and hybrid candidates: the filesystem candidate's id in its
    // CandidateScan, and the file size its metadata records (data.expectedSize
    // of a hybrid candidate is the carve's length instead).
    std::optional<CandidateId> filesystemCandidate;
    std::optional<std::uint64_t> recordedSize;
    // The carve of the same file: carving and hybrid candidates, and
    // filesystem candidates that carving found too.
    std::optional<carving::FileCandidate> carving;
    Mp4Structure structure;
    // Carving candidates on an added volume.
    std::optional<Mp4Allocation> allocation;
    std::vector<Mp4Warning> warnings;

    [[nodiscard]] bool hasWarning(Mp4Warning warning) const noexcept;
};

// ---------------------------------------------------------------------------
// Analysis of one file's data
// ---------------------------------------------------------------------------

// The structure of an MP4 file's content: the parse (with its movie
// fragments), what kind of file it is, moov discovery when the top-level
// boxes do not lead to a moov, the media extent, and the framing of its AVC
// and HEVC samples (unless format.checkSampleFraming is off). The content is
// taken as it is: each sample is beyond the data or intact (missing,
// unreadable and reallocated bytes need the file's layout, Mp4Recovery).
// Fails with InvalidInput for invalid limits, and with the reader's errors.
[[nodiscard]] Result<Mp4Structure> analyzeMp4(carving::IContentReader& content,
                                              const formats::Mp4FormatOptions& format = {});

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

struct Mp4RecoveryOptions {
    // Id of the first candidate; the others follow in the order they are
    // delivered.
    std::uint64_t firstId = 1;
    // Examine the filesystem candidates of the added volumes.
    bool useFilesystem = true;
    // Carve the source with the MP4 format.
    bool carve = true;
    // Carving: the scan's range, block size, alignment and hit limit, and the
    // reads (cancellation, known bad regions, retries) and logger that every
    // part of the recovery uses. Its validate and skip settings are not used:
    // the recovery validates each carve itself, and skips hits inside carves
    // that validated.
    carving::CarveOptions carving;
    // Parse limits, and whether sample framing is checked.
    formats::Mp4FormatOptions format;
    // Clusters checked against a volume's allocation per carved file, at most.
    std::uint64_t maxClusterChecks = std::uint64_t{1} << 22;
};

struct Mp4RecoveryReport {
    // Filesystem candidates whose name or content made them worth examining,
    // and those of them that are MP4 video files (not audio or images).
    std::uint64_t filesystemExamined = 0;
    std::uint64_t filesystemMp4 = 0;
    // Carving: the scan, the hits carved and their outcomes.
    carving::ScanReport scan;
    std::uint64_t carved = 0;
    std::uint64_t carvesRejected = 0;
    std::uint64_t hitsSkipped = 0;
    // Carves attached to a filesystem or hybrid candidate of the same file.
    std::uint64_t carvesMerged = 0;
    // Candidates delivered, by method and by structure.
    std::uint64_t filesystem = 0;
    std::uint64_t carving = 0;
    std::uint64_t hybrid = 0;
    std::uint64_t valid = 0;
    std::uint64_t truncated = 0;
    std::uint64_t invalid = 0;
    std::chrono::milliseconds elapsed{0};

    [[nodiscard]] std::uint64_t candidates() const noexcept { return filesystem + carving + hybrid; }
};

// Receives the candidates: filesystem and hybrid ones first (volume by
// volume, in scan order), then carving ones (in source order). An error
// stops the run and is returned by it (Cancelled too).
using Mp4CandidateSink = std::function<Status(Mp4Candidate&& candidate)>;

// Recovers MP4 files from a source (a disk or an image, or a partition of
// one), with the filesystem candidates of its volumes.
//
// Memory: the MP4 candidates are kept until the end of the run (carves are
// merged into filesystem candidates found earlier); every file's data is
// read through bounded caches.
//
// Thread safety: none; one owner at a time. The source, the volumes and
// their scans must outlive the object.
class Mp4Recovery {
public:
    explicit Mp4Recovery(storage::IStorageSource& source, Mp4RecoveryOptions options = {});

    // Adds a volume of the source: the filesystem recovery that scanned it
    // (for its filesystem's allocation) and the candidates it found. Fails
    // with InvalidInput for a scan from another volume.
    [[nodiscard]] Status addVolume(FilesystemRecovery& volume, const CandidateScan& scan);

    // Examines the volumes' candidates and carves the source. Fails with
    // InvalidInput for invalid options or a source that is not open, with
    // Cancelled, with the error of a read that fails for a reason other than
    // an I/O error, and with the sink's error.
    [[nodiscard]] Result<Mp4RecoveryReport> run(const Mp4CandidateSink& sink);

    [[nodiscard]] const Mp4RecoveryOptions& options() const noexcept { return options_; }

private:
    struct Volume {
        FilesystemRecovery* recovery;
        const CandidateScan* scan;
    };

    storage::IStorageSource* source_;
    Mp4RecoveryOptions options_;
    std::vector<Volume> volumes_;
};

// The extension of a carved file with this major brand: "mov" for
// QuickTime, "m4v", "3gp" and "3g2" for theirs, "mp4" otherwise.
[[nodiscard]] std::string_view mp4Extension(std::optional<formats::mp4::FourCc> majorBrand) noexcept;

}  // namespace recovery
