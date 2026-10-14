#include "support/corpus.hpp"

#include "carving/content_reader.hpp"
#include "formats/mp4_parser.hpp"
#include "imaging/image_metadata.hpp"
#include "partition/guid.hpp"
#include "recovery/sha256.hpp"
#include "recovery/text.hpp"
#include "storage/destination_file.hpp"
#include "support/audio_builders.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/zip_builders.hpp"
#include "support/zip_samples.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace recovery::test::corpus {

namespace {

constexpr std::uint32_t kSector = 512;
constexpr std::uint64_t kMiB = 1024 * 1024;

// ---------------------------------------------------------------------------
// Media files. Every file comes from a seed, so the files of a scenario differ
// from each other unless a scenario copies one on purpose.
// ---------------------------------------------------------------------------

Bytes jpeg(std::uint64_t seed, std::uint16_t width, std::uint16_t height) {
    JpegOptions options;
    options.width = width;
    options.height = height;
    options.seed = seed;
    return makeJpeg(options);
}

// What a camera writes: Exif with the camera, the time and a thumbnail (a
// whole JPEG inside the file).
Bytes cameraJpeg(std::uint64_t seed, std::uint16_t width, std::uint16_t height) {
    JpegOptions options;
    options.width = width;
    options.height = height;
    options.seed = seed;
    options.exifThumbnail = true;
    options.exifOrientation = 1;
    options.exifMake = "Corpus";
    options.exifModel = "Card Camera";
    options.exifDateTaken = "2024:05:17 13:45:30";
    return makeJpeg(options);
}

Bytes progressiveJpeg(std::uint64_t seed, std::uint16_t width, std::uint16_t height) {
    JpegOptions options;
    options.width = width;
    options.height = height;
    options.seed = seed;
    options.progressive = true;
    options.restartInterval = 4;
    return makeJpeg(options);
}

Bytes png(std::uint64_t seed, std::uint32_t width, std::uint32_t height) {
    PngOptions options;
    options.width = width;
    options.height = height;
    options.idatSize = 4096;
    options.textChunks = true;
    options.seed = seed;
    return makePng(options);
}

Bytes gif(std::uint64_t seed, std::uint16_t width, std::uint16_t height, std::size_t frames) {
    GifOptions options;
    options.width = width;
    options.height = height;
    options.frames = frames;
    options.colorBits = 8;
    options.loop = frames > 1;
    options.seed = seed;
    return makeGif(options);
}

Bytes bmp(std::uint64_t seed, std::int32_t width, std::int32_t height) {
    BmpOptions options;
    options.width = width;
    options.height = height;
    options.seed = seed;
    return makeBmp(options);
}

Bytes webp(WebpKind kind) {
    WebpOptions options;
    options.kind = kind;
    return makeWebp(options);
}

Bytes mp3(std::uint64_t seed, std::size_t frames) {
    Mp3Options options;
    options.frames = frames;
    options.id3v2 = 3;
    options.id3v1 = true;
    options.seed = seed;
    return makeMp3(options);
}

Bytes wav(std::uint64_t seed, std::size_t frames) {
    WavOptions options;
    options.frames = frames;
    options.listInfo = true;
    options.seed = seed;
    return makeWav(options);
}

Bytes m4a(std::uint64_t seed, std::size_t frames) {
    M4aOptions options;
    options.audio.frames = frames;
    options.audio.seed = seed;
    options.metadata = true;
    return makeM4a(options);
}

Bytes adts(std::uint64_t seed, std::size_t frames) {
    AacOptions options;
    options.frames = frames;
    options.seed = seed;
    return makeAdts(options);
}

// Real H.264: the samples and the avcC configuration of a file libx264 wrote
// (FFmpeg, embedded in tests/support/mp4_samples), read with the engine's MP4
// parser. The builder's own video samples are NAL units of made-up bytes,
// which the media level rightly calls corrupted.
struct RealVideo {
    Bytes avcC;
    std::vector<Bytes> samples;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t nalLengthSize = 0;
};

std::uint32_t loadBe32(const Bytes& bytes, std::uint64_t offset) {
    std::uint32_t value = 0;
    for (std::uint64_t i = 0; i < 4; ++i) {
        value = (value << 8) | std::to_integer<std::uint32_t>(bytes.at(static_cast<std::size_t>(offset + i)));
    }
    return value;
}

const RealVideo& realVideo() {
    static const RealVideo video = [] {
        RealVideo out;
        const Bytes file = mp4_samples::named("ffmpeg_video_only.mp4").data();
        carving::MemoryContentReader reader(file);
        Result<formats::mp4::Mp4File> parsed = formats::mp4::parseFile(reader);
        if (!parsed.ok() || !parsed->movie || parsed->movie->tracks.size() != 1 ||
            parsed->movie->tracks.front().samples.descriptions.empty()) {
            throw std::runtime_error("corpus: ffmpeg_video_only.mp4 does not parse");
        }
        const formats::mp4::Track& track = parsed->movie->tracks.front();
        const formats::mp4::SampleDescription& description = track.samples.descriptions.front();
        out.width = description.width;
        out.height = description.height;
        out.nalLengthSize = description.nalLengthSize;
        // The avcC box among the sample entry's children (after its 8-byte
        // header and the 78 bytes of a VisualSampleEntry).
        for (std::uint64_t at = description.offset + 8 + 78; at + 8 <= description.offset + description.size;) {
            const std::uint32_t size = loadBe32(file, at);
            const std::uint32_t type = loadBe32(file, at + 4);
            if (size < 8 || at + size > description.offset + description.size) {
                break;
            }
            if (type == 0x61766343) {  // 'avcC'
                out.avcC.assign(file.begin() + static_cast<std::ptrdiff_t>(at + 8),
                                file.begin() + static_cast<std::ptrdiff_t>(at + size));
            }
            at += size;
        }
        for (const formats::mp4::Chunk& chunk : track.chunks) {
            std::uint64_t offset = chunk.offset;
            for (std::uint32_t i = 0; i < chunk.sampleCount; ++i) {
                const std::uint32_t size = track.samples.sampleSize(chunk.firstSample + i);
                out.samples.emplace_back(file.begin() + static_cast<std::ptrdiff_t>(offset),
                                         file.begin() + static_cast<std::ptrdiff_t>(offset + size));
                offset += size;
            }
        }
        if (out.avcC.empty() || out.samples.empty() || out.nalLengthSize != 4) {
            throw std::runtime_error("corpus: no H.264 samples in ffmpeg_video_only.mp4");
        }
        return out;
    }();
    return video;
}

// An MP4 with an H.264 video track (the real samples, their group of pictures
// repeated `gops` times) and an AAC track. `padding` bytes of filler data (a
// filler NAL unit after each picture, as an encoder pads to a bit rate) make
// the samples as large as a camera's; `seed` varies the padding and the audio,
// so that files of other seeds differ.
Bytes video(std::uint64_t seed, std::size_t gops, bool moovFirst = false, std::size_t padding = 0) {
    const RealVideo& real = realVideo();
    Mp4Options options;
    options.seed = seed;
    options.moov = moovFirst ? Mp4MoovPlace::First : Mp4MoovPlace::Last;
    Mp4TrackOptions track{Mp4TrackKind::Video};
    track.width = real.width;
    track.height = real.height;
    track.codecConfig = real.avcC;
    track.samplesPerChunk = 5;
    std::uint64_t state = seed;
    for (std::size_t gop = 0; gop < gops; ++gop) {
        for (const Bytes& sample : real.samples) {
            Bytes coded = sample;
            if (padding != 0) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                const std::size_t fill = padding + static_cast<std::size_t>((state >> 33) % 1024);
                const std::size_t length = fill + 2;  // NAL header, ff_bytes, rbsp_trailing_bits
                for (int shift = 24; shift >= 0; shift -= 8) {
                    coded.push_back(static_cast<std::byte>((length >> shift) & 0xFF));
                }
                coded.push_back(std::byte{0x0C});  // nal_unit_type 12: filler data
                coded.insert(coded.end(), fill, std::byte{0xFF});
                coded.push_back(std::byte{0x80});
            }
            track.sampleData.push_back(std::move(coded));
        }
    }
    Mp4TrackOptions audio{Mp4TrackKind::Audio};
    audio.samples = gops * real.samples.size() / 2 + 1;
    audio.samplesPerChunk = 5;
    options.tracks = {track, audio};
    if (padding * gops * real.samples.size() > 64 * 1024 * 1024) {
        options.co64 = true;
        options.largeMediaData = true;
    }
    return makeMp4(options).bytes;
}

// A text file (nothing can check its content).
Bytes text(std::uint64_t seed, std::size_t size) {
    static constexpr std::string_view kWords[] = {"card ", "photo ", "holiday ", "beach ", "notes ",
                                                  "list ",  "music ", "video ",   "sun ",   "\r\n"};
    std::string out;
    std::uint64_t state = seed;
    while (out.size() < size) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        out += kWords[(state >> 33) % std::size(kWords)];
    }
    out.resize(size);
    Bytes bytes(out.size());
    std::memcpy(bytes.data(), out.data(), out.size());
    return bytes;
}

// Old data: what a card that has been used holds in its free space. Noise, a
// 64-bit word at a time (xorshift64), so that large images build fast.
Bytes oldData(std::size_t size, std::uint64_t seed) {
    Bytes bytes(size);
    std::uint64_t state = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL;
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        std::memcpy(bytes.data() + i, &state, 8);
    }
    for (; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        bytes[i] = static_cast<std::byte>(state & 0xFF);
    }
    return bytes;
}

template <typename T>
std::vector<T> run(T first, T count) {
    std::vector<T> clusters;
    for (T i = 0; i < count; ++i) {
        clusters.push_back(first + i);
    }
    return clusters;
}

