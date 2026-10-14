#include "fuzz_seeds.hpp"

#include "formats/archive_formats.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "imaging/image_metadata.hpp"
#include "partition/guid.hpp"
#include "recovery/text.hpp"
#include "session/recovery_session.hpp"
#include "session/session_journal.hpp"
#include "storage/destination_file.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/memory_source.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/zip_builders.hpp"
#include "support/zip_samples.hpp"
#include "validation/deflate_decoder.hpp"
#include "validation/media_validator.hpp"

#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace recovery::test::corpus {

namespace {

using Bytes = std::vector<std::byte>;

class Seeds {
public:
    explicit Seeds(std::filesystem::path folder) : folder_(std::move(folder)) {}

    // One seed of `target`: `name` (made unique per target) holding `bytes`.
    Status add(const std::string& target, const std::string& name, std::span<const std::byte> bytes) {
        std::error_code ec;
        const std::filesystem::path below = folder_ / target;
        std::filesystem::create_directories(below, ec);
        if (ec) {
            return makeError(ErrorCode::DestinationError, "cannot create " + toUtf8(below) + ": " + ec.message());
        }
        const std::filesystem::path path = below / (name + ".bin");
        Result<storage::DestinationFile> file =
            storage::DestinationFile::open(path, storage::DestinationFile::OpenMode::CreateNew);
        if (!file.ok()) {
            return file.error();
        }
        if (!bytes.empty()) {
            if (Status written = file->writeAt(0, bytes); !written.ok()) {
                return written;
            }
        }
        ++count_;
        return file->flush();
    }

    // The same seed, prefixed with one byte (targets whose first byte selects something).
    Status addWithPrefix(const std::string& target, const std::string& name, std::uint8_t prefix,
                         std::span<const std::byte> bytes) {
        Bytes prefixed;
        prefixed.reserve(bytes.size() + 1);
        prefixed.push_back(static_cast<std::byte>(prefix));
        prefixed.insert(prefixed.end(), bytes.begin(), bytes.end());
        return add(target, name, prefixed);
    }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return folder_; }

private:
    std::filesystem::path folder_;
    std::size_t count_ = 0;
};

