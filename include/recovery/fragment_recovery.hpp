#pragma once

// Fragment reconstruction (P13): files whose start is known but whose layout
// no metadata records, put back together from the evidence around them, one
// layout hypothesis at a time, each validated on exactly the bytes it would
// recover.
//
//   seeds ──► layout hypotheses ──► independent validation ──► ranking ──► status
//
// Seeds:
//  * deleted FAT32 and exFAT files whose metadata keeps only the first
//    cluster and the size (LayoutEvidence::Guessed, P7): FAT32 frees the
//    chain on deletion, exFAT may clear it;
//  * carved files whose structure breaks inside a volume's free space (P8):
//    their start is known, their layout and often their length are not.
//
// Evidence:
//  * the volume's allocation now: clusters allocated to other data cannot hold
//    a deleted file's data any more (or may not: see L35);
//  * other files' claims: the data of active files, the recorded layouts and
//    first clusters of deleted files, carves that validated, and the
//    reconstructions settled earlier in the run;
//  * cluster adjacency: a file's writer continues on the next clusters it
//    finds free, so a fragment usually follows its predecessor, wrapping
//    around at the end of the volume;
//  * the file's metadata: its start and size;
//  * its format's structure: the format's own validator, and where a layout
//    stops being consistent (the next fragment starts at or before that
//    point);
//  * for MP4, the sample tables: where every sample lies, and whether the
//    NAL units of each video sample fill it; a moov that the file's top-level
//    boxes put at the end of the file is searched for where they put it.
//
// Hypotheses are never a blind concatenation of clusters: the clusters after
// the start (P7's guess); those not allocated now; those not claimed either;
// continuations found where the structure breaks; for MP4, clusters placed
// sample by sample. Each goes through the format's validator on its own.
//
// Ranking (best first): the structure (Valid first; then a structure that
// does not end before the recorded size, more bytes the evidence confirms,
// Truncated before Invalid), fewer clusters allocated to other data now, fewer
// clusters claimed by other files, fewer fragments. Layouts equal in all of
// that whose bytes differ are ambiguous. A layout that does not validate
// delivers only what the evidence confirms (the part before its structure
// broke; a continuation counts once it held for minimumContinuation bytes).
//
// Status (first match wins): AMBIGUOUS, UNRECOVERABLE, PARTIAL, CORRUPTED,
// COMPLETE; see ReconstructionStatus. The source is only read. See
// docs/recovery/fragment_recovery.md.

#include "carving/file_candidate.hpp"
#include "carving/file_carver.hpp"
#include "carving/format_registry.hpp"
#include "carving/format_validator.hpp"
#include "carving/signature_scanner.hpp"
#include "filesystem/filesystem.hpp"
#include "formats/mp4_format.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/mp4_recovery.hpp"
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
// Reconstructions
// ---------------------------------------------------------------------------

enum class ReconstructionStatus : std::uint8_t {
    // One layout is best; it validates as Valid; every byte of the file is
    // placed and was read; none lies in clusters allocated to other data now.
    Complete,
    // Only part of the file could be placed: the reconstruction holds what
    // validates up to where the next fragment was not found, and the rest is
    // Missing (zeros inside the file, left out at its end).
    Partial,
    // The whole file is placed, but some of it is damaged: bytes in clusters
    // allocated to other data now (they may have been overwritten), bytes
    // that could not be read, or content the validator rejects between placed
    // parts (MP4 samples whose NAL units do not fill them while later samples
    // do).
    Corrupted,
    // Two or more layouts whose bytes differ are equally supported by all the
    // evidence: all of them are reported, none is chosen.
    Ambiguous,
    // Nothing of the file validates: its first cluster holds other data (it
    // is allocated to another file now, or its content is not this format),
    // or no data of it is left.
    Unrecoverable,
};

[[nodiscard]] std::string_view toString(ReconstructionStatus status) noexcept;

enum class SeedOrigin : std::uint8_t {
    // A deleted file whose metadata gives its first cluster and size but not
    // its layout (LayoutEvidence::Guessed).
    Filesystem,
    // A carved file whose structure breaks: no metadata names it.
    Carving,
};

[[nodiscard]] std::string_view toString(SeedOrigin origin) noexcept;

// How a hypothesis's layout was put together.
enum class LayoutSource : std::uint8_t {
    // The clusters after the first, in order (what P7 guesses).
    Contiguous,
    // The clusters after the first that are not allocated to other data now:
    // a file written around files that still exist.
    SkipAllocated,
    // The clusters after the first that are neither allocated now nor claimed
    // by other files' evidence.
    SkipClaimed,
    // A continuation found where the structure of the file stopped being
    // consistent.
    GapSearch,
    // MP4: clusters placed sample by sample from the sample tables (the NAL
    // units of the video samples), the moov read where the file's top-level
    // boxes put it.
    SampleTables,
};

[[nodiscard]] std::string_view toString(LayoutSource source) noexcept;

// Clusters as the volume numbers them (FilesystemInfo::firstCluster on).
struct ClusterRun {
    std::uint64_t firstCluster = 0;
    std::uint64_t count = 0;

