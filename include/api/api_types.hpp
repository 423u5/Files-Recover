#pragma once

// The value types of the GUI-facing API (P19): sources, scans, candidates,
// recovery, sessions, progress and events, as plain data that a user
// interface shows and passes back.
//
// Nothing here is an engine type. No filesystem structure, cluster, MFT
// record, partition-table entry or carving detail reaches a user interface:
// the API converts the engine's types at its boundary, and a user interface
// built on these headers needs no knowledge of FAT32, exFAT or NTFS. These
// headers include the standard library and the engine's structured errors
// (recovery/error.hpp, recovery/result.hpp: plan section 30) and nothing
// else, which the build checks (tests/api/api_header_check.cpp).
//
// Text is UTF-8. Names, labels and tags come from untrusted disks: they are
// valid UTF-8 but may hold any character, so a user interface must not
// interpret them (as markup, as format strings, as paths to open). Paths
// are std::filesystem::path. Times are UTC unless a field says otherwise.

#include "recovery/error.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::api {

// The version of this API: changes that break callers increase it.
inline constexpr std::uint32_t kApiVersion = 1;

using Time = std::chrono::sys_time<std::chrono::milliseconds>;

// ===========================================================================
// Sources
// ===========================================================================

enum class SourceKind : std::uint8_t {
    // A whole physical disk (\\.\PhysicalDriveN). Reading one needs
    // administrator rights.
    PhysicalDisk,
    // A disk image file: a raw copy of a disk (createImage() writes one).
    DiskImage,
};

// Names a source. The engine only ever reads it.
struct SourceRef {
    SourceKind kind = SourceKind::DiskImage;
    // PhysicalDisk: its number (DiskInfo::number).
    std::uint32_t disk = 0;
    // DiskImage: the file.
    std::filesystem::path image;
    // DiskImage: its logical sector size; 0: the size its metadata
    // (<image>.imgmeta) records, else 512.
    std::uint32_t sectorSize = 0;

    [[nodiscard]] static SourceRef physicalDisk(std::uint32_t number);
    [[nodiscard]] static SourceRef imageFile(std::filesystem::path path, std::uint32_t sectorSize = 0);
};

// A physical disk attached to the computer (listSources()).
struct DiskInfo {
    std::uint32_t number = 0;
    // "\\.\PhysicalDrive2"
    std::string devicePath;
    // Bytes and logical sector size; 0 when Windows does not know them (a
    // card reader without a card).
    std::uint64_t size = 0;
    std::uint32_t sectorSize = 0;
    std::string vendor;
    std::string product;
    std::optional<bool> removable;
    // How it is attached: "USB", "SD", "SATA", "NVMe", ...; empty when not known.
    std::string bus;
    // The drive letters of its volumes ("E:"), in order.
    std::vector<std::string> driveLetters;
    // It holds Windows.
    bool system = false;
    // It holds the sessions folder: a session cannot be kept on the disk it
    // scans, so scanning it needs a sessions folder on another disk
    // (ApiOptions::sessionsRoot).
    bool holdsSessions = false;
    // For people: "Disk 2: Generic STORAGE DEVICE, 29.7 GiB, USB (E:)".
    std::string description;
};

struct SourceDetails {
    SourceKind kind = SourceKind::DiskImage;
    // The image file, or the disk's device path.
    std::string path;
    std::optional<std::uint32_t> disk;
    std::uint64_t size = 0;
    std::uint32_t sectorSize = 0;
    std::uint32_t physicalSectorSize = 0;
    std::string vendor;
    std::string product;
    std::optional<bool> removable;
    // For people: "physical disk 2 (Generic STORAGE DEVICE)", "disk image E:\card.img".
    std::string description;
};

enum class ImageState : std::uint8_t {
    InProgress,
    Completed,
    Cancelled,
    Failed,
};