// Files of every format (ZIP archives too): what the formats, media, metadata
// and MP4 targets take.
std::vector<std::pair<std::string, Bytes>> mediaFiles() {
    std::vector<std::pair<std::string, Bytes>> files;
    const auto add = [&](std::string name, Bytes bytes) { files.emplace_back(std::move(name), std::move(bytes)); };
    {
        JpegOptions baseline;
        baseline.width = 48;
        baseline.height = 32;
        add("jpeg_baseline", makeJpeg(baseline));
        JpegOptions progressive = baseline;
        progressive.progressive = true;
        progressive.restartInterval = 2;
        add("jpeg_progressive", makeJpeg(progressive));
        JpegOptions exif = baseline;
        exif.exifThumbnail = true;
        exif.exifOrientation = 6;
        exif.exifMake = "Seed";
        exif.exifDateTaken = "2024:05:17 13:45:30";
        add("jpeg_exif", makeJpeg(exif));
        JpegOptions gray = baseline;
        gray.sampling = JpegSampling::Gray;
        add("jpeg_gray", makeJpeg(gray));
    }
    {
        PngOptions png;
        png.width = 16;
        png.height = 12;
        add("png_rgb", makePng(png));
        png.color = PngColor::Palette;
        png.textChunks = true;
        add("png_palette", makePng(png));
        png.color = PngColor::Rgba;
        png.interlaced = true;
        add("png_interlaced", makePng(png));
        png.interlaced = false;
        png.animated = true;
        add("png_animated", makePng(png));
    }
    {
        GifOptions gif;
        gif.width = 16;
        gif.height = 12;
        add("gif_89a", makeGif(gif));
        gif.frames = 3;
        gif.loop = true;
        gif.localColorTables = true;
        add("gif_animated", makeGif(gif));
        gif.version89a = false;
        gif.frames = 1;
        gif.loop = false;
        gif.graphicControl = false;
        gif.localColorTables = false;
        gif.interlaced = true;
        add("gif_87a", makeGif(gif));
    }
    {
        BmpOptions bmp;
        add("bmp_24", makeBmp(bmp));
        bmp.bitsPerPixel = 8;
        bmp.compression = BmpCompression::Rle8;
        add("bmp_rle8", makeBmp(bmp));
        bmp.bitsPerPixel = 4;
        bmp.compression = BmpCompression::Rle4;
        add("bmp_rle4", makeBmp(bmp));
        bmp.bitsPerPixel = 32;
        bmp.compression = BmpCompression::Bitfields;
        bmp.header = BmpHeader::V5;
        bmp.profileSize = 64;
        add("bmp_v5", makeBmp(bmp));
    }
    for (const auto& [kind, name] : std::vector<std::pair<WebpKind, std::string>>{
             {WebpKind::Lossy, "webp_lossy"},
             {WebpKind::Lossless, "webp_lossless"},
             {WebpKind::LossyWithAlpha, "webp_alpha"},
             {WebpKind::Animated, "webp_animated"}}) {
        WebpOptions webp;
        webp.kind = kind;
        add(name, makeWebp(webp));
    }
    {
        Mp3Options mp3;
        mp3.frames = 6;
        add("mp3_lame", makeMp3(mp3));
        mp3.bitrate = 0;
        mp3.infoTag = Mp3InfoTag::Xing;
        mp3.id3v2 = 3;
        mp3.id3v1 = true;
        mp3.crc = true;
        add("mp3_vbr_tags", makeMp3(mp3));
        mp3.version = MpegVersion::Mpeg2;
        mp3.sampleRate = 22050;
        mp3.channels = Mp3Channels::Mono;
        mp3.id3v2 = 4;
        mp3.apeTag = true;
        mp3.lyrics3 = true;
        add("mp3_mpeg2_ape", makeMp3(mp3));
    }
    {
        WavOptions wav;
        wav.frames = 32;
        add("wav_pcm", makeWav(wav));
        wav.encoding = WavEncoding::Float;
        wav.bitsPerSample = 32;
        wav.extensible = true;
        wav.listInfo = true;
        add("wav_float", makeWav(wav));
        wav.encoding = WavEncoding::ImaAdpcm;
        wav.extensible = false;
        wav.frames = 2;
        wav.cue = true;
        add("wav_adpcm", makeWav(wav));
    }
    {
        AacOptions adts;
        adts.frames = 4;
        add("adts", makeAdts(adts));
        M4aOptions m4a;
        m4a.audio.frames = 4;
        add("m4a_moov_last", makeM4a(m4a));
        m4a.moovFirst = true;
        m4a.metadata = true;
        m4a.co64 = true;
        add("m4a_moov_first", makeM4a(m4a));
    }
    {
        Mp4Options fragmented;
        fragmented.moov = Mp4MoovPlace::First;
        fragmented.fragmentSamples = 3;
        fragmented.tracks[0].samples = 6;
        fragmented.tracks[1].samples = 6;
        add("mp4_fragmented", makeMp4(fragmented).bytes);
        Mp4Options wide;
        wide.largeMediaData = true;
        wide.largeMoov = true;
        wide.co64 = true;
        wide.extraBoxes = true;
        wide.tracks[0].samples = 4;
        wide.tracks[0].compactSizes = true;
        wide.tracks[1].samples = 4;
        add("mp4_wide", makeMp4(wide).bytes);
    }
    for (const samples::Sample& sample : samples::all()) {
        add("sample_" + std::string(sample.name), sample.data());
    }
    for (const audio_samples::Sample& sample : audio_samples::all()) {
        add("sample_" + std::string(sample.name), sample.data());
    }
    for (const mp4_samples::Sample& sample : mp4_samples::all()) {
        add("sample_" + std::string(sample.name), sample.data());
    }
    // ZIP archives (after P20): Python's zipfile's and reworked ones, and one
    // with an image stored in it.
    for (const zip_samples::Sample& sample : zip_samples::all()) {
        add("zip_" + std::string(sample.name), sample.data());
    }
    {
        PngOptions png;
        png.width = 16;
        png.height = 16;
        add("zip_stored_png", storedArchive({{"icon.png", makePng(png)}, {"notes.txt", makePattern(300, 41)}}));
    }
    return files;
}

// The clusters of a file of `size` bytes in two pieces, the second before the first.
template <typename T>
std::vector<T> twoPieces(std::size_t size, std::uint32_t clusterSize, T first, T second) {
    const auto count = static_cast<T>((size + clusterSize - 1) / clusterSize);
    std::vector<T> clusters;
    for (T i = 0; i < count / 2; ++i) {
        clusters.push_back(first + i);
    }
    for (T i = count / 2; i < count; ++i) {
        clusters.push_back(second + i);
    }
    return clusters;
}