template <typename T>
std::vector<T> join(std::vector<T> a, const std::vector<T>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void copyInto(Bytes& image, std::uint64_t offset, std::span<const std::byte> bytes) {
    if (offset > image.size() || bytes.size() > image.size() - offset) {
        throw std::runtime_error("corpus: bytes placed beyond the image");
    }
    std::copy(bytes.begin(), bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
}

Original named(std::string path, Bytes bytes, Expected condition = Expected::Complete, std::string note = {}) {
    Original original;
    original.path = std::move(path);
    original.bytes = std::move(bytes);
    original.condition = condition;
    original.note = std::move(note);
    return original;
}

Original with(std::string path, Bytes bytes, Outcome outcome, std::string note, bool deepOnly = false) {
    Original original;
    original.path = std::move(path);
    original.bytes = std::move(bytes);
    original.outcome = outcome;
    original.deepOnly = deepOnly || outcome == Outcome::Carved || outcome == Outcome::NamedDamagedCarved;
    original.note = std::move(note);
    return original;
}

// ---------------------------------------------------------------------------
// Volumes that have been used: old data in every free cluster.
// ---------------------------------------------------------------------------

struct FatVolume {
    Fat32ImageBuilder builder;

    FatVolume(const Fat32BuilderOptions& options, std::uint64_t oldData) : builder(options) {
        Bytes& image = builder.raw();
        const auto first = static_cast<std::size_t>(builder.clusterOffset(3));
        copyInto(image, first, corpus::oldData(image.size() - first, oldData));
    }

    [[nodiscard]] std::uint32_t clustersFor(std::size_t bytes) const {
        return bytes == 0 ? 0 : static_cast<std::uint32_t>((bytes - 1) / builder.clusterSize() + 1);
    }
    std::uint32_t folder(std::uint32_t parent, std::string_view name) {
        return builder.addDirectory(parent, name).clusters.front();
    }
    // Data no entry names, from cluster `cluster`.
    void place(std::uint32_t cluster, std::span<const std::byte> bytes) {
        copyInto(builder.raw(), builder.clusterOffset(cluster), bytes);
    }
};

struct ExFatVolume {
    ExFatImageBuilder builder;

    ExFatVolume(const ExFatBuilderOptions& options, std::uint64_t oldData) : builder(options) {
        Bytes& image = builder.raw();
        const Bytes old = corpus::oldData(static_cast<std::size_t>(builder.clusterSize()) * 64, oldData);
        for (std::uint32_t cluster = 2; cluster <= builder.lastCluster(); ++cluster) {
            if (!builder.isAllocated(cluster)) {
                // Each cluster its own old data (the pattern shifted by the cluster).
                const std::size_t shift = (cluster % 61) * builder.clusterSize() / 61;
                const std::size_t take = std::min<std::size_t>(builder.clusterSize(), old.size() - shift);
                copyInto(image, builder.clusterOffset(cluster), std::span<const std::byte>(old).subspan(shift, take));
            }
        }
    }

    [[nodiscard]] std::uint32_t clustersFor(std::size_t bytes) const {
        return bytes == 0 ? 0 : static_cast<std::uint32_t>((bytes - 1) / builder.clusterSize() + 1);
    }
    std::uint32_t folder(std::uint32_t parent, std::string_view name) {
        return builder.addDirectory(parent, name).clusters.front();
    }
    void place(std::uint32_t cluster, std::span<const std::byte> bytes) {
        copyInto(builder.raw(), builder.clusterOffset(cluster), bytes);
    }
};

struct NtfsVolume {
    NtfsImageBuilder builder;

    NtfsVolume(const NtfsBuilderOptions& options, std::uint64_t oldData) : builder(options) {
        Bytes& image = builder.raw();
        const Bytes old = corpus::oldData(static_cast<std::size_t>(builder.clusterSize()) * 64, oldData);
        for (std::uint64_t cluster = 0; cluster < builder.clusterCount(); ++cluster) {
            if (!builder.isAllocated(cluster)) {
                const std::size_t shift = static_cast<std::size_t>(cluster % 61) * builder.clusterSize() / 61;
                const std::size_t take = std::min<std::size_t>(builder.clusterSize(), old.size() - shift);
                copyInto(image, builder.clusterOffset(cluster), std::span<const std::byte>(old).subspan(shift, take));
            }
        }
    }

    [[nodiscard]] std::uint64_t clustersFor(std::size_t bytes) const {
        return bytes == 0 ? 0 : (bytes - 1) / builder.clusterSize() + 1;
    }
    std::uint64_t folder(std::uint64_t parent, std::string_view name) {
        return builder.addDirectory(parent, name).record;
    }
    void place(std::uint64_t cluster, std::span<const std::byte> bytes) {
        copyInto(builder.raw(), builder.clusterOffset(cluster), bytes);
    }
};

// ---------------------------------------------------------------------------
// Disks: volumes laid out from 1 MiB, with old data around them.
// ---------------------------------------------------------------------------

struct Partitioned {
    Bytes disk;
    std::vector<std::uint64_t> offsets;  // of each volume, in bytes
};

struct MbrVolume {
    std::uint8_t type = 0;  // the partition type: 0x0C FAT32, 0x07 exFAT or NTFS
    const Bytes* bytes = nullptr;
};

// An MBR disk: `volumes` as primary partitions, then `logical` ones inside an
// extended partition.
Partitioned mbrDisk(const std::vector<MbrVolume>& volumes, const std::vector<MbrVolume>& logical,
                    std::uint64_t oldData) {
    constexpr std::uint64_t kAlign = kMiB / kSector;  // 2048 sectors
    const auto sectorsOf = [](const Bytes& volume) { return (volume.size() + kSector - 1) / kSector; };
    std::uint64_t lba = kAlign;
    std::vector<MbrEntrySpec> entries;
    Partitioned out;
    std::vector<std::pair<std::uint64_t, const Bytes*>> placed;
    for (const MbrVolume& volume : volumes) {
        entries.push_back(
            {0x00, volume.type, static_cast<std::uint32_t>(lba), static_cast<std::uint32_t>(sectorsOf(*volume.bytes))});
        placed.emplace_back(lba, volume.bytes);
        lba += (sectorsOf(*volume.bytes) + kAlign - 1) / kAlign * kAlign + kAlign;
    }
    std::vector<std::pair<std::uint64_t, std::vector<MbrEntrySpec>>> ebrs;
    if (!logical.empty()) {
        const std::uint64_t extendedStart = lba;
        std::uint64_t ebr = extendedStart;
        for (std::size_t i = 0; i < logical.size(); ++i) {
            const Bytes& volume = *logical[i].bytes;
            const std::uint64_t start = ebr + kAlign;
            placed.emplace_back(start, &volume);
            const std::uint64_t next = start + (sectorsOf(volume) + kAlign - 1) / kAlign * kAlign;
            std::vector<MbrEntrySpec> table = {{0x00, logical[i].type, static_cast<std::uint32_t>(kAlign),
                                                static_cast<std::uint32_t>(sectorsOf(volume))}};
            if (i + 1 < logical.size()) {
                table.push_back({0x00, 0x05, static_cast<std::uint32_t>(next - extendedStart),
                                 static_cast<std::uint32_t>(kAlign + sectorsOf(*logical[i + 1].bytes))});
            }
            ebrs.emplace_back(ebr, std::move(table));
            ebr = next;
        }
        entries.push_back({0x00, 0x0F, static_cast<std::uint32_t>(extendedStart),
                           static_cast<std::uint32_t>(ebr - extendedStart)});
        lba = ebr + kAlign;
    }
    out.disk = corpus::oldData(static_cast<std::size_t>(lba * kSector), oldData);
    for (const auto& [at, volume] : placed) {
        copyInto(out.disk, at * kSector, *volume);
        out.offsets.push_back(at * kSector);
    }
    // A partitioning tool writes the whole table: unused entries are zeros.
    std::fill_n(out.disk.begin() + 446, 64, std::byte{0});
    writeMbrSector(out.disk, kSector, 0, entries);
    for (const auto& [at, table] : ebrs) {
        // An EBR holds nothing but its table: no old data around it.
        std::fill_n(out.disk.begin() + static_cast<std::ptrdiff_t>(at * kSector), kSector, std::byte{0});
        writeMbrSector(out.disk, kSector, at, table);
    }
    return out;
}

struct GptVolume {
    const Bytes* bytes = nullptr;  // nullptr: `sizeBytes` of old data, no filesystem
    std::uint64_t sizeBytes = 0;
    std::u16string name;
};

Partitioned gptDisk(const std::vector<GptVolume>& volumes, std::uint32_t sectorSize, std::uint64_t tail,
                    std::uint64_t oldData) {
    const std::uint64_t align = kMiB / sectorSize;
    std::uint64_t lba = align;
    GptSpec spec;
    spec.sectorSize = sectorSize;
    Partitioned out;
    std::vector<std::pair<std::uint64_t, const Bytes*>> placed;
    std::uint64_t unique = 1;
    for (const GptVolume& volume : volumes) {
        const std::uint64_t bytes = volume.bytes != nullptr ? volume.bytes->size() : volume.sizeBytes;
        const std::uint64_t sectors = (bytes + sectorSize - 1) / sectorSize;
        GptPartitionSpec partition;
        partition.type = partition::gpt_types::kMicrosoftBasicData;
        partition.unique = *partition::Guid::parse("C0FFEE00-0000-4000-8000-00000000000" + std::to_string(unique++));
        partition.firstLba = lba;
        partition.lastLba = lba + sectors - 1;
        partition.name = volume.name;
        spec.partitions.push_back(partition);
        placed.emplace_back(lba, volume.bytes);
        out.offsets.push_back(lba * sectorSize);
        lba += (sectors + align - 1) / align * align + align;
    }
    const std::uint64_t total = lba + tail / sectorSize + 64;
    out.disk = corpus::oldData(static_cast<std::size_t>(total * sectorSize), oldData);
    for (const auto& [at, volume] : placed) {
        if (volume != nullptr) {
            copyInto(out.disk, at * sectorSize, *volume);
        }
    }
    // The tables: no old data in the sectors GPT reserves for them.
    std::fill_n(out.disk.begin(), 34 * sectorSize, std::byte{0});
    std::fill(out.disk.end() - 33 * static_cast<std::ptrdiff_t>(sectorSize), out.disk.end(), std::byte{0});
    (void)writeGpt(out.disk, spec);
    return out;
}

// ---------------------------------------------------------------------------
// The scenarios.
// ---------------------------------------------------------------------------

Fat32BuilderOptions fatGeometry(std::uint8_t sectorsPerCluster, std::uint32_t clusters, std::string label) {
    Fat32BuilderOptions options;
    options.sectorsPerCluster = sectorsPerCluster;
    options.clusterCount = clusters;
    options.label = std::move(label);
    return options;
}

// Normal and small files of every format, in folders.
Image fat32Normal() {
    FatVolume v(fatGeometry(8, 2560, "CARD"), 0x0101);
    Fat32ImageBuilder& b = v.builder;
    std::vector<Original> files;
    const auto add = [&](std::uint32_t folder, const std::string& path, Bytes bytes,
                         Expected condition = Expected::Complete, std::string note = {}) {
        const std::string name = path.substr(path.rfind('/') + 1);
        (void)b.addFile(folder, name, bytes);
        files.push_back(named(path, std::move(bytes), condition, std::move(note)));
    };
    const std::uint32_t root = Fat32ImageBuilder::root();
    const std::uint32_t dcim = v.folder(root, "DCIM");
    const std::uint32_t media = v.folder(dcim, "100MEDIA");
    add(media, "/DCIM/100MEDIA/IMG_0001.JPG", cameraJpeg(101, 320, 240));
    add(media, "/DCIM/100MEDIA/IMG_0002.JPG", progressiveJpeg(102, 256, 192));
    const std::uint32_t pictures = v.folder(root, "Pictures");
    add(pictures, "/Pictures/Sunset Beach.png", png(103, 160, 120));
    add(pictures, "/Pictures/ANIM.GIF", gif(104, 64, 48, 3));
    add(pictures, "/Pictures/ICON.BMP", bmp(105, 64, 48));
    add(pictures, "/Pictures/photo.webp", webp(WebpKind::Lossy));
    add(pictures, "/Pictures/lossless.webp", webp(WebpKind::Lossless));
    const std::uint32_t music = v.folder(root, "Music");
    add(music, "/Music/SONG.MP3", mp3(106, 300));
    add(music, "/Music/Voice memo.wav", wav(107, 20000));
    add(music, "/Music/TRACK.M4A", m4a(108, 120));
    add(music, "/Music/RADIO.AAC", adts(109, 120));
    const std::uint32_t videos = v.folder(root, "Video");
    add(videos, "/Video/CLIP.MP4", video(110, 6, false, 2048));
    // Small files: a 1x1 GIF, a WAV of four frames, a text file, an empty file.
    add(root, "/NOTES.TXT", text(111, 3000), Expected::Unverified, "no format checks text");
    add(root, "/DOT.GIF", gif(112, 1, 1, 1));
    add(root, "/TINY.WAV", wav(113, 4));
    add(root, "/EMPTY.DAT", Bytes{}, Expected::Any, "an empty file");
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// Deleted files: contiguous ones, long and short names, a deleted folder.
Image fat32Deleted() {
    FatVolume v(fatGeometry(8, 2048, "CARD"), 0x0202);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;

    const Bytes keep = cameraJpeg(201, 320, 240);
    (void)b.addFile(root, "KEEP.JPG", keep);
    files.push_back(named("/KEEP.JPG", keep));

    const Bytes shortName = jpeg(202, 240, 180);
    const auto e1 = b.addFile(root, "DELETED.JPG", shortName);
    const Bytes longName = video(203, 8, false, 2048);
    const auto e2 = b.addFile(root, "Deleted video.mp4", longName);
    const Bytes song = mp3(204, 250);
    const auto e3 = b.addFile(root, "DEL_SONG.MP3", song);
    const Bytes recording = wav(205, 30000);
    const auto e4 = b.addFile(root, "Old recording.wav", recording);
    const auto trip = b.addDirectory(root, "Old trip");
    const Bytes beach = jpeg(206, 200, 150);
    const auto e5 = b.addFile(trip.clusters.front(), "Beach day.jpg", beach);
    const Bytes sea = png(207, 128, 96);
    const auto e6 = b.addFile(trip.clusters.front(), "SEA.PNG", sea);
    const Bytes after = gif(208, 48, 48, 2);
    (void)b.addFile(root, "AFTER.GIF", after);
    files.push_back(named("/AFTER.GIF", after));

    // Windows deletes a folder's files before the folder.
    for (const auto& entry : {e1, e2, e3, e4, e5, e6, trip}) {
        b.deleteEntry(entry);
    }
    files.push_back(named("/_ELETED.JPG", shortName, Expected::Complete, "an 8.3 name loses its first letter"));
    files.push_back(named("/Deleted video.mp4", longName));
    files.push_back(named("/_EL_SONG.MP3", song));
    files.push_back(named("/Old recording.wav", recording));
    files.push_back(named("/Old trip/Beach day.jpg", beach));
    files.push_back(named("/Old trip/_EA.PNG", sea));
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// Fragmented files: active ones (the FAT chain says where) and deleted ones
// (fragment reconstruction puts them together).
Image fat32Fragmented() {
    FatVolume v(fatGeometry(8, 3072, "CARD"), 0x0303);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;

    // Active, in three pieces out of order.
    const Bytes frag1 = cameraJpeg(301, 320, 240);
    {
        const std::uint32_t n = v.clustersFor(frag1.size());
        const std::uint32_t a = n / 3;
        const std::uint32_t c = n / 3;
        const std::uint32_t bb = n - a - c;
        (void)b.addFileInClusters(root, "FRAG1.JPG", frag1,
                                  join(join(run<std::uint32_t>(400, a), run<std::uint32_t>(200, bb)),
                                       run<std::uint32_t>(600, c)));
        files.push_back(named("/FRAG1.JPG", frag1));
    }
    // Active MP4 in two pieces.
    const Bytes clip = video(302, 10, false, 2048);
    {
        const std::uint32_t n = v.clustersFor(clip.size());
        (void)b.addFileInClusters(root, "Fragmented clip.mp4", clip,
                                  join(run<std::uint32_t>(900, n / 2), run<std::uint32_t>(800, n - n / 2)));
        files.push_back(named("/Fragmented clip.mp4", clip));
    }
    // Deleted, in two pieces with old data between them.
    const Bytes broken = jpeg(303, 288, 216);
    {
        const std::uint32_t n = v.clustersFor(broken.size());
        const auto e = b.addFileInClusters(root, "Broken trip.jpg", broken,
                                           join(run<std::uint32_t>(1200, n / 2),
                                                run<std::uint32_t>(1200 + n / 2 + 24, n - n / 2)));
        b.deleteEntry(e);
        files.push_back(with("/Broken trip.jpg", broken, Outcome::Named,
                             "deleted in two pieces, 24 clusters of old data apart", true));
    }
    // Deleted, around a file that is still there.
    const Bytes around = jpeg(304, 256, 192);
    const Bytes between = png(305, 96, 72);
    {
        const std::uint32_t n = v.clustersFor(around.size());
        const std::uint32_t m = v.clustersFor(between.size());
        const auto e = b.addFileInClusters(root, "Around.jpg", around,
                                           join(run<std::uint32_t>(1500, n / 2),
                                                run<std::uint32_t>(1500 + n / 2 + m, n - n / 2)));
        (void)b.addFileInClusters(root, "BETWEEN.PNG", between, run<std::uint32_t>(1500 + n / 2, m));
        b.deleteEntry(e);
        files.push_back(with("/Around.jpg", around, Outcome::Named, "deleted, in two pieces around BETWEEN.PNG", true));
        files.push_back(named("/BETWEEN.PNG", between));
    }
    // A deleted WAV around a file that is still there: placed by the allocation (L107).
    const Bytes song = wav(306, 12000);
    const Bytes middle = gif(307, 64, 48, 1);
    {
        const std::uint32_t n = v.clustersFor(song.size());
        const std::uint32_t m = v.clustersFor(middle.size());
        const auto e = b.addFileInClusters(root, "Song around.wav", song,
                                           join(run<std::uint32_t>(1800, n / 2),
                                                run<std::uint32_t>(1800 + n / 2 + m, n - n / 2)));
        (void)b.addFileInClusters(root, "MIDDLE.GIF", middle, run<std::uint32_t>(1800 + n / 2, m));
        b.deleteEntry(e);
        files.push_back(with("/Song around.wav", song, Outcome::Named,
                             "deleted, in two pieces around MIDDLE.GIF; no structure, placed by the allocation (L107)",
                             true));
        files.push_back(named("/MIDDLE.GIF", middle));
    }
    // A deleted MP4 whose moov comes first, in two pieces: placed by its sample tables.
    const Bytes holiday = video(308, 10, true, 2048);
    {
        const std::uint32_t n = v.clustersFor(holiday.size());
        const auto e = b.addFileInClusters(root, "Holiday.mp4", holiday,
                                           join(run<std::uint32_t>(2200, n / 2),
                                                run<std::uint32_t>(2200 + n / 2 + 16, n - n / 2)));
        b.deleteEntry(e);
        files.push_back(with("/Holiday.mp4", holiday, Outcome::Named,
                             "deleted in two pieces, 16 clusters apart; moov first", true));
    }
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// Deleted files whose clusters later files took, in part or whole.
Image fat32Overwritten() {
    FatVolume v(fatGeometry(8, 2048, "CARD"), 0x0404);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;

    // The middle of a deleted JPEG holds a newer BMP.
    const Bytes middle = jpeg(401, 320, 240);
    const Bytes bitmap = bmp(402, 40, 30);
    {
        const std::uint32_t n = v.clustersFor(middle.size());
        const std::uint32_t m = std::min(v.clustersFor(bitmap.size()), n / 3);
        const auto e = b.addFileInClusters(root, "Overwritten middle.jpg", middle, run<std::uint32_t>(300, n));
        b.deleteEntry(e);
        const Bytes fitted(bitmap.begin(), bitmap.begin() + std::min<std::ptrdiff_t>(
                                                                static_cast<std::ptrdiff_t>(bitmap.size()),
                                                                static_cast<std::ptrdiff_t>(m) * b.clusterSize()));
        (void)b.addFileInClusters(root, "NEW1.BMP", fitted,
                                  run<std::uint32_t>(300 + n / 2, v.clustersFor(fitted.size())));
        files.push_back(with("/Overwritten middle.jpg", middle, Outcome::NamedDamaged, "its middle holds NEW1.BMP"));
        files.push_back(named("/NEW1.BMP", fitted, Expected::Any, "a bitmap cut to fit"));
    }
    // The end of a deleted MP4 (its moov) holds a newer PNG.
    const Bytes movie = video(403, 10, false, 2048);
    const Bytes picture = png(404, 64, 48);
    {
        const std::uint32_t n = v.clustersFor(movie.size());
        const std::uint32_t m = v.clustersFor(picture.size());
        const auto e = b.addFileInClusters(root, "Overwritten moov.mp4", movie, run<std::uint32_t>(700, n));
        b.deleteEntry(e);
        (void)b.addFileInClusters(root, "NEW2.PNG", picture, run<std::uint32_t>(700 + n - m, m));
        files.push_back(with("/Overwritten moov.mp4", movie, Outcome::NamedDamaged, "its last clusters hold NEW2.PNG"));
        files.push_back(named("/NEW2.PNG", picture));
    }
    // A deleted WAV entirely overwritten by a larger MP3.
    const Bytes lost = wav(405, 8000);
    const Bytes song = mp3(406, 200);
    {
        const std::uint32_t n = v.clustersFor(lost.size());
        const auto e = b.addFileInClusters(root, "Fully overwritten.wav", lost, run<std::uint32_t>(1100, n));
        b.deleteEntry(e);
        (void)b.addFileInClusters(root, "NEW3.MP3", song, run<std::uint32_t>(1100, v.clustersFor(song.size())));
        files.push_back(with("/Fully overwritten.wav", lost, Outcome::Lost, "NEW3.MP3 took all of its clusters"));
        files.push_back(named("/NEW3.MP3", song));
    }
    // The first cluster of a deleted JPEG holds a newer text file.
    const Bytes start = jpeg(407, 256, 192);
    const Bytes notes = text(408, 2000);
    {
        const std::uint32_t n = v.clustersFor(start.size());
        const auto e = b.addFileInClusters(root, "Overwritten start.jpg", start, run<std::uint32_t>(1500, n));
        b.deleteEntry(e);
        (void)b.addFileInClusters(root, "NOTES.TXT", notes, run<std::uint32_t>(1500, 1));
        files.push_back(
            with("/Overwritten start.jpg", start, Outcome::NamedDamaged, "its first cluster holds NOTES.TXT"));
        files.push_back(named("/NOTES.TXT", notes, Expected::Unverified));
    }
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// Damaged FAT and directory entries: a loop, a cross-link, a chain cut short,
// a cluster number out of range, an entry pointing outside the volume, a size
// beyond the chain, FAT copies that differ, a folder whose cluster was overwritten.
Image fat32DamagedTables() {
    FatVolume v(fatGeometry(8, 2048, "CARD"), 0x0505);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;
    const auto contiguous = [&](std::string_view name, const Bytes& bytes, std::uint32_t first) {
        return b.addFileInClusters(root, name, bytes, run<std::uint32_t>(first, v.clustersFor(bytes.size())));
    };

    const Bytes loop = jpeg(501, 256, 192);
    const auto loopEntry = contiguous("LOOP.JPG", loop, 300);
    b.setFat(loopEntry.clusters[loopEntry.clusters.size() / 2], loopEntry.clusters.front());
    files.push_back(with("/LOOP.JPG", loop, Outcome::Named,
                         "its chain loops back to its start from its middle cluster: a quick scan stops at the loop "
                         "(partial); a deep scan takes the carve's extent (HYBRID)", true));

    const Bytes cross1 = png(502, 128, 96);
    const Bytes cross2 = gif(503, 96, 72, 2);
    const auto c1 = contiguous("CROSS1.PNG", cross1, 400);
    const auto c2 = contiguous("CROSS2.GIF", cross2, 500);
    if (c1.clusters.size() < 2 || c2.clusters.size() < 2) {
        throw std::runtime_error("corpus: cross-linked files need two clusters");
    }
    b.setFat(c2.clusters.front(), c1.clusters[1]);
    files.push_back(named("/CROSS1.PNG", cross1, Expected::Complete, "CROSS2.GIF's chain joins its own"));
    files.push_back(with("/CROSS2.GIF", cross2, Outcome::Named,
                         "its chain runs into CROSS1.PNG's: a quick scan reads the wrong clusters (corrupted); a deep "
                         "scan takes the carve's extent (HYBRID)", true));

    const Bytes cut = video(504, 8, false, 2048);
    const auto cutEntry = contiguous("CUT.MP4", cut, 600);
    b.setFat(cutEntry.clusters[1], 0x0FFFFFFF);
    files.push_back(with("/CUT.MP4", cut, Outcome::Named,
                         "its chain ends after two clusters: partial in a quick scan, HYBRID in a deep one", true));

    const Bytes badCluster = wav(505, 6000);
    const auto badEntry = contiguous("BADCLUS.WAV", badCluster, 800);
    b.setFat(badEntry.clusters[1], 0x0FFFFF00 - 1);
    files.push_back(with("/BADCLUS.WAV", badCluster, Outcome::Named,
                         "its chain names a cluster beyond the volume: partial in a quick scan, HYBRID in a deep one",
                         true));

    const Bytes outside = jpeg(506, 200, 150);
    const auto outsideEntry = contiguous("OUTSIDE.JPG", outside, 1000);
    {
        const std::span<std::byte> entry = b.shortEntry(outsideEntry);
        entry[20] = std::byte{0xF0};
        entry[21] = std::byte{0x0F};
    }
    files.push_back(
        with("/OUTSIDE.JPG", outside, Outcome::Carved,
             "its entry's first cluster is beyond the volume; its chain is still allocated"));

    const Bytes bigger = png(507, 112, 84);
    const auto biggerEntry = contiguous("BIGGER.PNG", bigger, 1200);
    {
        const std::span<std::byte> entry = b.shortEntry(biggerEntry);
        const std::uint32_t size = static_cast<std::uint32_t>(bigger.size()) + 40000;
        for (std::size_t i = 0; i < 4; ++i) {
            entry[28 + i] = static_cast<std::byte>((size >> (8 * i)) & 0xFF);
        }
    }
    files.push_back(with("/BIGGER.PNG", bigger, Outcome::Named,
                         "its entry's size is 40000 bytes too large: partial in a quick scan; a deep scan takes the "
                         "carve's end (HYBRID)", true));

    const Bytes mirror = cameraJpeg(508, 240, 180);
    const auto mirrorEntry = contiguous("MIRROR.JPG", mirror, 1400);
    b.setFatInCopy(1, mirrorEntry.clusters.front(), 0);
    files.push_back(
        named("/MIRROR.JPG", mirror, Expected::Complete, "the second FAT copy differs at its first cluster"));

    // A folder whose cluster later held other data: the files in it lose their entries.
    const auto junk = b.addDirectory(root, "JUNK");
    const Bytes inJunk1 = jpeg(509, 160, 120);
    const Bytes inJunk2 = mp3(510, 150);
    (void)b.addFileInClusters(junk.clusters.front(), "A.JPG", inJunk1,
                              run<std::uint32_t>(1600, v.clustersFor(inJunk1.size())));
    (void)b.addFileInClusters(junk.clusters.front(), "B.MP3", inJunk2,
                              run<std::uint32_t>(1700, v.clustersFor(inJunk2.size())));
    v.place(junk.clusters.front(), makePattern(b.clusterSize(), 511));
    files.push_back(with("/JUNK/A.JPG", inJunk1, Outcome::Carved,
                         "its folder's cluster was overwritten; its chain is still allocated"));
    files.push_back(with("/JUNK/B.MP3", inJunk2, Outcome::Carved,
                         "its folder's cluster was overwritten; its chain is still allocated"));

    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// The first sector of each volume is lost: FAT32, exFAT and NTFS each fall
// back to their backup boot record.
Image damagedBootSectors() {
    std::vector<Original> files;
    FatVolume fat(fatGeometry(8, 1024, "FATVOL"), 0x0601);
    const Bytes fatPhoto = cameraJpeg(601, 256, 192);
    (void)fat.builder.addFile(Fat32ImageBuilder::root(), "FATPHOTO.JPG", fatPhoto);
    files.push_back(named("/FATPHOTO.JPG", fatPhoto, Expected::Complete, "the main boot sector is zeros"));
    Bytes fatBytes = fat.builder.build();
    std::fill_n(fatBytes.begin(), kSector, std::byte{0});

    ExFatBuilderOptions exOptions;
    exOptions.sectorsPerClusterShift = 3;
    exOptions.clusterCount = 1024;
    exOptions.label = "EXVOL";
    ExFatVolume ex(exOptions, 0x0602);
    const Bytes exClip = video(602, 8, false, 2048);
    (void)ex.builder.addFile(ex.builder.root(), "Exfat clip.mp4", exClip);
    files.push_back(named("/Exfat clip.mp4", exClip, Expected::Complete, "the main boot sector is zeros"));
    Bytes exBytes = ex.builder.build();
    std::fill_n(exBytes.begin(), kSector, std::byte{0});

    NtfsBuilderOptions ntOptions;
    ntOptions.sectorsPerCluster = 8;
    ntOptions.clusterCount = 1024;
    ntOptions.label = "NTVOL";
    NtfsVolume nt(ntOptions, 0x0603);
    const Bytes ntSong = mp3(603, 200);
    (void)nt.builder.addFile(NtfsImageBuilder::root(), "ntfs song.mp3", ntSong);
    files.push_back(named("/ntfs song.mp3", ntSong, Expected::Complete, "the main boot sector is zeros"));
    Bytes ntBytes = nt.builder.build();
    std::fill_n(ntBytes.begin(), kSector, std::byte{0});

    Image image;
    image.bytes =
        mbrDisk({MbrVolume{0x0C, &fatBytes}, MbrVolume{0x07, &exBytes}, MbrVolume{0x07, &ntBytes}}, {}, 0x0604).disk;
    image.originals = std::move(files);
    return image;
}

// A FAT32 card quick-formatted after use: a new boot sector, FATs and root
// folder; the old files' data is still there, but nothing names it.
Image fat32QuickFormatted() {
    const Fat32BuilderOptions options = fatGeometry(8, 2048, "CARD");
    FatVolume old(options, 0x0701);
    std::vector<Original> files;
    const std::uint32_t root = Fat32ImageBuilder::root();
    const Bytes photo1 = cameraJpeg(701, 320, 240);
    const Bytes photo2 = jpeg(702, 256, 192);
    const Bytes clip = video(703, 8, false, 2048);
    const Bytes song = mp3(704, 200);
    const Bytes first = png(705, 96, 72);
    // The first file written takes the first clusters; the folder the next free one.
    (void)old.builder.addFileInClusters(root, "FIRST.PNG", first, run<std::uint32_t>(3, old.clustersFor(first.size())));
    const std::uint32_t dcim = old.folder(root, "DCIM");
    (void)old.builder.addFileInClusters(dcim, "IMG_0001.JPG", photo1,
                                        run<std::uint32_t>(200, old.clustersFor(photo1.size())));
    (void)old.builder.addFileInClusters(dcim, "IMG_0002.JPG", photo2,
                                        run<std::uint32_t>(400, old.clustersFor(photo2.size())));
    (void)old.builder.addFileInClusters(root, "CLIP.MP4", clip, run<std::uint32_t>(600, old.clustersFor(clip.size())));
    (void)old.builder.addFileInClusters(root, "SONG.MP3", song, run<std::uint32_t>(900, old.clustersFor(song.size())));
    const Bytes split = jpeg(706, 240, 180);
    {
        const std::uint32_t n = old.clustersFor(split.size());
        (void)old.builder.addFileInClusters(root, "SPLIT.JPG", split,
                                            join(run<std::uint32_t>(1200, n / 2), run<std::uint32_t>(1300, n - n / 2)));
    }
    const Bytes oldImage = old.builder.build();

    // The format: a new volume whose clusters hold what the old one left.
    Fat32ImageBuilder formatted(options);
    copyInto(formatted.raw(), formatted.clusterOffset(3),
             std::span<const std::byte>(oldImage).subspan(static_cast<std::size_t>(formatted.clusterOffset(3))));
    // A file written after the format takes the first clusters: FIRST.PNG's.
    const Bytes newer = gif(707, 64, 48, 2);
    (void)formatted.addFile(root, "NEW.GIF", newer);

    files.push_back(named("/NEW.GIF", newer, Expected::Complete, "written after the format"));
    files.push_back(with("/FIRST.PNG", first, Outcome::Lost, "NEW.GIF took its clusters after the format"));
    files.push_back(with("/DCIM/IMG_0001.JPG", photo1, Outcome::Carved, "the format dropped its entry"));
    files.push_back(with("/DCIM/IMG_0002.JPG", photo2, Outcome::Carved, "the format dropped its entry"));
    files.push_back(with("/CLIP.MP4", clip, Outcome::Carved, "the format dropped its entry"));
    files.push_back(with("/SONG.MP3", song, Outcome::Carved, "the format dropped its entry"));
    files.push_back(with("/SPLIT.JPG", split, Outcome::Lost,
                         "in two pieces, its entry dropped: nothing says where the second piece is"));
    Image image;
    image.bytes = formatted.build();
    image.originals = std::move(files);
    return image;
}

ExFatBuilderOptions exfatGeometry(std::uint8_t clusterShift, std::uint32_t clusters, std::string label,
                                  bool clearFatOnDelete = false) {
    ExFatBuilderOptions options;
    options.sectorsPerClusterShift = clusterShift;
    options.clusterCount = clusters;
    options.label = std::move(label);
    options.clearFatOnDelete = clearFatOnDelete;
    return options;
}

// exFAT: files written in one piece, a fragmented one, deleted ones.
Image exfatMixed() {
    ExFatVolume v(exfatGeometry(6, 512, "EXCARD"), 0x0801);  // 32 KiB clusters, 16 MiB
    ExFatImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint32_t root = b.root();
    const std::uint32_t dcim = v.folder(root, "DCIM");
    const Bytes photo = cameraJpeg(801, 320, 240);
    (void)b.addFile(dcim, "IMG_1001.JPG", photo);
    files.push_back(named("/DCIM/IMG_1001.JPG", photo));
    const Bytes song = mp3(802, 400);
    (void)b.addFile(root, "Song.mp3", song);
    files.push_back(named("/Song.mp3", song));
    const Bytes clip = video(803, 20, false, 2048);
    (void)b.addFile(root, "Clip.mp4", clip);
    files.push_back(named("/Clip.mp4", clip));
    const Bytes webpImage = webp(WebpKind::LossyWithAlpha);
    (void)b.addFile(root, "Sticker.webp", webpImage);
    files.push_back(named("/Sticker.webp", webpImage));

    const Bytes fragmented = video(804, 27, false, 2048);
    {
        const std::uint32_t n = v.clustersFor(fragmented.size());
        (void)b.addFileInClusters(root, "Fragmented.mp4", fragmented,
                                  join(run<std::uint32_t>(300, n / 2), run<std::uint32_t>(200, n - n / 2)));
        files.push_back(named("/Fragmented.mp4", fragmented));
    }
    const Bytes deleted = jpeg(805, 288, 216);
    const auto e1 = b.addFile(root, "Old photo.jpg", deleted);
    b.deleteEntry(e1);
    files.push_back(named("/Old photo.jpg", deleted, Expected::Complete, "deleted, written in one piece"));
    const Bytes deletedChain = wav(806, 40000);
    {
        const std::uint32_t n = v.clustersFor(deletedChain.size());
        const auto e2 = b.addFileInClusters(root, "Deleted chained.wav", deletedChain,
                                            join(run<std::uint32_t>(400, n / 2), run<std::uint32_t>(420, n - n / 2)));
        b.deleteEntry(e2);
        files.push_back(with("/Deleted chained.wav", deletedChain, Outcome::Named,
                             "deleted in two pieces; the FAT chain is left (L12)"));
    }
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// exFAT volumes whose deletion clears FAT chains (and one that keeps them):
// deleted fragmented files need fragment reconstruction.
Image exfatDeletedFragmented() {
    std::vector<Original> files;
    ExFatVolume cleared(exfatGeometry(3, 2048, "CLEARED", true), 0x0901);  // 4 KiB clusters, 8 MiB
    {
        ExFatImageBuilder& b = cleared.builder;
        const Bytes photo = jpeg(901, 320, 240);
        const std::uint32_t n = cleared.clustersFor(photo.size());
        const auto e = b.addFileInClusters(b.root(), "Cleared fragments.jpg", photo,
                                           join(run<std::uint32_t>(300, n / 2),
                                                run<std::uint32_t>(300 + n / 2 + 20, n - n / 2)));
        b.deleteEntry(e);
        files.push_back(with("/Cleared fragments.jpg", photo, Outcome::Named,
                             "deleted in two pieces, 20 clusters apart, its chain cleared", true));
        const Bytes whole = png(902, 160, 120);
        const auto w = b.addFile(b.root(), "Whole.png", whole);
        b.deleteEntry(w);
        files.push_back(named("/Whole.png", whole, Expected::Complete, "deleted, written in one piece"));
        const Bytes kept = mp3(903, 120);
        (void)b.addFile(b.root(), "Kept.mp3", kept);
        files.push_back(named("/Kept.mp3", kept));
    }
    ExFatVolume kept(exfatGeometry(3, 2048, "KEPT", false), 0x0902);
    {
        ExFatImageBuilder& b = kept.builder;
        const Bytes photo = jpeg(904, 256, 192);
        const std::uint32_t n = kept.clustersFor(photo.size());
        const auto e = b.addFileInClusters(b.root(), "Kept chain.jpg", photo,
                                           join(run<std::uint32_t>(500, n / 2),
                                                run<std::uint32_t>(500 + n / 2 + 30, n - n / 2)));
        b.deleteEntry(e);
        files.push_back(with("/Kept chain.jpg", photo, Outcome::Named,
                             "deleted in two pieces, its FAT chain left as it was (L12)", false));
    }
    const Bytes first = cleared.builder.build();
    const Bytes second = kept.builder.build();
    Image image;
    image.bytes = mbrDisk({MbrVolume{0x07, &first}, MbrVolume{0x07, &second}}, {}, 0x0903).disk;
    image.originals = std::move(files);
    return image;
}

// exFAT with damage: an entry set whose checksum is wrong, a data length beyond
// the allocation, a folder whose cluster was overwritten, a broken main boot
// region checksum.
Image exfatDamagedEntries() {
    ExFatVolume v(exfatGeometry(3, 2048, "EXDAMAGE"), 0x0A01);
    ExFatImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint32_t root = b.root();
    const Bytes good = cameraJpeg(1001, 256, 192);
    (void)b.addFile(root, "Good.jpg", good);
    files.push_back(named("/Good.jpg", good, Expected::Complete, "the main boot region's checksum is wrong"));

    const Bytes checksum = png(1002, 128, 96);
    const auto c = b.addFile(root, "Bad checksum.png", checksum);
    b.slot(c, 2)[4] ^= std::byte{0x01};  // a name character changes: the set checksum no longer holds
    files.push_back(named("/B`d checksum.png", checksum, Expected::Complete,
                          "a name character changed: its entry set's checksum does not hold; the name is read as it "
                          "is stored"));

    const Bytes longer = mp3(1003, 150);
    const auto l = b.addFile(root, "Longer.mp3", longer);
    {
        // ValidDataLength and DataLength of the stream extension: 64 KiB more than allocated.
        const std::span<std::byte> stream = b.slot(l, 1);
        const std::uint64_t length = longer.size() + 65536;
        for (std::size_t i = 0; i < 8; ++i) {
            stream[8 + i] = static_cast<std::byte>((length >> (8 * i)) & 0xFF);
            stream[24 + i] = static_cast<std::byte>((length >> (8 * i)) & 0xFF);
        }
        b.rechecksum(l);
    }
    files.push_back(with("/Longer.mp3", longer, Outcome::Named,
                         "its length is 64 KiB beyond its clusters: corrupted in a quick scan; a deep scan takes the "
                         "carve's end (HYBRID)", true));

    const std::uint32_t folder = v.folder(root, "Folder");
    const Bytes inFolder = jpeg(1004, 200, 150);
    (void)b.addFile(folder, "Inside.jpg", inFolder);
    v.place(folder, makePattern(b.clusterSize(), 1005));
    files.push_back(with("/Folder/Inside.jpg", inFolder, Outcome::Carved, "its folder's cluster was overwritten"));

    Bytes bytes = b.build();
    bytes[100] ^= std::byte{0xFF};  // the main boot region: its checksum no longer holds
    Image image;
    image.bytes = std::move(bytes);
    image.originals = std::move(files);
    return image;
}

Image exfatQuickFormatted() {
    const ExFatBuilderOptions options = exfatGeometry(3, 2048, "EXCARD");
    ExFatVolume old(options, 0x0B01);
    std::vector<Original> files;
    const Bytes photo = cameraJpeg(1101, 320, 240);
    const Bytes clip = video(1102, 10, false, 2048);
    const Bytes recording = wav(1103, 20000);
    (void)old.builder.addFileInClusters(old.builder.root(), "Photo.jpg", photo,
                                        run<std::uint32_t>(300, old.clustersFor(photo.size())));
    (void)old.builder.addFileInClusters(old.builder.root(), "Clip.mp4", clip,
                                        run<std::uint32_t>(600, old.clustersFor(clip.size())));
    (void)old.builder.addFileInClusters(old.builder.root(), "Recording.wav", recording,
                                        run<std::uint32_t>(1000, old.clustersFor(recording.size())));
    const Bytes oldImage = old.builder.build();
    ExFatImageBuilder formatted(options);
    for (std::uint32_t cluster = 2; cluster <= formatted.lastCluster(); ++cluster) {
        if (!formatted.isAllocated(cluster)) {
            const auto offset = static_cast<std::size_t>(formatted.clusterOffset(cluster));
            copyInto(formatted.raw(), offset,
                     std::span<const std::byte>(oldImage).subspan(offset, formatted.clusterSize()));
        }
    }
    files.push_back(with("/Photo.jpg", photo, Outcome::Carved, "the format dropped its entry"));
    files.push_back(with("/Clip.mp4", clip, Outcome::Carved, "the format dropped its entry"));
    files.push_back(with("/Recording.wav", recording, Outcome::Carved, "the format dropped its entry"));
    Image image;
    image.bytes = formatted.build();
    image.originals = std::move(files);
    return image;
}

NtfsBuilderOptions ntfsGeometry(std::uint32_t sectorsPerCluster, std::uint64_t clusters, std::string label) {
    NtfsBuilderOptions options;
    options.sectorsPerCluster = sectorsPerCluster;
    options.clusterCount = clusters;
    options.mftRecords = 256;
    options.label = std::move(label);
    return options;
}

// NTFS: resident and non-resident data, fragmented runs, a sparse file, a hard
// link, deleted files and a deleted folder, a record reused by a new file.
Image ntfsMixed() {
    NtfsVolume v(ntfsGeometry(8, 4096, "NTCARD"), 0x0C01);  // 4 KiB clusters, 16 MiB
    NtfsImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint64_t root = NtfsImageBuilder::root();

    const Bytes small = text(1201, 300);
    (void)b.addFile(root, "small.txt", small);
    files.push_back(named("/small.txt", small, Expected::Unverified, "resident in its record"));
    const Bytes dot = gif(1202, 2, 2, 1);
    (void)b.addFile(root, "dot.gif", dot);
    files.push_back(named("/dot.gif", dot, Expected::Complete, "resident in its record"));

    const std::uint64_t photos = v.folder(root, "Photos");
    const Bytes photo = cameraJpeg(1203, 320, 240);
    const auto photoEntry = b.addFile(photos, "IMG_2001.JPG", photo);
    files.push_back(named("/Photos/IMG_2001.JPG", photo));
    b.addHardLink(photoEntry, root, "Link to photo.jpg");
    files.push_back(named("/Link to photo.jpg", photo, Expected::Complete, "a hard link to /Photos/IMG_2001.JPG"));
    const Bytes fragmented = jpeg(1204, 288, 216);
    {
        const std::uint64_t n = v.clustersFor(fragmented.size());
        const std::uint64_t a = n / 3;
        (void)b.addFileInClusters(photos, "fragmented.jpg", fragmented,
                                  join(join(run<std::uint64_t>(2000, a), run<std::uint64_t>(1800, a)),
                                       run<std::uint64_t>(2200, n - 2 * a)));
        files.push_back(named("/Photos/fragmented.jpg", fragmented));
    }
    const std::uint64_t music = v.folder(root, "Music");
    const Bytes song = mp3(1205, 300);
    (void)b.addFile(music, "song.mp3", song);
    files.push_back(named("/Music/song.mp3", song));
    // A sparse WAV: its middle is a hole (zeros) that no cluster holds.
    {
        Bytes sparse = wav(1206, 24000);
        const std::uint64_t n = v.clustersFor(sparse.size());
        const std::uint64_t clusterSize = b.clusterSize();
        std::fill(sparse.begin() + static_cast<std::ptrdiff_t>(4 * clusterSize),
                  sparse.begin() + static_cast<std::ptrdiff_t>(8 * clusterSize), std::byte{0});
        (void)b.addFileWithRuns(music, "sparse.wav", sparse,
                                {NtfsRunSpec{2600, 4}, NtfsRunSpec{std::nullopt, 4}, NtfsRunSpec{2604, n - 8}},
                                0x8000);
        files.push_back(named("/Music/sparse.wav", sparse, Expected::Complete, "sparse: clusters 4-7 are a hole"));
    }
    const std::uint64_t videos = v.folder(root, "Video");
    const Bytes clip = video(1207, 13, false, 2048);
    (void)b.addFile(videos, "clip.mp4", clip);
    files.push_back(named("/Video/clip.mp4", clip));

    // A deleted file whose record a new file took (the lowest free record):
    // only its data is left.
    const Bytes reused = jpeg(1211, 240, 180);
    const auto r = b.addFile(root, "reused record.jpg", reused);
    b.deleteEntry(r);
    const Bytes newer = png(1212, 96, 72);
    (void)b.addFile(root, "newer.png", newer);
    files.push_back(with("/reused record.jpg", reused, Outcome::Carved, "newer.png took its record"));
    files.push_back(named("/newer.png", newer));

    // Deleted: a file, and a folder with its files.
    const Bytes deleted = jpeg(1208, 256, 192);
    const auto d1 = b.addFile(root, "deleted photo.jpg", deleted);
    const auto trip = b.addDirectory(root, "Trip");
    const Bytes tripPhoto = png(1209, 160, 120);
    const auto d2 = b.addFile(trip.record, "beach.png", tripPhoto);
    const Bytes tripSong = mp3(1210, 120);
    const auto d3 = b.addFile(trip.record, "waves.mp3", tripSong);
    b.deleteEntry(d1);
    b.deleteEntry(d2);
    b.deleteEntry(d3);
    b.deleteEntry(trip);
    files.push_back(named("/deleted photo.jpg", deleted, Expected::Complete, "deleted"));
    files.push_back(named("/Trip/beach.png", tripPhoto, Expected::Complete, "deleted with its folder"));
    files.push_back(named("/Trip/waves.mp3", tripSong, Expected::Complete, "deleted with its folder"));

    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// NTFS with damage: a torn FILE record, $MFT's own record damaged (the mirror
// has it), a run list beyond the volume.
Image ntfsDamagedRecords() {
    NtfsVolume v(ntfsGeometry(8, 2048, "NTDAMAGE"), 0x0D01);  // 4 KiB clusters, 8 MiB
    NtfsImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint64_t root = NtfsImageBuilder::root();
    const Bytes good = cameraJpeg(1301, 256, 192);
    (void)b.addFile(root, "good.jpg", good);
    files.push_back(named("/good.jpg", good, Expected::Complete, "$MFT's own record is damaged; $MFTMirr holds it"));
    const Bytes torn = video(1302, 10, false, 2048);
    const auto tornEntry = b.addFile(root, "torn record.mp4", torn);
    files.push_back(
        with("/torn record.mp4", torn, Outcome::Carved,
             "its FILE record was torn (fixup mismatch); its clusters are still allocated"));
    const Bytes runs = png(1303, 128, 96);
    const auto runsEntry = b.addFile(root, "bad runs.png", runs);
    files.push_back(with("/bad runs.png", runs, Outcome::NamedDamagedCarved,
                         "its run list points beyond the volume; its clusters are still allocated"));
    const Bytes song = mp3(1304, 150);
    (void)b.addFile(root, "song.mp3", song);
    files.push_back(named("/song.mp3", song));

    // The run list: the first run's start (LCN) moved far beyond the volume.
    {
        const std::span<std::byte> record = b.record(runsEntry.record);
        const std::size_t attribute = b.attributeOffset(runsEntry.record, 0x80);
        const auto runsOffset = static_cast<std::size_t>(record[attribute + 32]) |
                                (static_cast<std::size_t>(record[attribute + 33]) << 8);
        const std::size_t header = attribute + runsOffset;
        const auto lengthBytes = static_cast<std::size_t>(record[header]) & 0x0F;
        const auto offsetBytes = (static_cast<std::size_t>(record[header]) >> 4) & 0x0F;
        if (offsetBytes == 0) {
            throw std::runtime_error("corpus: expected a run with an offset");
        }
        record[header + 1 + lengthBytes + offsetBytes - 1] = std::byte{0x70};  // a large positive LCN
    }
    Bytes bytes = b.build();
    // Torn: the record's last sector lost its update sequence number.
    {
        const std::uint64_t offset = b.recordOffset(tornEntry.record) + b.recordSize() - 2;
        bytes[static_cast<std::size_t>(offset)] ^= std::byte{0xFF};
    }
    // $MFT's record (0): a sector of it zeroed.
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(b.recordOffset(0)), kSector, std::byte{0});
    Image image;
    image.bytes = std::move(bytes);
    image.originals = std::move(files);
    return image;
}

Image ntfsQuickFormatted() {
    const NtfsBuilderOptions options = ntfsGeometry(8, 2048, "NTCARD");
    NtfsVolume old(options, 0x0E01);
    std::vector<Original> files;
    const Bytes photo = cameraJpeg(1401, 320, 240);
    const Bytes clip = video(1402, 10, false, 2048);
    const Bytes song = mp3(1403, 200);
    (void)old.builder.addFileInClusters(NtfsImageBuilder::root(), "photo.jpg", photo,
                                        run<std::uint64_t>(800, old.clustersFor(photo.size())));
    // $MFTMirr sits in the middle of the volume (cluster 1024).
    (void)old.builder.addFileInClusters(NtfsImageBuilder::root(), "clip.mp4", clip,
                                        run<std::uint64_t>(1100, old.clustersFor(clip.size())));
    (void)old.builder.addFileInClusters(NtfsImageBuilder::root(), "song.mp3", song,
                                        run<std::uint64_t>(1400, old.clustersFor(song.size())));
    const Bytes oldImage = old.builder.build();
    NtfsImageBuilder formatted(options);
    for (std::uint64_t cluster = 0; cluster < formatted.clusterCount(); ++cluster) {
        if (!formatted.isAllocated(cluster)) {
            const auto offset = static_cast<std::size_t>(formatted.clusterOffset(cluster));
            copyInto(formatted.raw(), offset,
                     std::span<const std::byte>(oldImage).subspan(offset, formatted.clusterSize()));
        }
    }
    files.push_back(with("/photo.jpg", photo, Outcome::Carved, "the format wrote a new MFT"));
    files.push_back(with("/clip.mp4", clip, Outcome::Carved, "the format wrote a new MFT"));
    files.push_back(with("/song.mp3", song, Outcome::Carved, "the format wrote a new MFT"));
    Image image;
    image.bytes = formatted.build();
    image.originals = std::move(files);
    return image;
}

// A disk of 4096-byte sectors: GPT and NTFS with 4 KiB sectors and records.
Image ntfs4kSectors() {
    NtfsBuilderOptions options;
    options.bytesPerSector = 4096;
    options.sectorsPerCluster = 1;
    options.clusterCount = 2048;
    options.recordSize = 4096;
    options.mftRecords = 128;
    options.label = "NT4K";
    NtfsVolume v(options, 0x0F01);
    NtfsImageBuilder& b = v.builder;
    std::vector<Original> files;
    const Bytes photo = cameraJpeg(1501, 320, 240);
    (void)b.addFile(NtfsImageBuilder::root(), "photo.jpg", photo);
    files.push_back(named("/photo.jpg", photo));
    const Bytes clip = video(1502, 10, false, 2048);
    (void)b.addFile(NtfsImageBuilder::root(), "clip.mp4", clip);
    files.push_back(named("/clip.mp4", clip));
    const Bytes deleted = png(1503, 160, 120);
    const auto e = b.addFile(NtfsImageBuilder::root(), "deleted.png", deleted);
    b.deleteEntry(e);
    files.push_back(named("/deleted.png", deleted, Expected::Complete, "deleted"));
    const Bytes volume = b.build();
    Image image;
    image.sectorSize = 4096;
    image.bytes = gptDisk({GptVolume{&volume, 0, u"Data"}}, 4096, kMiB, 0x0F02).disk;
    image.originals = std::move(files);
    return image;
}

// Cluster sizes from 512 bytes to 128 KiB on one GPT disk.
Image clusterSizes() {
    std::vector<Original> files;
    FatVolume small(fatGeometry(1, 8192, "FAT512"), 0x1001);
    const Bytes a1 = jpeg(1601, 256, 192);
    const Bytes a2 = video(1602, 7, false, 2048);
    (void)small.builder.addFile(Fat32ImageBuilder::root(), "fat512.jpg", a1);
    (void)small.builder.addFile(Fat32ImageBuilder::root(), "fat512.mp4", a2);
    files.push_back(named("/fat512.jpg", a1, Expected::Complete, "FAT32, 512-byte clusters"));
    files.push_back(named("/fat512.mp4", a2, Expected::Complete, "FAT32, 512-byte clusters"));
    const Bytes v1 = small.builder.build();

    FatVolume large(fatGeometry(64, 160, "FAT32K"), 0x1002);
    const Bytes b1 = cameraJpeg(1603, 320, 240);
    const Bytes b2 = wav(1604, 30000);
    (void)large.builder.addFile(Fat32ImageBuilder::root(), "fat32k.jpg", b1);
    (void)large.builder.addFile(Fat32ImageBuilder::root(), "fat32k.wav", b2);
    files.push_back(named("/fat32k.jpg", b1, Expected::Complete, "FAT32, 32 KiB clusters"));
    files.push_back(named("/fat32k.wav", b2, Expected::Complete, "FAT32, 32 KiB clusters"));
    const Bytes v2 = large.builder.build();

    ExFatVolume ex(exfatGeometry(8, 48, "EX128K"), 0x1003);
    const Bytes c1 = png(1605, 160, 120);
    const Bytes c2 = mp3(1606, 300);
    (void)ex.builder.addFile(ex.builder.root(), "exfat128k.png", c1);
    (void)ex.builder.addFile(ex.builder.root(), "exfat128k.mp3", c2);
    files.push_back(named("/exfat128k.png", c1, Expected::Complete, "exFAT, 128 KiB clusters"));
    files.push_back(named("/exfat128k.mp3", c2, Expected::Complete, "exFAT, 128 KiB clusters"));
    const Bytes v3 = ex.builder.build();

    NtfsVolume nt(ntfsGeometry(128, 96, "NT64K"), 0x1004);
    const Bytes d1 = jpeg(1607, 288, 216);
    const Bytes d2 = video(1608, 10, false, 2048);
    (void)nt.builder.addFile(NtfsImageBuilder::root(), "ntfs64k.jpg", d1);
    (void)nt.builder.addFile(NtfsImageBuilder::root(), "ntfs64k.mp4", d2);
    files.push_back(named("/ntfs64k.jpg", d1, Expected::Complete, "NTFS, 64 KiB clusters"));
    files.push_back(named("/ntfs64k.mp4", d2, Expected::Complete, "NTFS, 64 KiB clusters"));
    const Bytes v4 = nt.builder.build();

    Image image;
    image.bytes = gptDisk({GptVolume{&v1, 0, u"FAT512"}, GptVolume{&v2, 0, u"FAT32K"}, GptVolume{&v3, 0, u"EX128K"},
                           GptVolume{&v4, 0, u"NT64K"}},
                          kSector, kMiB, 0x1005)
                      .disk;
    image.originals = std::move(files);
    return image;
}

// MBR: a primary FAT32 volume, then exFAT and NTFS volumes inside an extended
// partition.
Image mbrLogicalPartitions() {
    std::vector<Original> files;
    FatVolume fat(fatGeometry(8, 1024, "PRIMARY"), 0x1101);
    const Bytes a = cameraJpeg(1701, 256, 192);
    (void)fat.builder.addFile(Fat32ImageBuilder::root(), "primary.jpg", a);
    files.push_back(named("/primary.jpg", a));
    const Bytes aDeleted = mp3(1702, 120);
    const auto e = fat.builder.addFile(Fat32ImageBuilder::root(), "primary deleted.mp3", aDeleted);
    fat.builder.deleteEntry(e);
    files.push_back(named("/primary deleted.mp3", aDeleted, Expected::Complete, "deleted"));
    const Bytes v1 = fat.builder.build();

    ExFatVolume ex(exfatGeometry(3, 1024, "LOGICAL1"), 0x1102);
    const Bytes b = video(1703, 8, false, 2048);
    (void)ex.builder.addFile(ex.builder.root(), "logical exfat.mp4", b);
    files.push_back(named("/logical exfat.mp4", b));
    const Bytes v2 = ex.builder.build();

    NtfsVolume nt(ntfsGeometry(8, 1024, "LOGICAL2"), 0x1103);
    const Bytes c = png(1704, 160, 120);
    (void)nt.builder.addFile(NtfsImageBuilder::root(), "logical ntfs.png", c);
    files.push_back(named("/logical ntfs.png", c));
    const Bytes cDeleted = wav(1705, 10000);
    const auto d = nt.builder.addFile(NtfsImageBuilder::root(), "logical deleted.wav", cDeleted);
    nt.builder.deleteEntry(d);
    files.push_back(named("/logical deleted.wav", cDeleted, Expected::Complete, "deleted"));
    const Bytes v3 = nt.builder.build();

    Image image;
    image.bytes = mbrDisk({MbrVolume{0x0C, &v1}}, {MbrVolume{0x07, &v2}, MbrVolume{0x07, &v3}}, 0x1104).disk;
    image.originals = std::move(files);
    return image;
}

// GPT: FAT32, exFAT and NTFS volumes, a partition with no filesystem whose old
// data holds files, and files in space no partition covers.
Image gptMulti() {
    std::vector<Original> files;
    FatVolume fat(fatGeometry(8, 1024, "GPTFAT"), 0x1201);
    const Bytes a = jpeg(1801, 256, 192);
    (void)fat.builder.addFile(Fat32ImageBuilder::root(), "gpt fat.jpg", a);
    files.push_back(named("/gpt fat.jpg", a));
    const Bytes v1 = fat.builder.build();
    ExFatVolume ex(exfatGeometry(3, 1024, "GPTEX"), 0x1202);
    const Bytes b = mp3(1802, 200);
    (void)ex.builder.addFile(ex.builder.root(), "gpt exfat.mp3", b);
    files.push_back(named("/gpt exfat.mp3", b));
    const Bytes v2 = ex.builder.build();
    NtfsVolume nt(ntfsGeometry(8, 1024, "GPTNT"), 0x1203);
    const Bytes c = video(1803, 8, false, 2048);
    (void)nt.builder.addFile(NtfsImageBuilder::root(), "gpt ntfs.mp4", c);
    files.push_back(named("/gpt ntfs.mp4", c));
    const Bytes v3 = nt.builder.build();

    Partitioned disk = gptDisk({GptVolume{&v1, 0, u"FAT32"}, GptVolume{&v2, 0, u"exFAT"}, GptVolume{&v3, 0, u"NTFS"},
                                GptVolume{nullptr, 2 * kMiB, u"Raw"}},
                               kSector, 2 * kMiB, 0x1204);
    const Bytes rawPhoto = cameraJpeg(1804, 240, 180);
    const Bytes rawSong = mp3(1805, 150);
    copyInto(disk.disk, disk.offsets[3] + 64 * 1024, rawPhoto);
    copyInto(disk.disk, disk.offsets[3] + kMiB, rawSong);
    files.push_back(with("", rawPhoto, Outcome::Carved, "in a partition with no filesystem"));
    files.push_back(with("", rawSong, Outcome::Carved, "in a partition with no filesystem"));
    const Bytes unpartitioned = png(1806, 128, 96);
    // After the last partition, before the backup GPT.
    copyInto(disk.disk, disk.disk.size() - kMiB - 64 * kSector, unpartitioned);
    files.push_back(with("", unpartitioned, Outcome::Carved, "in space no partition covers"));
    Image image;
    image.bytes = std::move(disk.disk);
    image.originals = std::move(files);
    return image;
}

// Unicode and long names, and paths longer than MAX_PATH, on all three filesystems.
Image unicodeLongNames() {
    std::vector<Original> files;
    const std::string longName = std::string(246, 'L') + "_end.jpg";  // 254 characters
    std::string deepPath;
    std::vector<std::string> deepFolders;
    for (int i = 0; i < 8; ++i) {
        deepFolders.push_back("Folder level " + std::to_string(i) + " with a long name to deepen the path");
    }

    FatVolume fat(fatGeometry(8, 1536, "UNIFAT"), 0x1301);
    {
        Fat32ImageBuilder& b = fat.builder;
        const std::uint32_t root = Fat32ImageBuilder::root();
        const auto add = [&](std::uint32_t folder, const std::string& path, const std::string& name, Bytes bytes) {
            (void)b.addFile(folder, name, bytes);
            files.push_back(named(path, std::move(bytes)));
        };
        add(root, "/Ünïcödé фото.jpg", "Ünïcödé фото.jpg", jpeg(1901, 200, 150));
        add(root, "/日本語の写真.png", "日本語の写真.png", png(1902, 96, 72));
        add(root, "/emoji 😀🎉.gif", "emoji 😀🎉.gif", gif(1903, 32, 32, 1));
        add(root, "/" + longName, longName, jpeg(1904, 160, 120));
        std::uint32_t folder = root;
        std::string path;
        for (const std::string& name : deepFolders) {
            folder = fat.folder(folder, name);
            path += "/" + name;
        }
        add(folder, path + "/deep fat file.mp3", "deep fat file.mp3", mp3(1905, 100));
        const Bytes deleted = jpeg(1906, 128, 96);
        const auto e = b.addFile(root, "Gelöscht ✓.jpg", deleted);
        b.deleteEntry(e);
        files.push_back(named("/Gelöscht ✓.jpg", deleted, Expected::Complete, "deleted"));
    }
    const Bytes v1 = fat.builder.build();

    ExFatVolume ex(exfatGeometry(3, 1536, "UNIEX"), 0x1302);
    {
        ExFatImageBuilder& b = ex.builder;
        const auto add = [&](std::uint32_t folder, const std::string& path, const std::string& name, Bytes bytes) {
            (void)b.addFile(folder, name, bytes);
            files.push_back(named(path, std::move(bytes)));
        };
        add(b.root(), "/Ελληνικά.jpg", "Ελληνικά.jpg", jpeg(1911, 200, 150));
        add(b.root(), "/Кириллица.png", "Кириллица.png", png(1912, 96, 72));
        const std::string exName = "x" + longName.substr(1);
        add(b.root(), "/" + exName, exName, jpeg(1913, 160, 120));
        std::uint32_t folder = b.root();
        std::string path;
        for (const std::string& name : deepFolders) {
            folder = ex.folder(folder, name);
            path += "/" + name;
        }
        add(folder, path + "/deep exfat file.mp4", "deep exfat file.mp4", video(1914, 6, false, 2048));
        const Bytes deleted = video(1915, 6, false, 2048);
        const auto e = b.addFile(b.root(), "Supprimé été.mp4", deleted);
        b.deleteEntry(e);
        files.push_back(named("/Supprimé été.mp4", deleted, Expected::Complete, "deleted"));
    }
    const Bytes v2 = ex.builder.build();

    NtfsVolume nt(ntfsGeometry(8, 1536, "UNINT"), 0x1303);
    {
        NtfsImageBuilder& b = nt.builder;
        const auto add = [&](std::uint64_t folder, const std::string& path, const std::string& name, Bytes bytes) {
            (void)b.addFile(folder, name, bytes);
            files.push_back(named(path, std::move(bytes)));
        };
        add(NtfsImageBuilder::root(), "/中文文件.jpg", "中文文件.jpg", jpeg(1921, 200, 150));
        add(NtfsImageBuilder::root(), "/עברית.png", "עברית.png", png(1922, 96, 72));
        add(NtfsImageBuilder::root(), "/Ñandú.wav", "Ñandú.wav", wav(1923, 8000));
        const std::string ntName = "y" + longName.substr(1);
        add(NtfsImageBuilder::root(), "/" + ntName, ntName, jpeg(1924, 160, 120));
        std::uint64_t folder = NtfsImageBuilder::root();
        std::string path;
        for (const std::string& name : deepFolders) {
            folder = nt.folder(folder, name);
            path += "/" + name;
        }
        add(folder, path + "/deep ntfs file.png", "deep ntfs file.png", png(1925, 64, 48));
        const Bytes deleted = jpeg(1926, 128, 96);
        const auto e = b.addFile(NtfsImageBuilder::root(), "Ελληνικά διαγραμμένο.jpg", deleted);
        b.deleteEntry(e);
        files.push_back(named("/Ελληνικά διαγραμμένο.jpg", deleted, Expected::Complete, "deleted"));
    }
    const Bytes v3 = nt.builder.build();

    Image image;
    image.bytes = mbrDisk({MbrVolume{0x0C, &v1}, MbrVolume{0x07, &v2}, MbrVolume{0x07, &v3}}, {}, 0x1304).disk;
    image.originals = std::move(files);
    return image;
}

// The same content under several names: in other folders, deleted, and where
// no entry names it.
Image duplicates() {
    FatVolume v(fatGeometry(8, 2048, "DUPS"), 0x1401);
    Fat32ImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint32_t root = Fat32ImageBuilder::root();
    const Bytes photo = cameraJpeg(2001, 320, 240);
    const Bytes song = mp3(2002, 200);
    const Bytes clip = video(2003, 8, false, 2048);
    const std::uint32_t a = v.folder(root, "A");
    const std::uint32_t bb = v.folder(root, "B");
    (void)b.addFile(a, "PHOTO.JPG", photo);
    (void)b.addFile(bb, "Photo copy.jpg", photo);
    const auto deleted = b.addFile(root, "Deleted copy.jpg", photo);
    b.deleteEntry(deleted);
    (void)b.addFile(a, "SONG.MP3", song);
    (void)b.addFile(bb, "Song (2).mp3", song);
    (void)b.addFile(a, "CLIP.MP4", clip);
    (void)b.addFile(bb, "CLIP.MP4", clip);
    v.place(1800, photo);
    files.push_back(named("/A/PHOTO.JPG", photo));
    files.push_back(named("/B/Photo copy.jpg", photo));
    files.push_back(named("/Deleted copy.jpg", photo, Expected::Complete, "deleted"));
    files.push_back(named("/A/SONG.MP3", song));
    files.push_back(named("/B/Song (2).mp3", song));
    files.push_back(named("/A/CLIP.MP4", clip));
    files.push_back(named("/B/CLIP.MP4", clip));
    files.push_back(with("", photo, Outcome::Carved, "a copy no entry names"));
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// Media files whose content is damaged, while the filesystem is intact.
Image corruptMedia() {
    FatVolume v(fatGeometry(8, 2048, "DAMAGED"), 0x1501);
    Fat32ImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint32_t root = Fat32ImageBuilder::root();
    const auto add = [&](const std::string& name, Bytes bytes, std::string note,
                         Outcome outcome = Outcome::NamedDamaged) {
        (void)b.addFile(root, name, bytes);
        files.push_back(with("/" + name, std::move(bytes), outcome, std::move(note)));
    };
    {
        Bytes truncated = jpeg(2101, 320, 240);
        std::fill(truncated.begin() + static_cast<std::ptrdiff_t>(truncated.size() / 2), truncated.end(), std::byte{0});
        add("TRUNC.JPG", std::move(truncated), "its second half is zeros");
    }
    {
        Bytes huffman = jpeg(2102, 256, 192);
        // Entropy-coded data replaced by other bytes (no 0xFF, so no markers appear).
        const std::size_t from = huffman.size() / 2;
        for (std::size_t i = 0; i < 256; ++i) {
            huffman[from + i] = static_cast<std::byte>((i * 37 + 11) % 0xFF);
        }
        add("BADHUFF.JPG", std::move(huffman), "256 bytes of its scan data replaced");
    }
    {
        Bytes crc = png(2103, 128, 96);
        crc[crc.size() / 2] ^= std::byte{0x40};
        add("BADCRC.PNG", std::move(crc), "a byte of its image data flipped: the chunk's CRC fails");
    }
    {
        Bytes lzw = gif(2104, 96, 72, 1);
        for (std::size_t i = lzw.size() / 2; i < lzw.size() / 2 + 32; ++i) {
            lzw[i] = std::byte{0x00};
        }
        (void)b.addFile(root, "BADLZW.GIF", lzw);
        Original original = named("/BADLZW.GIF", std::move(lzw), Expected::Any,
                                  "32 bytes of its LZW data zeroed: GIF has no checksum, and zero codes still decode "
                                  "(L64)");
        files.push_back(std::move(original));
    }
    {
        Bytes cut = gif(2111, 96, 72, 2);
        cut.resize(cut.size() * 7 / 10);
        add("CUT.GIF", std::move(cut), "cut at 70%: its trailer and last frame are missing");
    }
    {
        Bytes stco = video(2105, 8, false, 2048);
        // Every chunk offset of the first track's table moved beyond the file.
        const std::vector<BoxPosition> boxes = mp4Boxes(stco);
        for (const BoxPosition& box : boxes) {
            if (box.path.ends_with("/stco")) {
                // Header, version and flags, entry count: then the first offset.
                stco[box.offset + 16] = std::byte{0x7F};
                break;
            }
        }
        add("BADSTCO.MP4", std::move(stco), "a chunk offset points beyond the file");
    }
    {
        Bytes garbage = mp3(2106, 300);
        const Bytes noise = makePattern(2048, 2107);
        std::copy(noise.begin(), noise.end(), garbage.begin() + static_cast<std::ptrdiff_t>(garbage.size() / 2));
        add("GARBAGE.MP3", std::move(garbage), "2 KiB of its frames replaced by noise");
    }
    {
        Bytes shortWav = wav(2108, 20000);
        shortWav.resize(shortWav.size() / 2);
        add("SHORT.WAV", std::move(shortWav), "cut in half: its data chunk claims twice what is there");
    }
    {
        Bytes moov = m4a(2109, 120);
        const std::vector<BoxPosition> boxes = m4aBoxes(moov);
        for (const BoxPosition& box : boxes) {
            if (box.path == "moov") {
                std::fill_n(moov.begin() + static_cast<std::ptrdiff_t>(box.offset) + 8, 64, std::byte{0});
                break;
            }
        }
        add("ZEROED.M4A", std::move(moov), "the start of its moov box zeroed");
    }
    const Bytes intact = cameraJpeg(2110, 200, 150);
    (void)b.addFile(root, "INTACT.JPG", intact);
    files.push_back(named("/INTACT.JPG", intact));
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// An image whose source had bad sectors: imaging wrote zeros there and
// recorded them as unreadable.
Image badRegions() {
    FatVolume v(fatGeometry(8, 2048, "BADCARD"), 0x1601);
    Fat32ImageBuilder& b = v.builder;
    std::vector<Original> files;
    const std::uint32_t root = Fat32ImageBuilder::root();
    const Bytes photo = cameraJpeg(2201, 320, 240);
    const Bytes clip = video(2202, 10, false, 2048);
    const Bytes picture = png(2203, 160, 120);
    const Bytes recording = wav(2204, 30000);
    const auto pe = b.addFileInClusters(root, "PHOTO.JPG", photo, run<std::uint32_t>(300, v.clustersFor(photo.size())));
    (void)b.addFileInClusters(root, "CLIP.MP4", clip, run<std::uint32_t>(500, v.clustersFor(clip.size())));
    const auto ce =
        b.addFileInClusters(root, "PICTURE.PNG", picture, run<std::uint32_t>(800, v.clustersFor(picture.size())));
    (void)b.addFileInClusters(root, "RECORD.WAV", recording, run<std::uint32_t>(1000, v.clustersFor(recording.size())));
    Image image;
    image.bytes = b.build();
    const auto unreadable = [&](std::uint64_t offset, std::uint64_t length) {
        std::fill_n(image.bytes.begin() + static_cast<std::ptrdiff_t>(offset), length, std::byte{0});
        image.badRegions.push_back(storage::BadRegion{offset, length, 23});
    };
    // In the FAT's second copy, in the middle of PHOTO.JPG, PICTURE.PNG's first
    // cluster, and free space.
    unreadable(b.fatOffset(1) + 4096, 4096);
    unreadable(b.clusterOffset(pe.clusters[pe.clusters.size() / 2]), 8192);
    unreadable(b.clusterOffset(ce.clusters.front()), 4096);
    unreadable(b.clusterOffset(1600), 65536);
    std::sort(image.badRegions.begin(), image.badRegions.end(),
              [](const storage::BadRegion& x, const storage::BadRegion& y) { return x.offset < y.offset; });
    files.push_back(with("/PHOTO.JPG", photo, Outcome::NamedDamaged, "8 KiB of its middle could not be read"));
    files.push_back(named("/CLIP.MP4", clip));
    files.push_back(with("/PICTURE.PNG", picture, Outcome::NamedDamaged, "its first cluster could not be read"));
    files.push_back(named("/RECORD.WAV", recording));
    image.originals = std::move(files);
    return image;
}

// A nearly full card: files fill it, a few were deleted, and newer files took
// some of their clusters.
Image nearlyFull() {
    FatVolume v(fatGeometry(8, 640, "FULL"), 0x1701);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;
    std::vector<Fat32ImageBuilder::Entry> entries;
    std::vector<Bytes> contents;
    std::vector<std::string> names;
    std::uint32_t next = 3;
    for (int i = 0;; ++i) {
        // Few, large files: the root folder keeps to its one cluster.
        Bytes bytes = (i % 3 == 2) ? mp3(2300 + static_cast<std::uint64_t>(i), 300)
                                   : jpeg(2300 + static_cast<std::uint64_t>(i), 320, 240);
        const std::uint32_t n = v.clustersFor(bytes.size());
        if (next + n > b.lastCluster() - 6) {
            break;
        }
        std::string number = std::to_string(i);
        number.insert(0, 3 - std::min<std::size_t>(3, number.size()), '0');
        const std::string name = (i % 3 == 2) ? "S" + number + ".MP3" : "P" + number + ".JPG";
        entries.push_back(b.addFileInClusters(root, name, bytes, run<std::uint32_t>(next, n)));
        contents.push_back(std::move(bytes));
        names.push_back(name);
        next += n;
    }
    if (entries.size() < 12) {
        throw std::runtime_error("corpus: the nearly full card holds too few files");
    }
    // Delete four; two newer files take the clusters of the first and part of the second.
    const std::vector<std::size_t> deleted = {2, 5, 8, 9};
    for (const std::size_t index : deleted) {
        b.deleteEntry(entries[index]);
    }
    const std::vector<std::uint32_t>& taken = entries[2].clusters;
    const Bytes newer1 = jpeg(2400, 96, 72);
    (void)b.addFileInClusters(root, "NEW1.JPG", newer1,
                              run<std::uint32_t>(taken.front(), v.clustersFor(newer1.size())));
    const std::vector<std::uint32_t>& half = entries[5].clusters;
    const Bytes newer2 = gif(2401, 64, 48, 1);
    const std::uint32_t newer2Clusters = v.clustersFor(newer2.size());
    (void)b.addFileInClusters(root, "NEW2.GIF", newer2,
                              run<std::uint32_t>(half[half.size() / 2], newer2Clusters));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool wasDeleted = std::find(deleted.begin(), deleted.end(), i) != deleted.end();
        std::string path = "/" + names[i];
        if (wasDeleted) {
            path[1] = '_';
        }
        if (i == 2) {
            const bool whollyTaken = newer1.size() >= contents[i].size();
            files.push_back(with(path, contents[i], whollyTaken ? Outcome::Lost : Outcome::NamedDamaged,
                                 "deleted; NEW1.JPG took its first clusters"));
        } else if (i == 5) {
            files.push_back(
                with(path, contents[i], Outcome::NamedDamaged, "deleted; NEW2.GIF took clusters in its middle"));
        } else if (wasDeleted) {
            files.push_back(named(path, contents[i], Expected::Complete, "deleted"));
        } else {
            files.push_back(named(path, contents[i]));
        }
    }
    files.push_back(named("/NEW1.JPG", newer1));
    files.push_back(named("/NEW2.GIF", newer2));
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// No filesystem at all: files in old data, found by their content alone.
Image rawMedia() {
    Image image;
    image.bytes = oldData(static_cast<std::size_t>(12 * kMiB), 0x1801);
    std::vector<Original> files;
    std::uint64_t at = 64 * 1024;
    const auto plant = [&](Bytes bytes, std::string note) {
        copyInto(image.bytes, at, bytes);
        at = (at + bytes.size() + 4 * 1024 + 511) / 512 * 512 + 32 * 1024;
        files.push_back(with("", std::move(bytes), Outcome::Carved, std::move(note)));
    };
    plant(cameraJpeg(2501, 320, 240), "JPEG with Exif and a thumbnail");
    plant(progressiveJpeg(2502, 256, 192), "progressive JPEG");
    plant(png(2503, 160, 120), "PNG");
    plant(gif(2504, 96, 72, 3), "animated GIF");
    plant(bmp(2505, 64, 48), "BMP");
    plant(webp(WebpKind::Lossy), "lossy WebP");
    plant(webp(WebpKind::Animated), "animated WebP");
    plant(mp3(2506, 300), "MP3 with ID3 tags");
    plant(wav(2507, 20000), "WAV");
    plant(m4a(2508, 120), "M4A");
    plant(adts(2509, 120), "ADTS AAC");
    plant(video(2510, 10, false, 2048), "MP4, moov last");
    plant(video(2511, 10, true, 2048), "MP4, moov first");
    // Two files back to back: no gap between them.
    {
        const Bytes first = jpeg(2512, 200, 150);
        const Bytes second = png(2513, 96, 72);
        copyInto(image.bytes, at, first);
        copyInto(image.bytes, at + first.size(), second);
        at += first.size() + second.size() + 32 * 1024;
        files.push_back(with("", first, Outcome::Carved, "a JPEG with a PNG right after it"));
        files.push_back(with("", second, Outcome::Carved, "a PNG right after a JPEG"));
    }
    image.originals = std::move(files);
    return image;
}

// An empty card whose free space holds old data only: nothing may be called complete.
Image emptyNoise() {
    FatVolume v(fatGeometry(8, 2048, "EMPTY"), 0x1901);
    Image image;
    image.bytes = v.builder.build();
    return image;
}

// Large files on exFAT with 128 KiB clusters.
Image largeFiles() {
    ExFatVolume v(exfatGeometry(8, 400, "LARGE"), 0x1A01);  // 128 KiB clusters, 50 MiB
    ExFatImageBuilder& b = v.builder;
    std::vector<Original> files;
    const Bytes wave = wav(2601, 3'000'000);  // about 11.4 MiB
    (void)b.addFile(b.root(), "Concert.wav", wave);
    files.push_back(named("/Concert.wav", wave));
    const Bytes movie = video(2602, 140, false, 16384);  // about 14 MiB
    (void)b.addFile(b.root(), "Movie.mp4", movie);
    files.push_back(named("/Movie.mp4", movie));
    const Bytes panorama = jpeg(2603, 1600, 1200);
    (void)b.addFile(b.root(), "Panorama.jpg", panorama);
    files.push_back(named("/Panorama.jpg", panorama));
    const Bytes deleted = wav(2604, 1'500'000);  // about 5.7 MiB
    const auto e = b.addFile(b.root(), "Deleted big.wav", deleted);
    b.deleteEntry(e);
    files.push_back(named("/Deleted big.wav", deleted, Expected::Complete, "deleted"));
    const Bytes fragmented = video(2605, 60, true, 16384);
    {
        const std::uint32_t n = v.clustersFor(fragmented.size());
        (void)b.addFileInClusters(b.root(), "Fragmented big.mp4", fragmented,
                                  join(run<std::uint32_t>(360, n / 2), run<std::uint32_t>(320, n - n / 2)));
        files.push_back(named("/Fragmented big.mp4", fragmented));
    }
    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// After P20 (the user's production-readiness list): a FAT16 card of an older
// camera, as an MBR disk with one FAT16 partition (type 0x06): files in the
// fixed root and in folders, deleted ones with long and 8.3 names, a deleted
// folder, a video.
Image fat16Card() {
    Fat32BuilderOptions options = fatGeometry(4, 4300, "CAMERA16");  // 2 KiB clusters, about 8.4 MiB
    options.fat16 = true;
    options.reservedSectors = 1;
    FatVolume v(options, 0x1B01);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;

    const Bytes first = cameraJpeg(2701, 320, 240);
    (void)b.addFile(root, "IMG_0001.JPG", first);
    files.push_back(named("/IMG_0001.JPG", first));
    const auto dcim = b.addDirectory(root, "DCIM");
    const auto camera = b.addDirectory(dcim.clusters.front(), "100CANON");
    const Bytes second = cameraJpeg(2702, 256, 192);
    (void)b.addFile(camera.clusters.front(), "IMG_0002.JPG", second);
    files.push_back(named("/DCIM/100CANON/IMG_0002.JPG", second));
    const Bytes clip = video(2703, 8, false, 2048);
    (void)b.addFile(camera.clusters.front(), "MVI_0003.MP4", clip);
    files.push_back(named("/DCIM/100CANON/MVI_0003.MP4", clip));
    const Bytes song = mp3(2704, 200);
    (void)b.addFile(root, "Ringtone song.mp3", song);
    files.push_back(named("/Ringtone song.mp3", song));

    const Bytes shot = jpeg(2705, 240, 180);
    const auto e1 = b.addFile(camera.clusters.front(), "Deleted shot.jpg", shot);
    const Bytes chart = png(2706, 128, 96);
    const auto e2 = b.addFile(root, "CHART.PNG", chart);
    const Bytes voice = wav(2707, 20000);
    const auto e3 = b.addFile(root, "Voice note.wav", voice);
    const auto album = b.addDirectory(root, "Old album");
    const Bytes party = jpeg(2708, 200, 150);
    const auto e4 = b.addFile(album.clusters.front(), "Party.jpg", party);
    for (const auto& entry : {e1, e2, e3, e4, album}) {
        b.deleteEntry(entry);
    }
    files.push_back(named("/DCIM/100CANON/Deleted shot.jpg", shot, Expected::Complete, "deleted, long name"));
    files.push_back(named("/_HART.PNG", chart, Expected::Complete, "deleted: an 8.3 name loses its first letter"));
    files.push_back(named("/Voice note.wav", voice, Expected::Complete, "deleted"));
    files.push_back(named("/Old album/Party.jpg", party, Expected::Complete, "deleted with its folder"));

    const Bytes volume = b.build();
    Image image;
    image.bytes = mbrDisk({MbrVolume{0x06, &volume}}, {}, 0x1B02).disk;
    image.originals = std::move(files);
    return image;
}

// After P20 (the user's decision of 2026-10-14 on L225): a 1.44 MB floppy
// disk, FAT12 without a partition table: files in the fixed root and in a
// folder, a sound file whose chain runs through FAT entries that lie across
// two FAT sectors (341 and 682), a fragmented file, deleted files with long
// and 8.3 names, a deleted folder, and a deleted file in two pieces.
Image fat12Floppy() {
    Fat32BuilderOptions options = fatGeometry(1, 2847, "FLOPPY12");  // 512-byte clusters
    options.fat12 = true;
    options.reservedSectors = 1;
    options.rootEntries = 224;
    options.media = 0xF0;
    FatVolume v(options, 0x1D01);
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;

    const Bytes scan = cameraJpeg(2901, 320, 240);
    (void)b.addFile(root, "SCAN0001.JPG", scan);
    files.push_back(named("/SCAN0001.JPG", scan));
    const Bytes readme = text(2902, 2500);
    (void)b.addFile(root, "README.TXT", readme);
    files.push_back(named("/README.TXT", readme, Expected::Unverified, "no format checks text"));
    const auto documents = b.addDirectory(root, "Documents");
    const Bytes chart = png(2903, 160, 120);
    (void)b.addFile(documents.clusters.front(), "Sales chart.png", chart);
    files.push_back(named("/Documents/Sales chart.png", chart));
    const Bytes tune = mp3(2904, 60);
    (void)b.addFile(root, "Ringtone.mp3", tune);
    files.push_back(named("/Ringtone.mp3", tune));
    const Bytes memo = wav(2905, 55000);  // 430 clusters
    (void)b.addFileInClusters(documents.clusters.front(), "Voice memo.wav", memo,
                              run<std::uint32_t>(320, v.clustersFor(memo.size())));
    files.push_back(named("/Documents/Voice memo.wav", memo, Expected::Complete,
                          "its chain runs through FAT entries 341 and 682, which lie across two FAT sectors"));
    const Bytes pages = jpeg(2906, 288, 216);
    {
        const std::uint32_t n = v.clustersFor(pages.size());
        (void)b.addFileInClusters(documents.clusters.front(), "Fragmented scan.jpg", pages,
                                  join(run<std::uint32_t>(1000, n / 2), run<std::uint32_t>(1100, n - n / 2)));
        files.push_back(named("/Documents/Fragmented scan.jpg", pages, Expected::Complete, "in two pieces"));
    }

    const Bytes shot = jpeg(2907, 240, 180);
    const auto e1 = b.addFile(root, "Deleted shot.jpg", shot);
    const Bytes old = png(2908, 96, 72);
    const auto e2 = b.addFile(root, "OLDCHART.PNG", old);
    const auto letters = b.addDirectory(root, "Old letters");
    const Bytes party = jpeg(2909, 200, 150);
    const auto e3 = b.addFile(letters.clusters.front(), "Party.jpg", party);
    for (const auto& entry : {e1, e2, e3, letters}) {
        b.deleteEntry(entry);
    }
    files.push_back(named("/Deleted shot.jpg", shot, Expected::Complete, "deleted, long name"));
    files.push_back(named("/_LDCHART.PNG", old, Expected::Complete, "deleted: an 8.3 name loses its first letter"));
    files.push_back(named("/Old letters/Party.jpg", party, Expected::Complete, "deleted with its folder"));
    const Bytes trip = jpeg(2910, 256, 192);
    {
        const std::uint32_t n = v.clustersFor(trip.size());
        const auto e = b.addFileInClusters(root, "Broken trip.jpg", trip,
                                           join(run<std::uint32_t>(1500, n / 2),
                                                run<std::uint32_t>(1500 + n / 2 + 12, n - n / 2)));
        b.deleteEntry(e);
        files.push_back(with("/Broken trip.jpg", trip, Outcome::Named,
                             "deleted in two pieces, 12 clusters of old data apart", true));
    }

    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

// After P20 (the user's decision to carve ZIP): archives on a FAT32 stick.
// Named ones (DEFLATE, a DOCX, one with an LZMA member nothing can check),
// deleted ones (an APK, one written with data descriptors), and ones no entry
// names any more: images stored in an intact archive are part of it, while
// the images stored in a damaged one are carved as before.
Image zipArchives() {
    FatVolume v(fatGeometry(8, 3000, "ARCHIVES"), 0x1C01);  // 4 KiB clusters, about 12 MiB
    Fat32ImageBuilder& b = v.builder;
    const std::uint32_t root = Fat32ImageBuilder::root();
    std::vector<Original> files;
    const auto sample = [](std::string_view name) { return zip_samples::named(name).data(); };

    const std::uint32_t backup = v.folder(root, "Backup");
    const Bytes projects = sample("mixed");
    (void)b.addFile(backup, "Projects.zip", projects);
    files.push_back(named("/Backup/Projects.zip", projects, Expected::Complete, "DEFLATE, folders, an empty member"));
    const Bytes report = sample("docx");
    (void)b.addFile(backup, "Report.docx", report);
    files.push_back(named("/Backup/Report.docx", report, Expected::Complete, "a DOCX is a ZIP archive"));
    const Bytes packed = sample("lzma");
    (void)b.addFile(root, "Packed.zip", packed);
    files.push_back(named("/Packed.zip", packed, Expected::Unverified, "an LZMA member: nothing checks it"));

    const Bytes app = sample("apk");
    const auto e1 = b.addFile(root, "My App.apk", app);
    const Bytes streamed = sample("streamed");
    const auto e2 = b.addFile(root, "Stream backup.zip", streamed);
    b.deleteEntry(e1);
    b.deleteEntry(e2);
    files.push_back(named("/My App.apk", app, Expected::Complete, "deleted: an APK signing block"));
    files.push_back(named("/Stream backup.zip", streamed, Expected::Complete, "deleted: data descriptors"));

    const Bytes icons = storedArchive({{"icons/app.png", png(2801, 96, 96)}, {"icons/logo.png", png(2802, 64, 32)}});
    v.place(1200, icons);
    files.push_back(with("", icons, Outcome::Carved, "no entry: two PNGs stored in it, which are part of it"));
    const Bytes photo = png(2803, 120, 90);
    Bytes damaged = storedArchive({{"photo.png", photo}, {"notes.txt", makePattern(3000, 2804)}});
    damaged[damaged.size() - 200] ^= std::byte{0x01};  // in notes.txt: its CRC-32 fails
    v.place(1400, damaged);
    Original broken = with("", damaged, Outcome::Carved, "no entry, a member damaged: never trusted");
    broken.condition = Expected::Any;
    files.push_back(std::move(broken));
    files.push_back(with("", photo, Outcome::Carved, "a PNG stored in the damaged archive: carved as before"));
    const Bytes wide = sample("zip64_end");
    v.place(1600, wide);
    files.push_back(with("", wide, Outcome::Carved, "no entry: ZIP64 end records"));

    Image image;
    image.bytes = b.build();
    image.originals = std::move(files);
    return image;
}

struct Entry {
    Scenario scenario;
    std::function<Image()> make;
};

const std::vector<Entry>& entries() {
    static const std::vector<Entry> all = [] {
        std::vector<Entry> list;
        const auto add = [&](std::string name, std::string summary, std::vector<std::string> categories,
                             std::function<Image()> make) {
            list.push_back(
                Entry{Scenario{std::move(name), std::move(summary), std::move(categories)}, std::move(make)});
        };
        add("fat32-normal", "FAT32 card, 4 KiB clusters: every format in folders, small and empty files",
            {"normal files", "small files", "different cluster sizes"}, fat32Normal);
        add("fat32-deleted", "FAT32: deleted files with short and long names, a deleted folder",
            {"deleted files"}, fat32Deleted);
        add("fat32-fragmented", "FAT32: fragmented files, active (chains) and deleted (reconstruction)",
            {"fragmented files", "deleted files"}, fat32Fragmented);
        add("fat32-overwritten", "FAT32: deleted files partly or wholly overwritten by newer files",
            {"partially overwritten files", "deleted files"}, fat32Overwritten);
        add("fat32-damaged-tables", "FAT32: a chain loop, a cross-link, cut and broken chains, bad entries, FAT copies "
            "that differ, an overwritten folder", {"corrupt filesystem metadata"}, fat32DamagedTables);
        add("damaged-boot-sectors", "MBR with FAT32, exFAT and NTFS whose first sectors are zeros: the backup boot "
            "records", {"corrupt filesystem metadata", "multiple partitions"}, damagedBootSectors);
        add("fat32-quick-formatted", "FAT32 quick-formatted after use, one file written since",
            {"quick-formatted drives"}, fat32QuickFormatted);
        add("exfat-mixed", "exFAT, 32 KiB clusters: contiguous, fragmented and deleted files",
            {"normal files", "deleted files", "fragmented files", "different cluster sizes"}, exfatMixed);
        add("exfat-deleted-fragmented",
            "two exFAT volumes, one clearing FAT chains on delete: deleted fragmented files",
            {"fragmented files", "deleted files", "multiple partitions"}, exfatDeletedFragmented);
        add("exfat-damaged-entries", "exFAT: a bad entry set checksum, a length beyond the clusters, an overwritten "
            "folder, a bad boot region checksum", {"corrupt filesystem metadata"}, exfatDamagedEntries);
        add("exfat-quick-formatted", "exFAT quick-formatted after use", {"quick-formatted drives"},
            exfatQuickFormatted);
        add("ntfs-mixed", "NTFS, 4 KiB clusters: resident, fragmented, sparse, hard-linked and deleted files",
            {"normal files", "small files", "deleted files", "fragmented files"}, ntfsMixed);
        add("ntfs-damaged-records", "NTFS: a torn FILE record, a damaged $MFT record, a run list beyond the volume",
            {"corrupt filesystem metadata"}, ntfsDamagedRecords);
        add("ntfs-quick-formatted", "NTFS quick-formatted after use", {"quick-formatted drives"}, ntfsQuickFormatted);
        add("ntfs-4k-sectors", "GPT disk of 4096-byte sectors with NTFS", {"different cluster sizes"}, ntfs4kSectors);
        add("cluster-sizes", "GPT: FAT32 with 512-byte and 32 KiB clusters, exFAT with 128 KiB, NTFS with 64 KiB",
            {"different cluster sizes", "multiple partitions"}, clusterSizes);
        add("mbr-logical-partitions", "MBR: a primary FAT32 volume, exFAT and NTFS in an extended partition",
            {"multiple partitions", "deleted files"}, mbrLogicalPartitions);
        add("gpt-multi", "GPT: FAT32, exFAT, NTFS, a partition without a filesystem, unpartitioned space",
            {"multiple partitions"}, gptMulti);
        add("unicode-long-names", "Unicode names, 254-character names and paths beyond MAX_PATH on FAT32, exFAT and "
            "NTFS", {"Unicode names", "long names", "multiple partitions"}, unicodeLongNames);
        add("duplicates", "FAT32: the same content under other names, in other folders, deleted, and unnamed",
            {"duplicates"}, duplicates);
        add("corrupt-media", "FAT32: media files whose content is damaged", {"corrupt media"}, corruptMedia);
        add("bad-regions", "FAT32 image with unreadable regions recorded by imaging", {"bad regions"}, badRegions);
        add("nearly-full", "FAT32 card filled to the last clusters, files deleted and partly reused",
            {"nearly full drives", "partially overwritten files"}, nearlyFull);
        add("raw-media", "no filesystem: every format in old data, two files back to back", {"normal files"},
            rawMedia);
        add("empty-noise", "an empty FAT32 card whose free space is old data", {"normal files"}, emptyNoise);
        add("large-files", "exFAT, 128 KiB clusters: files of 5 to 14 MiB, deleted and fragmented",
            {"large files", "different cluster sizes"}, largeFiles);
        add("fat16-card", "MBR with a FAT16 card: the fixed root, folders, deleted files and a deleted folder",
            {"normal files", "deleted files"}, fat16Card);
        add("zip-archives", "FAT32: named, deleted and unnamed ZIP archives (DOCX, APK, ZIP64, LZMA), images stored "
            "in intact and damaged archives", {"normal files", "deleted files", "corrupt media"}, zipArchives);
        add("fat12-floppy", "a 1.44 MB FAT12 floppy disk: the fixed root, a chain through FAT entries across two "
            "sectors, fragmented and deleted files, a deleted folder",
            {"normal files", "deleted files", "fragmented files"}, fat12Floppy);
        return list;
    }();
    return all;
}

std::string hex(const Bytes& bytes) {
    return sha256(bytes).hex();
}

}  // namespace

std::string_view toString(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::Named:
            return "named";
        case Outcome::Carved:
            return "carved";
        case Outcome::NamedDamaged:
            return "named-damaged";
        case Outcome::NamedDamagedCarved:
            return "named-damaged-carved";
        case Outcome::Lost:
            return "lost";
    }
    return "?";
}

std::string_view toString(Expected expected) noexcept {
    switch (expected) {
        case Expected::Complete:
            return "Complete";
        case Expected::Unverified:
            return "Unverified";
        case Expected::Any:
            return "any";
    }
    return "?";
}

const std::vector<std::string>& categories() {
    static const std::vector<std::string> all = {
        "normal files",       "deleted files",
        "large files",        "small files",
        "fragmented files",   "partially overwritten files",
        "corrupt filesystem metadata", "corrupt media",
        "duplicates",         "Unicode names",
        "long names",         "different cluster sizes",
        "nearly full drives", "quick-formatted drives",
        "bad regions",        "multiple partitions",
    };
    return all;
}

const std::vector<Scenario>& scenarios() {
    static const std::vector<Scenario> all = [] {
        std::vector<Scenario> list;
        for (const Entry& entry : entries()) {
            list.push_back(entry.scenario);
        }
        return list;
    }();
    return all;
}

const Scenario* findScenario(std::string_view name) {
    for (const Scenario& scenario : scenarios()) {
        if (scenario.name == name) {
            return &scenario;
        }
    }
    return nullptr;
}

Image generate(const Scenario& scenario) {
    for (const Entry& entry : entries()) {
        if (entry.scenario.name == scenario.name) {
            return entry.make();
        }
    }
    throw std::invalid_argument("corpus: no scenario " + scenario.name);
}

std::string manifest(const Scenario& scenario, const Image& image) {
    std::ostringstream out;
    out << "# RecoveryEngine test corpus scenario (tools/test_image_generator)\n";
    out << "scenario: " << scenario.name << "\n";
    out << "summary: " << scenario.summary << "\n";
    out << "categories:";
    for (std::size_t i = 0; i < scenario.categories.size(); ++i) {
        out << (i == 0 ? " " : ", ") << scenario.categories[i];
    }
    out << "\n";
    out << "image: " << image.bytes.size() << " bytes, " << image.sectorSize << "-byte sectors, sha256 "
        << hex(image.bytes) << "\n";
    for (const storage::BadRegion& region : image.badRegions) {
        out << "unreadable: offset " << region.offset << ", " << region.length << " bytes\n";
    }
    out << "# outcome | condition | deep only | size | sha256 | path | note\n";
    for (const Original& original : image.originals) {
        out << "file: " << toString(original.outcome) << " | " << toString(original.condition) << " | "
            << (original.deepOnly ? "deep" : "any") << " | " << original.bytes.size() << " | " << hex(original.bytes)
            << " | " << (original.path.empty() ? std::string("(no name)") : original.path) << " | " << original.note
            << "\n";
    }
    return out.str();
}

namespace {

Status writeNew(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    Result<storage::DestinationFile> file =
        storage::DestinationFile::open(path, storage::DestinationFile::OpenMode::CreateNew);
    if (!file.ok()) {
        return file.error();
    }
    constexpr std::size_t kChunk = 4 * 1024 * 1024;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
        const std::size_t count = std::min(kChunk, bytes.size() - offset);
        if (Status written = file->writeAt(offset, bytes.subspan(offset, count)); !written.ok()) {
            return written;
        }
    }
    if (Status flushed = file->flush(); !flushed.ok()) {
        return flushed;
    }
    file->close();
    return success();
}

std::span<const std::byte> asBytes(const std::string& text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

}  // namespace

Status write(const std::filesystem::path& folder, const Scenario& scenario, const Image& image, bool originals) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError, "cannot create " + toUtf8(folder) + ": " + ec.message());
    }
    const std::filesystem::path imagePath = folder / (scenario.name + ".img");
    const std::filesystem::path metadataPath = imaging::metadataPathFor(imagePath);
    const std::filesystem::path manifestPath = folder / (scenario.name + ".manifest.txt");
    for (const std::filesystem::path& path : {imagePath, metadataPath, manifestPath}) {
        if (std::filesystem::exists(path, ec)) {
            return makeError(ErrorCode::DestinationError, toUtf8(path) + " exists already: it is never overwritten");
        }
    }
    if (Status written = writeNew(imagePath, image.bytes); !written.ok()) {
        return written;
    }
    if (!image.badRegions.empty() || image.sectorSize != kSector) {
        imaging::ImageMetadata metadata;
        metadata.engineVersion = "test corpus";
        metadata.state = imaging::ImageState::Completed;
        metadata.sourceType = storage::SourceType::Synthetic;
        metadata.sourcePath = "corpus:" + scenario.name;
        metadata.sourceSize = image.bytes.size();
        metadata.sectorSize = image.sectorSize;
        metadata.blockSize = kMiB;
        metadata.bytesCompleted = image.bytes.size();
        metadata.startedUtc = "2024-05-17T11:45:30Z";
        metadata.updatedUtc = "2024-05-17T11:45:30Z";
        metadata.badRegions = image.badRegions;
        if (Status written = imaging::writeImageMetadata(metadataPath, metadata); !written.ok()) {
            return written;
        }
    }
    if (Status written = writeNew(manifestPath, asBytes(manifest(scenario, image))); !written.ok()) {
        return written;
    }
    if (originals) {
        const std::filesystem::path below = folder / (scenario.name + ".originals");
        std::filesystem::create_directories(below, ec);
        if (ec) {
            return makeError(ErrorCode::DestinationError, "cannot create " + toUtf8(below) + ": " + ec.message());
        }
        // By content: names may repeat (duplicates) or be missing (carved files).
        std::map<std::string, const Original*> unique;
        for (const Original& original : image.originals) {
            unique.emplace(hex(original.bytes), &original);
        }
        for (const auto& [digest, original] : unique) {
            if (Status written = writeNew(below / (digest.substr(0, 16) + ".bin"), original->bytes); !written.ok()) {
                return written;
            }
        }
    }
    return success();
}

}  // namespace recovery::test::corpus