// What an image file's metadata (<image>.imgmeta, written with the image)
// says about it.
struct ImageFileInfo {
    std::filesystem::path metadataFile;
    ImageState state = ImageState::InProgress;
    // The source it was imaged from, for people.
    std::string imagedFrom;
    std::uint64_t sourceSize = 0;
    std::uint64_t bytesImaged = 0;
    std::uint32_t sectorSize = 0;
    // The source could not be read there: the image holds zeros, and a scan
    // treats those bytes as unreadable.
    std::uint64_t unreadableBytes = 0;
    std::uint64_t unreadableRegions = 0;
    // As recorded ("2026-10-09T10:15:30Z").
    std::string started;
    std::string updated;
};

enum class PartitionScheme : std::uint8_t {
    // No partition table is recognised: the source is read as one volume.
    None,
    // The first sector holds a volume's boot record: the source is one volume.
    Unpartitioned,
    Mbr,
    Gpt,
};

struct PartitionInfo {
    std::uint32_t index = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    // "FAT32 (LBA)", "Microsoft basic data", ...
    std::string type;
    // GPT: the partition's name.
    std::string name;
    bool bootable = false;
    // MBR: inside an extended partition.
    bool logical = false;
    // It reached beyond the end of the source and is cut to it.
    bool truncated = false;
};

enum class FilesystemKind : std::uint8_t {
    Fat32,
    ExFat,
    Ntfs,
};

struct VolumeInfo {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    // Its partition (PartitionInfo::index), if the source has a partition table.
    std::optional<std::uint32_t> partition;
    // The filesystem found; none: none the engine reads (a deep scan still
    // finds its files by their content).
    std::optional<FilesystemKind> filesystem;
    std::string label;
    // As Windows shows it: "1A2B-3C4D" (FAT32, exFAT), 16 hex digits (NTFS).
    std::string serialNumber;
    std::uint32_t clusterSize = 0;
    // Sessions: the scan is done with it, and the files its metadata knows.
    bool scanned = false;
    std::uint64_t files = 0;
    std::vector<std::string> warnings;
    // Why it could not be used.
    std::optional<Error> error;
};

// What inspectSource() finds: the source, its partitions and the volumes a
// scan would read, with their filesystems. Only boot records and partition
// tables are read.
struct SourceInspection {
    SourceDetails source;
    // An image file with metadata.
    std::optional<ImageFileInfo> image;
    PartitionScheme partitionScheme = PartitionScheme::None;
    std::vector<PartitionInfo> partitions;
    // Problems of the partition table, for people.
    std::vector<std::string> partitionIssues;
    // The partitions of an MBR or a GPT, else the whole source.
    std::vector<VolumeInfo> volumes;
};

// ===========================================================================
// Scans
// ===========================================================================

enum class ScanMode : std::uint8_t {
    // Filesystem metadata (FAT32, exFAT, NTFS), deleted files included.
    Quick,
    // Also files found by their content (carving), MP4 recovery and the
    // reconstruction of fragmented files.
    Deep,
};

// What a scan looks for. A session keeps it: a resumed scan goes on with it.
struct ScanSettings {
    ScanMode mode = ScanMode::Deep;
    // Deep scans: carving by content, MP4 recovery, fragment reconstruction.
    bool carving = true;
    bool mp4 = true;
    bool fragments = true;
    // Files of the filesystems: those that are there, and deleted ones.
    bool activeFiles = true;
    bool deletedFiles = true;
    // Deep scans: files are looked for where they start at multiples of
    // `alignment` bytes (1: anywhere; 512: sector starts), and carving stops
    // after `maxSignatures` signatures.
    std::uint32_t alignment = 1;
    std::uint64_t maxSignatures = 10'000'000;
    // Reads of a failing sector after the first.
    std::uint32_t sectorRetries = 1;
    // Validation: the media data (the engine's decoders), and playability
    // (Windows' decoders decode every file: slow).
    bool mediaValidation = true;
    bool playability = false;
    // How the scan runs (not kept): worker threads, 0 to choose from the
    // processors.
    std::uint32_t workerThreads = 0;
};