    friend bool operator==(const ClusterRun&, const ClusterRun&) = default;
};

// What a layout rests on, counted over the clusters holding the part of the
// file the reconstruction delivers.
struct HypothesisEvidence {
    // Physically separate pieces.
    std::size_t fragments = 0;
    // Clusters placed; those allocated to other data now (or bad), which may
    // have been overwritten; and those claimed by other files' evidence.
    std::uint64_t clusters = 0;
    std::uint64_t allocatedClusters = 0;
    std::uint64_t claimedClusters = 0;
    // File bytes placed on the source, and bytes the reconstruction could not
    // place (Missing).
    std::uint64_t placedBytes = 0;
    std::uint64_t missingBytes = 0;
    // Placed bytes that could not be read while validating (bad sectors,
    // beyond the end of the source): zeros in the reconstruction.
    std::uint64_t unreadableBytes = 0;
    // Valid layouts: the format's validator checks where the pieces join. It
    // rejects the layout when the first cluster of any fragment after the
    // first (in one piece, the middle cluster) holds other bytes, every offset
    // unchanged. False when the data there has no structure the validator
    // checks (BMP pixels, WAV samples, audio in MP4, a cluster inside a NAL
    // unit): the layout then rests on the allocation evidence alone. True for
    // files of one or two clusters.
    bool dataChecked = false;
};

struct ReconstructionHypothesis {
    // The reconstruction as a recovery candidate, which reconstructCandidate()
    // and RecoveryWriter take as it is. method is Fragmented when the layout
    // has several fragments; in one piece it is Hybrid for a filesystem seed
    // (as in P12) and Carving for a carve. A filesystem seed's name, path,
    // times and filesystem evidence are kept; a carve's name is
    // "recovered_<id>.<ext>". fragmentation.known is false: the layout is
    // inferred, not recorded.
    RecoveryCandidate data;
    // The placed clusters, in file order.
    std::vector<ClusterRun> clusters;
    LayoutSource source = LayoutSource::Contiguous;
    // The format's own validator on exactly the bytes `data` delivers.
    carving::ValidationResult validation;
    HypothesisEvidence evidence;
    // MP4: the structure and sample evidence of this layout (P12's analysis,
    // with the samples counted against the layout's damage).
    std::optional<Mp4Structure> mp4;
};

struct SearchStats {
    // Layouts validated in full, and MP4 windows of samples checked.
    std::uint64_t layoutsValidated = 0;
    std::uint64_t sampleProbes = 0;
    // Source bytes read by the search and the validations.
    std::uint64_t bytesRead = 0;
    // False when a limit (FragmentSearchLimits) stopped the search: a layout
    // it did not reach might fit better.
    bool complete = true;
    // Which limit, when one stopped it.
    std::string limit;
};

// One seed, reconstructed.
struct FragmentCandidate {
    // From FragmentRecoveryOptions::firstId, in delivery order.
    CandidateId id{0};
    ReconstructionStatus status = ReconstructionStatus::Unrecoverable;
    // Why this status (never file content).
    std::string reason;
    SeedOrigin origin = SeedOrigin::Filesystem;
    // The format the file was reconstructed as (FormatDescriptor::id).
    std::string formatId;
    // The file's name: its metadata's (UTF-8, as recorded), or
    // "recovered_<id>.<ext>" for a carve; and its path, when the metadata
    // records one.
    std::string name;
    std::string path;
    // Filesystem seeds: the P7 candidate (its id in its CandidateScan) and the
    // size its metadata records.
    std::optional<CandidateId> filesystemCandidate;
    std::optional<std::uint64_t> recordedSize;
    // The carve that starts where the file starts: a carve seed's own, or the
    // carve of a filesystem seed's first cluster when carving found one.
    std::optional<carving::FileCandidate> carve;
    // The volume the file lies on.
    filesystem::FilesystemType filesystem = filesystem::FilesystemType::Fat32;
    std::uint64_t volumeOffset = 0;
    std::uint32_t clusterSize = 0;
    // Best first. Complete, Corrupted and Partial: the first is the
    // reconstruction, the others the alternatives that lost. Ambiguous: the
    // first `tied` are equally supported. Unrecoverable: what was tried, if
    // anything. At most FragmentSearchLimits::maxAlternatives.
    std::vector<ReconstructionHypothesis> hypotheses;
    std::size_t tied = 0;
    SearchStats search;

    // The chosen reconstruction; nullptr when Ambiguous or Unrecoverable.
    [[nodiscard]] const ReconstructionHypothesis* reconstruction() const noexcept;
};

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