Bytes smallFat32() {
    Fat32BuilderOptions options;
    options.clusterCount = 256;
    options.label = "SEED";
    Fat32ImageBuilder builder(options);
    const std::uint32_t root = Fat32ImageBuilder::root();
    JpegOptions jpeg;
    jpeg.width = 16;
    jpeg.height = 16;
    (void)builder.addFile(root, "PHOTO.JPG", makeJpeg(jpeg));
    const auto folder = builder.addDirectory(root, "A long folder name");
    (void)builder.addFile(folder.clusters.front(), "Unicode ünïcode.png", makePng({}));
    const auto deleted = builder.addFile(root, "DELETED.GIF", makeGif({}));
    builder.deleteEntry(deleted);
    const Bytes wav = makeWav({});
    (void)builder.addFileInClusters(root, "FRAG.WAV", wav, twoPieces<std::uint32_t>(wav.size(), 512, 200, 170));
    return builder.build();
}

// After P20: FAT16 needs 4085 clusters at least; the seed keeps the first
// 64 KiB (boot sector, FATs, the fixed root and the files in low clusters):
// the module reads a volume larger than its source as a truncated image.
Bytes smallFat16() {
    Fat32BuilderOptions options;
    options.fat16 = true;
    options.clusterCount = 4100;
    options.reservedSectors = 1;
    options.label = "SEED16";
    Fat32ImageBuilder builder(options);
    const std::uint32_t root = Fat32ImageBuilder::root();
    JpegOptions jpeg;
    jpeg.width = 16;
    jpeg.height = 16;
    (void)builder.addFile(root, "PHOTO.JPG", makeJpeg(jpeg));
    const auto folder = builder.addDirectory(root, "A long folder name");
    (void)builder.addFile(folder.clusters.front(), "Unicode ünïcode.png", makePng({}));
    const auto deleted = builder.addFile(root, "DELETED.GIF", makeGif({}));
    builder.deleteEntry(deleted);
    const Bytes wav = makeWav({});
    (void)builder.addFileInClusters(root, "FRAG.WAV", wav, twoPieces<std::uint32_t>(wav.size(), 512, 40, 30));
    Bytes image = builder.build();
    image.resize(std::min<std::size_t>(image.size(), 64 * 1024));
    return image;
}

Bytes smallExFat() {
    ExFatBuilderOptions options;
    options.clusterCount = 256;
    options.label = "SEED";
    ExFatImageBuilder builder(options);
    JpegOptions jpeg;
    jpeg.width = 16;
    jpeg.height = 16;
    (void)builder.addFile(builder.root(), "photo.jpg", makeJpeg(jpeg));
    const auto folder = builder.addDirectory(builder.root(), "Folder");
    (void)builder.addFile(folder.clusters.front(), "inside.png", makePng({}));
    const auto deleted = builder.addFile(builder.root(), "deleted.gif", makeGif({}));
    builder.deleteEntry(deleted);
    const Bytes wav = makeWav({});
    (void)builder.addFileInClusters(builder.root(), "frag.wav", wav,
                                    twoPieces<std::uint32_t>(wav.size(), builder.clusterSize(), 200, 170));
    return builder.build();
}

Bytes smallNtfs() {
    NtfsBuilderOptions options;
    options.sectorsPerCluster = 8;
    options.clusterCount = 256;
    options.mftRecords = 96;
    options.label = "SEED";
    NtfsImageBuilder builder(options);
    (void)builder.addFile(NtfsImageBuilder::root(), "small.txt", makePattern(100, 1));
    JpegOptions jpeg;
    jpeg.width = 16;
    jpeg.height = 16;
    (void)builder.addFile(NtfsImageBuilder::root(), "photo.jpg", makeJpeg(jpeg));
    const auto folder = builder.addDirectory(NtfsImageBuilder::root(), "Folder");
    (void)builder.addFile(folder.record, "inside.png", makePng({}));
    const auto deleted = builder.addFile(NtfsImageBuilder::root(), "deleted.gif", makeGif({}));
    builder.deleteEntry(deleted);
    const Bytes wav = makeWav({});
    (void)builder.addFileInClusters(NtfsImageBuilder::root(), "frag.wav", wav,
                                    twoPieces<std::uint64_t>(wav.size(), builder.clusterSize(), 200, 180));
    return builder.build();
}

// Small disks: only the tables matter (partitions are checked against the disk's size).
Bytes mbrTables() {
    Bytes disk(128 * 512, std::byte{0});
    writeMbrSector(disk, 512, 0, {{0x80, 0x0C, 8, 32}, {0x00, 0x07, 40, 16}, {0x00, 0x0F, 64, 64}});
    writeMbrSector(disk, 512, 64, {{0x00, 0x07, 4, 20}, {0x00, 0x05, 32, 32}});
    writeMbrSector(disk, 512, 96, {{0x00, 0x0C, 4, 16}});
    return disk;
}