// The stages of a scan, in order. A Quick scan runs Volumes and Evaluation.
enum class ScanStage : std::uint8_t {
    // Partitions, filesystems and the files they know.
    Volumes,
    // MP4 recovery examines the filesystems' video files.
    Mp4Examination,
    // Fragment reconstruction examines deleted files.
    FragmentSeeds,
    // One pass over the whole source: files found by their content.
    SourcePass,
    // MP4 files put together.
    Mp4Delivery,
    // Fragmented files reconstructed.
    Fragments,
    // Every file validated and identified: the candidates are delivered.
    Evaluation,
    Completed,
};

// ===========================================================================
// Operations and progress
// ===========================================================================

enum class OperationKind : std::uint8_t {
    None,
    Scan,
    Recovery,
    Imaging,
};

enum class OperationState : std::uint8_t {
    // Nothing has run since the session was opened (getSession() tells what
    // earlier runs did).
    Idle,
    Running,
    Paused,
    // How the last operation ended.
    Completed,
    Cancelled,
    Failed,
};

struct ScanMetrics {
    std::uint64_t sourceSize = 0;
    // Bytes of the source the pass over it has examined (Deep scans).
    std::uint64_t bytesScanned = 0;
    // Bytes read from the source by every stage.
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesPerSecond = 0;
    // Files the filesystems know, files found by content, MP4 files and
    // fragmented files put together.
    std::uint64_t filesFound = 0;
    std::uint64_t carves = 0;
    std::uint64_t mp4Candidates = 0;
    std::uint64_t reconstructions = 0;
    // Candidates delivered, those that failed validation, and those with the
    // content of an earlier one.
    std::uint64_t candidates = 0;
    std::uint64_t validationFailures = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t unreadableBytes = 0;
    // Running time over every run (time paused is not counted).
    std::chrono::milliseconds elapsed{0};
};

struct ScanProgress {
    ScanStage stage = ScanStage::Volumes;
    // The stage's number among those of the scan's mode (1-based), and how
    // many there are (Quick 2, Deep 7).
    std::uint32_t stageNumber = 1;
    std::uint32_t stageCount = 0;
    // The stage's units done and to do (volumes, files, bytes of the pass,
    // candidates); a total of 0 is not known yet.
    std::uint64_t stageDone = 0;
    std::uint64_t stageTotal = 0;
    // The whole scan, from 0 to 1: an estimate for a progress bar (the
    // stages weighted by their usual share of the time). It never goes back
    // while the API watches the scan.
    double fraction = 0.0;
    ScanMetrics metrics;
};

struct RecoveryMetrics {
    // Files to write, written, and that could not be written.
    std::uint64_t files = 0;
    std::uint64_t recovered = 0;
    std::uint64_t failed = 0;
    // Bytes written, and bytes of them the source could not give (zeros).
    std::uint64_t bytesRecovered = 0;
    std::uint64_t unreadableBytes = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesPerSecond = 0;
    std::chrono::milliseconds elapsed{0};
};

struct RecoveryProgress {
    // The recovery job running, and its place among the operation's jobs
    // (1-based).
    std::uint32_t job = 0;
    std::uint32_t jobNumber = 0;
    std::uint32_t jobCount = 0;
    // The job's files done (written or failed) and to do.
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    // The whole operation, from 0 to 1 (every job's files).
    double fraction = 0.0;
    RecoveryMetrics metrics;
};

struct ImagingProgress {
    std::filesystem::path image;
    std::filesystem::path metadataFile;
    std::uint64_t bytesImaged = 0;
    std::uint64_t totalBytes = 0;
    // A resumed image: where this run began.
    std::uint64_t resumedFrom = 0;
    std::uint64_t unreadableBytes = 0;
    std::uint64_t unreadableRegions = 0;
    std::uint64_t bytesPerSecond = 0;
    std::chrono::milliseconds elapsed{0};
    double fraction = 0.0;
};

// A session's (or an imaging's) operation now: what runs and how far it is,
// or how the last one ended.
struct Progress {
    OperationKind operation = OperationKind::None;
    OperationState state = OperationState::Idle;
    // A scan; an idle session: what its scan recorded.
    std::optional<ScanProgress> scan;
    std::optional<RecoveryProgress> recovery;
    std::optional<ImagingProgress> imaging;
    // Failed: why.
    std::optional<Error> error;
};

// ===========================================================================
// Candidates
// ===========================================================================