// Bounds on the work of one seed's search. A search that reaches one stops
// with the hypotheses it has (SearchStats::complete is false).
struct FragmentSearchLimits {
    // Fragments of one layout.
    std::size_t maxFragments = 16;
    // Where a structure breaks, the cluster boundaries tried as the end of the
    // fragment (the nearest first), and the clusters tried as the start of the
    // next fragment for each boundary.
    std::size_t maxBoundaries = 4;
    std::size_t maxContinuations = 64;
    // Clusters looked at, from a fragment's end on (wrapping around at the end
    // of the volume), for the start of the next fragment.
    std::uint64_t maxSearchClusters = std::uint64_t{1} << 20;
    // A continuation found where a structure broke counts as found once its
    // structure holds for this many bytes past its start (or the file
    // validates): foreign data can look consistent for a while.
    std::uint64_t minimumContinuation = 4096;
    // Partial layouts carried from one fragment to the next.
    std::size_t beamWidth = 8;
    // Layouts validated in full per seed, and source bytes read per seed.
    std::size_t maxValidations = 2048;
    std::uint64_t maxReadBytes = std::uint64_t{4} << 30;
    // Carve seeds (the length is unknown until the structure ends): the
    // longest file a layout may grow to.
    std::uint64_t maxUnknownLength = std::uint64_t{256} << 20;
    // MP4: moov boxes tried where the top-level boxes put the movie, video
    // samples checked per window, and windows checked per seed.
    std::size_t maxMovieAnchors = 8;
    std::size_t probeSamples = 16;
    std::size_t maxSampleProbes = std::size_t{1} << 20;
    // Hypotheses delivered per seed.
    std::size_t maxAlternatives = 8;
};

// InvalidInput when a limit is 0.
[[nodiscard]] Status validate(const FragmentSearchLimits& limits);

struct FragmentRecoveryOptions {
    // Id of the first candidate; the others follow in delivery order.
    std::uint64_t firstId = 1;
    // Reconstruct the deleted files of the added volumes whose layout the
    // metadata only guesses.
    bool useFilesystem = true;
    // Carve the source for broken files to reconstruct (and for the evidence
    // of the files that carve whole).
    bool carve = true;
    // The scan (range, block size, hit limit), and the reads (cancellation,
    // known bad regions, retries), cache size and logger every part of the
    // recovery uses. Its validate and skip settings are not used.
    carving::CarveOptions carving;
    // Parse limits for MP4 and M4A files (the moov search and the sample
    // tables).
    formats::Mp4FormatOptions mp4;
    FragmentSearchLimits limits;
};

struct FragmentRecoveryReport {
    // Deleted files with a guessed layout that were reconstructed, and those
    // of no registered format (not reconstructed).
    std::uint64_t filesystemSeeds = 0;
    std::uint64_t filesystemSkipped = 0;
    // Carving: the scan, the hits carved (cluster-aligned, in free clusters),
    // the carves that became seeds, and the carves that validated whole (the
    // evidence of other files).
    carving::ScanReport scan;
    std::uint64_t carved = 0;
    std::uint64_t carvingSeeds = 0;
    std::uint64_t carvesValid = 0;
    // Candidates delivered by status.
    std::uint64_t complete = 0;
    std::uint64_t partial = 0;
    std::uint64_t corrupted = 0;
    std::uint64_t ambiguous = 0;
    std::uint64_t unrecoverable = 0;
    // Searches that a limit stopped.
    std::uint64_t searchesLimited = 0;
    std::uint64_t layoutsValidated = 0;
    std::chrono::milliseconds elapsed{0};

    [[nodiscard]] std::uint64_t candidates() const noexcept {
        return complete + partial + corrupted + ambiguous + unrecoverable;
    }
};

// Receives the candidates: filesystem seeds first (volume by volume, in scan
// order), then carve seeds (in source order). An error stops the run and is
// returned by it (Cancelled too).
using FragmentCandidateSink = std::function<Status(FragmentCandidate&& candidate)>;

// Reconstructs fragmented files of a source (a disk or an image, or a
// partition of one), with the filesystem candidates of its volumes and the
// formats of `formats` (images, audio, video: every format that can validate
// a file).
//
// Memory: the carve seeds and the claims of other files are kept until the
// end of the run; each seed's hypotheses until it is delivered; every layout
// is read through bounded caches.
//
// Thread safety: none; one owner at a time. The source, the registry, the
// volumes and their scans must outlive the object.
class FragmentRecovery {
public:
    FragmentRecovery(storage::IStorageSource& source, const carving::FormatRegistry& formats,
                     FragmentRecoveryOptions options = {});

    // Adds a volume of the source: the filesystem recovery that scanned it
    // (for its allocation) and the candidates it found. Fails with
    // InvalidInput for a scan from another volume, or a volume without
    // clusters.
    [[nodiscard]] Status addVolume(FilesystemRecovery& volume, const CandidateScan& scan);

    // Finds the seeds, carves the source, reconstructs every seed. Fails with
    // InvalidInput for invalid options, a source that is not open or a
    // registry without formats, with Cancelled, with the error of a read that
    // fails for a reason other than an I/O error, and with the sink's error.
    [[nodiscard]] Result<FragmentRecoveryReport> run(const FragmentCandidateSink& sink);

    [[nodiscard]] const FragmentRecoveryOptions& options() const noexcept { return options_; }

private:
    struct Volume {
        FilesystemRecovery* recovery;
        const CandidateScan* scan;
    };

    storage::IStorageSource* source_;
    const carving::FormatRegistry* formats_;
    FragmentRecoveryOptions options_;
    std::vector<Volume> volumes_;
};

}  // namespace recovery