Bytes gptTables(std::uint32_t sectorSize) {
    const std::uint64_t sectors = sectorSize == 512 ? 256 : 64;
    Bytes disk(static_cast<std::size_t>(sectorSize * sectors), std::byte{0});
    GptSpec spec;
    spec.sectorSize = sectorSize;
    const std::uint64_t first = sectorSize == 512 ? 40 : 6;
    spec.partitions.push_back(GptPartitionSpec{partition::gpt_types::kMicrosoftBasicData,
                                               *partition::Guid::parse("C0FFEE00-0000-4000-8000-000000000001"), first,
                                               first + 15, 0, u"Data"});
    spec.partitions.push_back(GptPartitionSpec{partition::gpt_types::kEfiSystem,
                                               *partition::Guid::parse("C0FFEE00-0000-4000-8000-000000000002"),
                                               first + 16, first + 31, 0, u"EFI"});
    (void)writeGpt(disk, spec);
    return disk;
}

// A card: old data with files of several formats planted in it.
Bytes card() {
    Bytes bytes = makePattern(256 * 1024, 0xCA2D);
    std::size_t at = 4096;
    for (const auto& [name, file] : mediaFiles()) {
        if (!name.starts_with("sample_") && at + file.size() + 4096 <= bytes.size()) {
            std::copy(file.begin(), file.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
            at = (at + file.size() + 1024 + 511) / 512 * 512;
        }
    }
    return bytes;
}

Status recordSeeds(Seeds& seeds, const std::filesystem::path& journal);

// Records and journals of a real session: a scan of a small volume and a recovery job.
Status journalSeeds(Seeds& seeds) {
    const std::filesystem::path work = seeds.folder() / "journal-session.tmp";
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    MemoryStorageSource source(smallFat32());
    if (Status opened = source.open(); !opened.ok()) {
        return opened;
    }
    carving::FormatRegistry formats;
    validation::MediaValidatorRegistry media;
    if (!formats::registerImageFormats(formats).ok() || !formats::registerAudioFormats(formats).ok() ||
        !formats::registerVideoFormats(formats).ok() ||
        !formats::registerArchiveFormats(formats, validation::deflateDecoder()).ok() ||
        !validation::registerMediaValidators(media).ok()) {
        return makeError(ErrorCode::InternalError, "registering the formats");
    }
    std::filesystem::path journal;
    {
        Result<std::unique_ptr<session::RecoverySession>> created =
            session::RecoverySession::create(work / "sessions", source, scan::ScanConfiguration{});
        if (!created.ok()) {
            return created.error();
        }
        session::RecoverySession& recovery = **created;
        journal = recovery.folder() / session::RecoverySession::kJournalName;
        scan::ScanRunOptions options;
        options.blockSize = 64 * 1024;
        options.checkpointItems = 2;
        if (Result<scan::ScanSummary> scanned = recovery.runScan(source, formats, media, options); !scanned.ok()) {
            return scanned.error();
        }
        Result<std::uint32_t> job = recovery.addRecoveryJob(work / "recovered");
        if (!job.ok()) {
            return job.error();
        }
        if (Result<scan::RecoveryJobSummary> written = recovery.runRecovery(*job, source); !written.ok()) {
            return written.error();
        }
    }
    const Bytes whole = readFile(journal);
    if (Status added = seeds.addWithPrefix("journal", "journal_file", 0x80, whole); !added.ok()) {
        return added;
    }
    if (Status added = recordSeeds(seeds, journal); !added.ok()) {
        return added;
    }
    // Cut journals: what a crash leaves.
    for (const std::size_t cut : {whole.size() / 3, whole.size() / 2, whole.size() - 7}) {
        const Bytes torn(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
        if (Status added = seeds.addWithPrefix("journal", "journal_cut_" + std::to_string(cut), 0x80, torn);
            !added.ok()) {
            return added;
        }
    }
    std::filesystem::remove_all(work, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError, "cannot remove " + toUtf8(work) + ": " + ec.message());
    }
    return success();
}

// Each record of the journal, its payload after a byte that names its type.
Status recordSeeds(Seeds& seeds, const std::filesystem::path& journal) {
    Result<session::JournalReader> reader = session::JournalReader::open(journal);
    if (!reader.ok()) {
        return reader.error();
    }
    std::map<std::uint16_t, int> perType;
    for (;;) {
        Result<std::optional<session::JournalRecord>> record = reader->next();
        if (!record.ok()) {
            return record.error();
        }
        if (!record->has_value()) {
            break;
        }
        const session::JournalRecord& read = **record;
        if (read.type < 1 || read.type > 8 || perType[read.type]++ >= 4) {
            continue;
        }
        const std::string name = "record_" + std::to_string(read.type) + "_" + std::to_string(perType[read.type]);
        if (Status added = seeds.addWithPrefix("journal", name, static_cast<std::uint8_t>(read.type - 1), read.payload);
            !added.ok()) {
            return added;
        }
    }
    return success();
}

Status imageMetadataSeeds(Seeds& seeds) {
    imaging::ImageMetadata metadata;
    metadata.engineVersion = "0.1.0";
    metadata.state = imaging::ImageState::Completed;
    metadata.sourceType = storage::SourceType::PhysicalDisk;
    metadata.sourcePath = "\\\\.\\PhysicalDrive2";
    metadata.sourceVendor = "Vendor";
    metadata.sourceProduct = "Card Reader";
    metadata.sourceSize = 64 * 1024 * 1024;
    metadata.sectorSize = 512;
    metadata.blockSize = 1024 * 1024;
    metadata.bytesCompleted = metadata.sourceSize;
    metadata.startedUtc = "2024-05-17T11:45:30.000Z";
    metadata.updatedUtc = "2024-05-17T11:47:30.000Z";
    metadata.badRegions = {storage::BadRegion{4096, 512, 23}, storage::BadRegion{1048576, 65536, 27}};
    const auto add = [&](const std::string& name, const imaging::ImageMetadata& m) -> Status {
        Result<std::string> text = imaging::serializeImageMetadata(m);
        if (!text.ok()) {
            return text.error();
        }
        return seeds.add("imgmeta", name, std::as_bytes(std::span<const char>(text->data(), text->size())));
    };
    if (Status added = add("completed", metadata); !added.ok()) {
        return added;
    }
    metadata.state = imaging::ImageState::InProgress;
    metadata.bytesCompleted = 8 * 1024 * 1024;
    metadata.badRegions.pop_back();
    if (Status added = add("in_progress", metadata); !added.ok()) {
        return added;
    }
    metadata.state = imaging::ImageState::Failed;
    metadata.sectorSize = 4096;
    metadata.badRegions.clear();
    return add("failed_4k", metadata);
}

}  // namespace