// A candidate's number in its session: 1 for the first delivered, then one
// more each.
struct CandidateId {
    std::uint64_t value = 0;
    friend auto operator<=>(const CandidateId&, const CandidateId&) = default;
};

enum class MediaKind : std::uint8_t {
    // No format is known, or it is not image, audio or video.
    Other,
    Image,
    Audio,
    Video,
};

// How the engine found a file.
enum class FoundBy : std::uint8_t {
    // A filesystem's metadata names it and says where its data is.
    Filesystem,
    // Its content alone: no metadata names it, its name is made up
    // ("recovered_000001.jpg").
    Carving,
    // The metadata names it, and its own structure gave or confirmed where
    // its data is.
    Hybrid,
    // Put together from fragments.
    Reconstruction,
};

// The condition a candidate would be recovered in, first rule that applies.
enum class Condition : std::uint8_t {
    // Every byte is there and its structure validates.
    Complete,
    // Every byte is there, but nothing checked the content.
    Unverified,
    // The whole file is there, but part of it is damaged.
    Corrupted,
    // Part of the file is missing.
    Partial,
    // Nothing of the file can be recovered.
    Unrecoverable,
    // One of several possible layouts: which one holds the file is not known.
    Ambiguous,
};

// The facts a condition rests on (every one that applies).
enum class ConditionReason : std::uint8_t {
    AlternativeLayout,
    NothingLocated,
    ReconstructionFailed,
    DataMissing,
    ContentTruncated,
    ReconstructionPartial,
    ValidationFailed,
    DataUnreadable,
    ClustersReallocated,
    ReconstructionCorrupted,
    NotValidated,
};

// The structure's validation, all levels together.
enum class ValidationResult : std::uint8_t {
    NotValidated,
    Valid,
    // Consistent as far as it goes, but cut short.
    Truncated,
    Invalid,
};

// One validation level's result.
enum class CheckResult : std::uint8_t {
    NotRun,
    Passed,
    Truncated,
    Failed,
    // Nothing is coded at this level (uncompressed pixels, PCM samples).
    NotApplicable,
    // This level cannot check this content (no decoder for it).
    Unsupported,
};

enum class ValidationLevel : std::uint8_t {
    Structural,
    Media,
    Playability,
};

struct Validation {
    ValidationResult result = ValidationResult::NotValidated;
    CheckResult structural = CheckResult::NotRun;
    CheckResult media = CheckResult::NotRun;
    CheckResult playability = CheckResult::NotRun;
    std::optional<ValidationLevel> deepestPassed;
};

enum class RecoveryState : std::uint8_t {
    // No recovery job writes it.
    NotRecovered,
    // A job will write it (it has not reached it yet, or was stopped first).
    Pending,
    // A job wrote it.
    Recovered,
    // A job could not write it, and none wrote it.
    Failed,
};

// A time a filesystem records. FAT records local times without a zone:
// `local` is set and `time` holds the recorded clock reading, to be shown as
// it is, not converted.
struct FileTime {
    Time time{};
    bool local = false;
};

// What a list shows about a candidate (getCandidates()).
struct CandidateInfo {
    CandidateId id;
    // The name ("IMG_0001.JPG"; made up for a file only its content found),
    // its folder path on its volume ("/DCIM/100MEDIA/IMG_0001.JPG"; empty
    // without a filesystem's metadata) and its extension ("jpg").
    std::string name;
    std::string path;
    std::string extension;
    MediaKind kind = MediaKind::Other;
    // The format its content was validated as ("jpeg", "mp4", ...; empty
    // when none) and its media type ("image/jpeg").
    std::string format;
    std::string mediaType;
    // The bytes recovery writes, and the size the metadata or the structure gives.
    std::uint64_t size = 0;
    std::uint64_t expectedSize = 0;
    // The filesystem says it was deleted.
    bool deleted = false;
    FoundBy foundBy = FoundBy::Filesystem;
    // Its data is in more than one piece on the source.
    bool fragmented = false;
    std::optional<FileTime> created;
    std::optional<FileTime> modified;
    Condition condition = Condition::Complete;
    std::vector<ConditionReason> reasons;
    Validation validation;
    // SHA-256 of the bytes recovery writes (64 hex digits; empty when not
    // computed), and the earlier candidate with the same bytes.
    std::string sha256;
    std::optional<CandidateId> duplicateOf;
    // A file found by its content inside another candidate (a picture inside
    // a document that is there): that candidate.
    std::optional<CandidateId> container;
    // Bytes of it the source could not give (written as zeros).
    std::uint64_t unreadableBytes = 0;
    // What recovery jobs did with it; Recovered: the last file written, and
    // whether every byte of it was read.
    RecoveryState recovery = RecoveryState::NotRecovered;
    std::filesystem::path recoveredFile;
    bool recoveredComplete = false;
};

// Which candidates: every list given must hold the candidate's value (an
// empty list holds every value).
struct CandidateFilter {
    std::vector<MediaKind> kinds;
    std::vector<Condition> conditions;
    std::vector<RecoveryState> recovery;
    // Only deleted files (true) or only files that are there (false).
    std::optional<bool> deleted;
    // Leave out candidates with the content of an earlier one.
    bool skipDuplicates = false;

    [[nodiscard]] bool selectsAll() const noexcept;
};

struct CandidateQuery {
    CandidateFilter filter;
    // A page of the matching candidates, in id order: from the `first`
    // (0-based), at most `count` (at most kMaxPage).
    std::uint64_t first = 0;
    std::uint64_t count = 1000;

    static constexpr std::uint64_t kMaxPage = 100'000;
};

struct CandidatePage {
    // The session's candidates, and those that match the filter.
    std::uint64_t total = 0;
    std::uint64_t matching = 0;
    std::vector<CandidateInfo> candidates;
};

// ---- Details: read from the candidate's content on demand ----

// A date and time as a file records it: to the precision it records, in the
// zone it says (or none).
struct MediaTime {
    // ISO 8601 to its precision: "2024", "2024-05-17T14:23:05+02:00".
    std::string text;
    // The instant, when the zone is known.
    std::optional<std::chrono::sys_seconds> utc;
};

struct ImageDetails {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // "rgb", "grayscale", "indexed", "ycbcr", "cmyk", "ycck", "unknown".
    std::string colorModel;
    std::uint32_t channels = 0;
    std::uint32_t bitsPerChannel = 0;
    std::uint32_t bitsPerPixel = 0;
    bool alpha = false;
    std::uint32_t frames = 1;
    std::optional<std::uint32_t> loopCount;
    bool progressive = false;
    // How to show it (Exif orientation): turn `rotation` degrees clockwise,
    // after mirroring left to right when `mirrored`.
    std::uint32_t rotation = 0;
    bool mirrored = false;
    std::optional<MediaTime> dateTaken;
    std::string cameraMake;
    std::string cameraModel;
};

struct AudioDetails {
    // "mp3", "aac", "pcm", "alac", ...
    std::string codec;
    std::string profile;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint64_t bitrate = 0;
    bool variableBitrate = false;
};

struct VideoDetails {
    // "h264", "hevc", "mpeg4", ...
    std::string codec;
    std::string profile;
    std::string level;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    // Frames per second (numerator / denominator, e.g. 30000/1001).
    std::optional<std::pair<std::uint64_t, std::uint64_t>> frameRate;
    // Degrees to turn the picture clockwise to show it.
    std::uint32_t rotation = 0;
    std::uint64_t bitrate = 0;
};

struct TrackDetails {
    std::uint32_t id = 0;
    MediaKind kind = MediaKind::Other;
    // "vide", "soun", ...; the sample description ("avc1", "mp4a").
    std::string handler;
    std::string sampleEntry;
    std::string language;
    bool enabled = true;
    std::optional<std::chrono::microseconds> duration;
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::optional<VideoDetails> video;
    std::optional<AudioDetails> audio;
};

struct MovieDetails {
    std::string majorBrand;
    std::vector<std::string> compatibleBrands;
    std::optional<MediaTime> created;
    std::optional<MediaTime> modified;
    bool fragmented = false;
    std::vector<TrackDetails> tracks;
};