Status writeFuzzSeeds(const std::filesystem::path& folder) {
    Seeds seeds(folder);
    // Partition tables: the first byte picks the sector size (0: 512, 1: 4096).
    struct Table {
        std::string name;
        bool largeSectors = false;
        Bytes bytes;
    };
    const std::vector<Table> tables = {{"mbr_extended", false, mbrTables()},
                                       {"gpt_512", false, gptTables(512)},
                                       {"gpt_4096", true, gptTables(4096)},
                                       {"superfloppy", false, makeFat32BootRecordStub()}};
    for (const Table& table : tables) {
        if (Status added = seeds.addWithPrefix("partition", table.name, table.largeSectors ? 1 : 0, table.bytes);
            !added.ok()) {
            return added;
        }
    }
    const Bytes fat32 = smallFat32();
    const Bytes fat16 = smallFat16();
    const Bytes exfat = smallExFat();
    const Bytes ntfs = smallNtfs();
    for (const auto& [name, bytes] : std::vector<std::pair<std::string, const Bytes*>>{
             {"fat32", &fat32}, {"fat16", &fat16}, {"exfat", &exfat}, {"ntfs", &ntfs}}) {
        for (const std::string target : {"filesystem", "scan"}) {
            if (Status added = seeds.add(target, name, *bytes); !added.ok()) {
                return added;
            }
        }
    }
    for (const auto& [name, bytes] : mediaFiles()) {
        const bool mp4 = name.find("mp4") != std::string::npos || name.find("m4a") != std::string::npos ||
                         name.find(".mov") != std::string::npos || name.find(".3gp") != std::string::npos;
        for (const std::string target : {"formats", "media", "metadata"}) {
            if (Status added = seeds.add(target, name, bytes); !added.ok()) {
                return added;
            }
        }
        if (mp4) {
            if (Status added = seeds.add("mp4", name, bytes); !added.ok()) {
                return added;
            }
        }
    }
    const Bytes cardBytes = card();
    for (const std::string target : {"carving", "scan"}) {
        if (Status added = seeds.add(target, "card", cardBytes); !added.ok()) {
            return added;
        }
    }
    if (Status added = imageMetadataSeeds(seeds); !added.ok()) {
        return added;
    }
    return journalSeeds(seeds);
}

}  // namespace recovery::test::corpus