struct MediaTags {
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::optional<MediaTime> date;
    std::optional<std::uint32_t> track;
    std::optional<std::uint32_t> trackTotal;
};

enum class PreviewKind : std::uint8_t {
    // The file itself: an image to show, audio or video to play.
    Content,
    // A smaller picture stored with an image (an Exif thumbnail).
    Thumbnail,
    // A picture stored with audio or video (cover art).
    CoverArt,
};

// A preview: bytes a platform decoder can show or play as they are
// (readPreview() hands them out). Nothing is decoded by the engine.
struct PreviewInfo {
    PreviewKind kind = PreviewKind::Content;
    // The format of its bytes ("jpeg", "png", ...), and their media type.
    std::string format;
    std::string mediaType;
    std::uint64_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // How to show it: turn `rotation` degrees clockwise, after mirroring
    // left to right when `mirrored`.
    std::uint32_t rotation = 0;
    bool mirrored = false;
    // Cover art: the picture type of ID3 (3: front cover).
    std::optional<std::uint32_t> pictureType;
};

// What a candidate's content says about itself.
struct MediaDetails {
    MediaKind kind = MediaKind::Other;
    std::string format;
    std::string mediaType;
    std::optional<std::chrono::microseconds> duration;
    // The duration is an estimate (part of the file was counted).
    bool durationEstimated = false;
    std::optional<std::uint64_t> bitrate;
    std::optional<ImageDetails> image;
    std::optional<AudioDetails> audio;
    std::optional<VideoDetails> video;
    std::optional<MovieDetails> movie;
    MediaTags tags;
    // Thumbnails and cover art first, then the content.
    std::vector<PreviewInfo> previews;
    // Problems of the content found while reading it (the first ones, and
    // how many there were), for people.
    std::vector<std::string> issues;
    std::uint64_t issueCount = 0;
};

// What one recovery job did with a candidate.
struct RecoveryRecord {
    std::uint32_t job = 0;
    RecoveryState state = RecoveryState::Pending;
    // Recovered: the file, its size, and the bytes of it not located or not
    // read (zeros, or the file is cut short); whether none are.
    std::filesystem::path file;
    std::uint64_t size = 0;
    std::uint64_t missingBytes = 0;
    std::uint64_t unreadableBytes = 0;
    bool complete = false;
    // Failed: why.
    std::optional<Error> error;
};

struct CandidateDetails {
    CandidateInfo candidate;
    // How the engine found and checked it, in words, one fact per line.
    std::vector<std::string> explanation;
    // Every job that wrote it or will, by job.
    std::vector<RecoveryRecord> recoveries;
    // Read from its content (the source must be attached): the metadata,
    // or why it could not be read.
    std::optional<MediaDetails> media;
    std::optional<Error> mediaError;
};

// ===========================================================================
// Recovery
// ===========================================================================

struct RecoveryOptions {
    // The folder the files are written to, created when needed. It must not
    // be on the source. A file is never overwritten: a name that is taken
    // gets " (1)", " (2)", ...
    std::filesystem::path destination;
    // Write again files already recovered to this folder (they get new
    // names). Default: they are left out.
    bool again = false;
    // Worker threads; 0 to choose from the processors.
    std::uint32_t workerThreads = 0;
};

// What a recovery request starts. Recovery converges on its folder: recovery
// jobs that write there and did not end are finished first, then one new job
// writes the requested candidates that no job has written there.
struct RecoveryStart {
    // The candidates asked for; those left out because a job recovered them
    // to the folder before; those an unfinished job there writes.
    std::uint64_t requested = 0;
    std::uint64_t alreadyRecovered = 0;
    std::uint64_t inUnfinishedJobs = 0;
    // The jobs the operation runs, in order (the new one last). Empty:
    // nothing to do, no operation started.
    std::vector<std::uint32_t> jobs;
    std::optional<std::uint32_t> newJob;
};

// ===========================================================================
// Sessions
// ===========================================================================

enum class RunState : std::uint8_t {
    NotStarted,
    // Running (or recorded as running by a program that has it open).
    Running,
    Paused,
    Completed,
    Cancelled,
    Failed,
    // Recorded as running, but the program that ran it ended without
    // recording an end (a crash, a power cut): it resumes.
    Interrupted,
};

struct StateChange {
    RunState state = RunState::Running;
    Time time{};
    std::string engineVersion;
    std::optional<Error> error;
};

struct ScanStatus {
    RunState state = RunState::NotStarted;
    ScanStage stage = ScanStage::Volumes;
    ScanMetrics metrics;
    std::uint64_t candidates = 0;
    // resumeScan() can run it (again); why not.
    bool resumable = true;
    std::string notResumable;
    std::vector<StateChange> history;
};

struct JobInfo {
    std::uint32_t id = 0;
    std::filesystem::path destination;
    Time created{};
    RunState state = RunState::NotStarted;
    // Its candidates, and those done: written, or failed.
    std::uint64_t files = 0;
    std::uint64_t done = 0;
    std::uint64_t recovered = 0;
    std::uint64_t failed = 0;
    // Files begun and not finished (written again when it resumes).
    std::uint64_t filesInProgress = 0;
    RecoveryMetrics metrics;
    std::vector<StateChange> history;
};

struct SessionError {
    Time time{};
    // "scan", "volume at <offset>", "recovery job <n>, candidate <id>", ...
    std::string context;
    Error error;
};

// Everything a session holds but its candidates (getCandidates()).
struct SessionDetails {
    std::string id;
    std::filesystem::path folder;
    std::string engineVersion;
    Time created{};
    Time updated{};
    SourceDetails source;
    // workerThreads is 0: a session does not keep it.
    ScanSettings settings;
    ScanStatus scan;
    PartitionScheme partitionScheme = PartitionScheme::None;
    std::vector<VolumeInfo> volumes;
    std::vector<JobInfo> jobs;
    std::vector<SessionError> errors;
    std::uint64_t unreadableBytes = 0;
    std::uint64_t unreadableRegions = 0;
    // Repairs made when it was opened (damage cut off, a copy kept), for people.
    std::vector<std::string> repairs;
    // The operation under way.
    OperationKind running = OperationKind::None;
    bool paused = false;
};

// A session found in the sessions folder (listSessions()), read without
// opening it.
struct SessionListing {
    std::string id;
    std::filesystem::path folder;
    // It cannot be read: why (another format, damage at its start).
    std::optional<Error> error;
    std::string engineVersion;
    Time created{};
    Time updated{};
    SourceKind sourceKind = SourceKind::DiskImage;
    std::string sourcePath;
    std::uint64_t sourceSize = 0;
    std::string sourceDescription;
    ScanMode mode = ScanMode::Deep;
    RunState scanState = RunState::NotStarted;
    ScanStage stage = ScanStage::Volumes;
    std::uint64_t candidates = 0;
    std::uint64_t jobs = 0;
    // Open in this API.
    bool open = false;
};

// ===========================================================================
// Imaging and reports
// ===========================================================================

struct ImagingId {
    std::uint32_t value = 0;
    friend auto operator<=>(const ImagingId&, const ImagingId&) = default;
};

struct ImagingOptions {
    // The image file to write: never on the source, never overwritten. Its
    // metadata goes next to it (<image>.imgmeta).
    std::filesystem::path image;
    // Go on with an image an earlier run did not finish.
    bool resume = false;
    // Reads of a failing sector after the first, and the size of each read
    // (a multiple of the sector size, at most 64 MiB).
    std::uint32_t sectorRetries = 2;
    std::uint64_t blockSize = 1024 * 1024;
};

enum class ReportFormat : std::uint8_t {
    // "recovery-session-report" (docs/recovery/cli.md), for programs.
    Json,
    // For people.
    Text,
};

struct ReportOptions {
    ReportFormat format = ReportFormat::Json;
    // Text: every candidate's evidence and checks too.
    bool details = false;
};

// ===========================================================================
// Events
// ===========================================================================

enum class EventKind : std::uint8_t {
    // An operation began; Event::progress is its state.
    OperationStarted,
    // An operation's progress (at most every ApiOptions::progressInterval).
    Progress,
    Paused,
    Resumed,
    // A scan delivered candidates: ids firstCandidate to firstCandidate +
    // candidateCount - 1, now in getCandidates().
    CandidatesFound,
    // A recovery job is done with files: written, or not (Event::files).
    FilesRecovered,
    // An operation ended: Event::progress.state says how (Completed,
    // Cancelled, Failed with progress.error).
    OperationFinished,
};

// A file a recovery job is done with.
struct RecoveredFile {
    CandidateId candidate;
    std::uint32_t job = 0;
    // Written: the file, its size, the bytes not located or not read, and
    // whether none are. Not written: why.
    std::optional<std::filesystem::path> file;
    std::uint64_t size = 0;
    std::uint64_t missingBytes = 0;
    std::uint64_t unreadableBytes = 0;
    bool complete = false;
    std::optional<Error> error;
};

struct Event {
    EventKind kind = EventKind::Progress;
    // The session, or (imaging) empty and `imaging` set.
    std::string session;
    std::optional<ImagingId> imaging;
    // The operation's state as of the event.
    Progress progress;
    // CandidatesFound.
    CandidateId firstCandidate;
    std::uint64_t candidateCount = 0;
    // FilesRecovered.
    std::vector<RecoveredFile> files;
    // FilesRecovered and OperationFinished: files the event queue had to
    // leave out since the last such event (the user interface fell behind);
    // getCandidates() has them.
    std::uint64_t filesDropped = 0;
};

// Called on the API's event thread, one event at a time, in order (see
// ApiOptions::onEvent).
using EventCallback = std::function<void(const Event& event)>;

// ===========================================================================
// Logging
// ===========================================================================

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

// One record of the engine's log. Fields hold offsets, sizes, ids and
// states, never file content.
struct LogEntry {
    Time time{};
    LogLevel level = LogLevel::Info;
    std::string component;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
    // One line: "<time> <LEVEL> [component] message key=value ...", control
    // characters escaped.
    std::string line;
};

using LogCallback = std::function<void(const LogEntry& entry)>;

// ===========================================================================
// Names for people and for programs
// ===========================================================================

// The API's names for its values: lower case, words joined by '-' ("deep",
// "source-pass", "physical-disk"). They do not change: programs may store
// and compare them. (The JSON report names values as the engine does.)
[[nodiscard]] std::string_view toString(SourceKind kind) noexcept;
[[nodiscard]] std::string_view toString(ImageState state) noexcept;
[[nodiscard]] std::string_view toString(PartitionScheme scheme) noexcept;
[[nodiscard]] std::string_view toString(FilesystemKind kind) noexcept;
[[nodiscard]] std::string_view toString(ScanMode mode) noexcept;
[[nodiscard]] std::string_view toString(ScanStage stage) noexcept;
[[nodiscard]] std::string_view toString(OperationKind kind) noexcept;
[[nodiscard]] std::string_view toString(OperationState state) noexcept;
[[nodiscard]] std::string_view toString(MediaKind kind) noexcept;
[[nodiscard]] std::string_view toString(FoundBy method) noexcept;
[[nodiscard]] std::string_view toString(Condition condition) noexcept;
[[nodiscard]] std::string_view toString(ConditionReason reason) noexcept;
[[nodiscard]] std::string_view toString(ValidationResult result) noexcept;
[[nodiscard]] std::string_view toString(CheckResult result) noexcept;
[[nodiscard]] std::string_view toString(ValidationLevel level) noexcept;
[[nodiscard]] std::string_view toString(RecoveryState state) noexcept;
[[nodiscard]] std::string_view toString(PreviewKind kind) noexcept;
[[nodiscard]] std::string_view toString(RunState state) noexcept;
[[nodiscard]] std::string_view toString(ReportFormat format) noexcept;
[[nodiscard]] std::string_view toString(EventKind kind) noexcept;
[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

// For people: "1.5 KiB", "2.0 GiB" (powers of 1024). An error for people is
// recovery::describe(error) (recovery/error.hpp).
[[nodiscard]] std::string formatSize(std::uint64_t bytes);

}  // namespace recovery::api
