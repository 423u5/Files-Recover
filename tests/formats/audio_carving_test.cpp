// Carving whole images with the audio formats registered (and the image
// formats with them): files of all four formats planted in noise and carved
// back byte for byte, streams that are cut off or damaged carved once (the
// self-synchronizing rule), the tail of a stream whose start was lost,
// files cut off by the end of the source, an overwritten file, floods of
// false signatures, pictures inside audio files, audio in a FAT32 volume
// (active, deleted and fragmented), and a disk image file.

#include "formats/audio_formats.hpp"

#include "carving/format_registry.hpp"
#include "format_test_helpers.hpp"
#include "formats/image_formats.hpp"
#include "storage/disk_image_source.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace recovery::formats {
namespace {

using carving::CarveWarning;
using carving::EndStatus;
using carving::FileCandidate;
using carving::ValidationStatus;
using testing::Bytes;
using testing::carve;
using testing::carveAudio;
using testing::carvedBytes;
using testing::Carved;
using testing::Formats;

struct Planted {
    std::string formatId;
    std::string what;
    Bytes bytes;
};

std::vector<Planted> everyFormat() {
    test::Mp3Options mp3;
    mp3.id3v2 = 3;
    mp3.id3v1 = true;
    test::Mp3Options bare;
    bare.infoTag = test::Mp3InfoTag::None;
    bare.version = test::MpegVersion::Mpeg2;
    bare.sampleRate = 22050;
    bare.bitrate = 0;
    test::WavOptions wav;
    wav.frames = 5000;
    wav.listInfo = true;
    test::M4aOptions m4a;
    m4a.audio.frames = 40;
    test::M4aOptions phone;  // a generic brand, as phones and Windows write it
    phone.majorBrand = "mp42";
    phone.compatibleBrands = {"mp41", "isom"};
    phone.moovFirst = true;
    test::AacOptions aac;
    aac.frames = 40;
    return {
        {"mp3", "an MP3 with tags and a LAME tag", test::makeMp3(mp3)},
        {"mp3", "an MPEG-2 stream without an info tag", test::makeMp3(bare)},
        {"wav", "a WAV with a LIST chunk", test::makeWav(wav)},
        {"m4a", "an M4A with moov last", test::makeM4a(m4a)},
        {"m4a", "an M4A with a generic brand", test::makeM4a(phone)},
        {"aac", "raw AAC", test::makeAdts(aac)},
        {"mp3", "mediafoundation.mp3", test::audio_samples::named("mediafoundation.mp3").data()},
        {"m4a", "mediafoundation_aac.m4a", test::audio_samples::named("mediafoundation_aac.m4a").data()},
        {"wav", "speech_ulaw.wav", test::audio_samples::named("speech_ulaw.wav").data()},
        {"aac", "fdkaac_adts.aac", test::audio_samples::named("fdkaac_adts.aac").data()},
        {"m4a", "ffmpeg_fragmented.m4a", test::audio_samples::named("ffmpeg_fragmented.m4a").data()},
    };
}

const FileCandidate* at(const Carved& carved, std::uint64_t offset, std::string_view format) {
    const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(), [&](const FileCandidate& c) {
        return c.sourceOffset == offset && c.formatId == format;
    });
    return found == carved.candidates.end() ? nullptr : &*found;
}

TEST(AudioCarvingTest, RegistersEveryAudioFormatInPriorityOrder) {
    carving::FormatRegistry registry;
    RECOVERY_ASSERT_OK(registerAudioFormats(registry));
    ASSERT_EQ(registry.size(), 4u);
    const std::vector<std::string> expected = {"mp3", "wav", "m4a", "aac"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(registry.formats()[i]->descriptor().id, expected[i]);
        RECOVERY_EXPECT_OK(carving::validateDescriptor(registry.formats()[i]->descriptor()));
    }
    RECOVERY_EXPECT_ERROR(registerAudioFormats(registry), ErrorCode::InvalidInput);
    EXPECT_EQ(registry.size(), 4u);
    // Images and audio side by side: no id is taken twice.
    RECOVERY_ASSERT_OK(registerImageFormats(registry));
    EXPECT_EQ(registry.size(), 9u);
}

TEST(AudioCarvingTest, CarvesEveryFormatFromANoisyDiskExactly) {
    const std::vector<Planted> files = everyFormat();
    test::VirtualSource source(8 * kMiB);
    source.setNoise(0x51C0FFEE);
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 4096;
    for (const Planted& file : files) {
        offsets.push_back(offsets.size() % 2 == 0 ? offset : offset + 7);
        source.plant(offsets.back(), file.bytes);
        offset += 512 * 1024;
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carve(source, Formats::All);
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].what);
        const FileCandidate* found = at(carved, offsets[i], files[i].formatId);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
        EXPECT_EQ(found->end.status, EndStatus::Found);
        EXPECT_EQ(found->length, files[i].bytes.size());
        EXPECT_TRUE(found->warnings.empty());
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].bytes);
    }
    // Nothing else validated: the noise holds no files.
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), files.size());
    EXPECT_EQ(carved.report.scan.hits,
              carved.report.candidates + carved.report.rejectedTotal() + carved.report.skippedInsideCandidates);
}

TEST(AudioCarvingTest, AStreamIsCarvedOnceWhateverItsVerdict) {
    // Every frame of a stream is a hit. A stream that is cut off or damaged
    // does not validate, and without the self-synchronizing rule every one of
    // its frames would start a candidate of its own.
    test::Mp3Options options;
    options.frames = 200;
    const Bytes file = test::makeMp3(options);
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    Bytes damaged = file;  // one byte of music changed: the LAME tag's music CRC fails
    damaged[frames[100].offset + 200] ^= std::byte{0x40};
    test::AacOptions aac;
    aac.frames = 200;
    const Bytes adts = test::makeAdts(aac);

    test::VirtualSource source(1 * kMiB);
    source.plant(4096, damaged);
    source.plant(256 * 1024, adts);
    const std::uint64_t cut = 1 * kMiB - file.size() / 2;  // the source ends in the middle of this one
    source.plant(cut, file);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveAudio(source);
    std::size_t mp3 = 0;
    std::size_t raw = 0;
    std::string all;
    for (const FileCandidate& candidate : carved.candidates) {
        mp3 += candidate.formatId == "mp3" ? 1 : 0;
        raw += candidate.formatId == "aac" ? 1 : 0;
        all += testing::describe(candidate) + "\n";
    }
    EXPECT_EQ(mp3, 2u) << all;
    EXPECT_EQ(raw, 1u) << all;
    const FileCandidate* invalid = at(carved, 4096, "mp3");
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(invalid->end.status, EndStatus::Found);
    EXPECT_EQ(invalid->length, file.size());
    EXPECT_EQ(invalid->validation.status, ValidationStatus::Invalid) << testing::describe(*invalid);
    const FileCandidate* truncated = at(carved, cut, "mp3");
    ASSERT_NE(truncated, nullptr);
    EXPECT_EQ(truncated->end.status, EndStatus::Truncated) << testing::describe(*truncated);
    EXPECT_TRUE(truncated->hasWarning(CarveWarning::TruncatedBySourceEnd));
    EXPECT_EQ(truncated->validation.status, ValidationStatus::Truncated);
    // The frames inside them were skipped, not carved.
    EXPECT_GE(carved.report.skippedInsideCandidates, 199u + 199u + 90u);
}

TEST(AudioCarvingTest, TheTailOfAStreamWhoseStartWasLostIsCarved) {
    // A stream without an info tag whose first half was overwritten by zeros:
    // the frames that are left are carved as one file from the first whole
    // frame, which validation reports as a stream whose start is missing.
    test::Mp3Options options;
    options.infoTag = test::Mp3InfoTag::None;
    options.frames = 100;
    const Bytes file = test::makeMp3(options);
    const std::vector<test::Mp3Frame> frames = test::mp3Frames(file);
    const std::size_t lost = frames[50].offset - 100;  // the zeros end inside frame 49
    Bytes damaged = file;
    std::fill(damaged.begin(), damaged.begin() + static_cast<std::ptrdiff_t>(lost), std::byte{0});

    test::VirtualSource source(512 * 1024);
    source.plant(8192, damaged);
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carveAudio(source);
    ASSERT_EQ(carved.candidates.size(), 1u);
    const FileCandidate& tail = carved.candidates[0];
    SCOPED_TRACE(testing::describe(tail));
    EXPECT_EQ(tail.sourceOffset, 8192 + frames[50].offset);
    EXPECT_EQ(tail.sourceEnd(), 8192 + file.size());
    EXPECT_EQ(tail.end.status, EndStatus::Found);
    EXPECT_EQ(tail.validation.status, ValidationStatus::Invalid);
    EXPECT_NE(tail.validation.detail.find("start is missing"), std::string::npos);
    EXPECT_GE(carved.report.skippedInsideCandidates, 49u);
}

TEST(AudioCarvingTest, FilesCutOffByTheEndOfTheSourceAreTruncated) {
    test::M4aOptions m4a;
    m4a.moovFirst = true;
    test::AacOptions aac;
    aac.frames = 40;
    const Bytes adts = test::makeAdts(aac);
    const std::vector<std::size_t> adtsFrames = test::adtsFrames(adts);
    const std::vector<std::pair<Planted, std::size_t>> cases = {
        {{"mp3", "an MP3 with a LAME tag", test::makeMp3()}, 0},
        {{"wav", "a WAV", test::makeWav()}, 0},
        {{"m4a", "an M4A with moov first", test::makeM4a(m4a)}, 0},
        {{"aac", "raw AAC cut inside a frame", adts}, adtsFrames[20] + 5},
    };
    for (const auto& [file, keep] : cases) {
        SCOPED_TRACE(file.what);
        const std::uint64_t size = 64 * kKiB;
        const std::uint64_t kept = keep != 0 ? keep : file.bytes.size() / 2;
        const std::uint64_t start = size - kept;
        test::VirtualSource source(size);
        source.plant(start, file.bytes);
        RECOVERY_ASSERT_OK(source.open());
        const Carved carved = carveAudio(source);
        const FileCandidate* candidate = at(carved, start, file.formatId);
        ASSERT_NE(candidate, nullptr);
        SCOPED_TRACE(testing::describe(*candidate));
        EXPECT_EQ(candidate->end.status, EndStatus::Truncated);
        EXPECT_EQ(candidate->length, kept);
        EXPECT_TRUE(candidate->hasWarning(CarveWarning::TruncatedBySourceEnd));
        EXPECT_EQ(candidate->validation.status, ValidationStatus::Truncated);
    }
}

TEST(AudioCarvingTest, AnM4aWhoseMoovWasOverwrittenIsKeptUpToTheBreak) {
    test::M4aOptions options;
    options.audio.frames = 60;
    const Bytes m4a = test::makeM4a(options);  // moov after mdat
    const std::size_t moov = [&] {
        for (const test::BoxPosition& box : test::m4aBoxes(m4a)) {
            if (box.path == "moov") {
                return box.offset;
            }
        }
        return std::size_t{0};
    }();
    ASSERT_NE(moov, 0u);
    const Bytes wav = test::makeWav();
    test::VirtualSource source(1 * kMiB);
    source.plant(4096, m4a);
    source.plant(4096 + moov, wav);  // a new file took the clusters that held moov
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveAudio(source);
    const FileCandidate* first = at(carved, 4096, "m4a");
    ASSERT_NE(first, nullptr);
    SCOPED_TRACE(testing::describe(*first));
    EXPECT_EQ(first->end.status, EndStatus::Broken);
    EXPECT_EQ(first->length, moov);
    EXPECT_NE(first->validation.status, ValidationStatus::Valid);
    const FileCandidate* second = at(carved, 4096 + moov, "wav");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->validation.status, ValidationStatus::Valid);
    EXPECT_TRUE(carvedBytes(source, *second) == wav);
}

TEST(AudioCarvingTest, AFloodOfFalseSignaturesGivesNoValidCandidates) {
    test::VirtualSource source(2 * kMiB);
    source.setNoise(777);
    const auto bytes = [](std::initializer_list<int> values) {
        Bytes out;
        for (const int value : values) {
            out.push_back(static_cast<std::byte>(value));
        }
        return out;
    };
    const std::vector<Bytes> signatures = {
        bytes({'I', 'D', '3', 3, 0, 0, 0, 0, 0x10, 0}),                          // an ID3v2 tag
        bytes({0xFF, 0xFB, 0x90, 0x64, 0, 0}),                                    // an MPEG-1 frame
        bytes({'R', 'I', 'F', 'F', 0x40, 0, 0, 0, 'W', 'A', 'V', 'E'}),           // RIFF WAVE
        bytes({0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '}),           // ftyp
        bytes({0xFF, 0xF1, 0x50, 0x80, 0x2E, 0x7F, 0xFC}),                       // an ADTS header
    };
    std::uint64_t planted = 0;
    for (std::uint64_t offset = 1024; offset + 64 < 2 * kMiB; offset += 1024) {
        source.plant(offset, signatures[static_cast<std::size_t>(planted % signatures.size())]);
        ++planted;
    }
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carve(source, Formats::All);
    EXPECT_GE(carved.report.scan.hits, planted);
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), 0u);
    for (const FileCandidate& candidate : carved.candidates) {
        EXPECT_NE(candidate.validation.status, ValidationStatus::Valid) << testing::describe(candidate);
    }
    // A rejected hit costs a small read.
    EXPECT_LE(carved.report.carveBytesRead, carved.report.scan.hits * 64 * kKiB);
}

TEST(AudioCarvingTest, PicturesInsideAudioFilesBelongToThem) {
    // A cover picture in the ID3v2 tag is a whole JPEG inside the MP3: its
    // own hit is skipped because the MP3 around it validated.
    test::Mp3Options options;
    options.id3v2 = 3;
    options.picture = test::makeJpeg();
    const Bytes file = test::makeMp3(options);
    test::VirtualSource source(512 * 1024);
    source.plant(4096, file);
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carve(source, Formats::All);
    ASSERT_EQ(carved.candidates.size(), 1u);
    EXPECT_EQ(carved.candidates[0].formatId, "mp3");
    EXPECT_EQ(carved.candidates[0].length, file.size());
    EXPECT_EQ(carved.candidates[0].validation.status, ValidationStatus::Valid);
    EXPECT_GE(carved.report.skippedInsideCandidates, 1u);
}

TEST(AudioCarvingTest, CarvesAudioFromAFat32VolumeIncludingDeletedOnes) {
    test::Fat32BuilderOptions volumeOptions;
    volumeOptions.sectorsPerCluster = 4;
    volumeOptions.clusterCount = 4096;
    test::Fat32ImageBuilder builder(volumeOptions);
    const auto music = builder.addDirectory(test::Fat32ImageBuilder::root(), "MUSIC");
    const std::vector<Planted> files = everyFormat();
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string name = "TRACK" + std::to_string(i) + "." + files[i].formatId;
        const auto entry = builder.addFile(music.clusters.front(), name, files[i].bytes);
        if (i % 2 == 0) {
            builder.deleteEntry(entry);
        }
    }
    const Bytes volume = builder.build();
    test::MemoryStorageSource source(volume);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carve(source, Formats::All);
    for (const Planted& file : files) {
        SCOPED_TRACE(file.what);
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(), [&](const FileCandidate& c) {
            return c.formatId == file.formatId && c.length == file.bytes.size() && carvedBytes(source, c) == file.bytes;
        });
        ASSERT_NE(found, carved.candidates.end());
        EXPECT_EQ(found->validation.status, ValidationStatus::Valid) << testing::describe(*found);
    }
}

TEST(AudioCarvingTest, FragmentedFilesInAVolume) {
    test::Fat32BuilderOptions volumeOptions;
    volumeOptions.sectorsPerCluster = 1;
    volumeOptions.clusterCount = 8192;
    test::Fat32ImageBuilder builder(volumeOptions);
    const std::uint32_t clusterSize = builder.clusterSize();
    // The file's clusters are not in one piece: another file sits between them.
    const auto fragment = [&](const Bytes& file, const std::string& name, std::uint32_t first) {
        const auto needed = static_cast<std::uint32_t>((file.size() + clusterSize - 1) / clusterSize);
        std::vector<std::uint32_t> clusters;
        for (std::uint32_t i = 0; i < needed; ++i) {
            clusters.push_back(i < 4 ? first + i : first + 100 + i);
        }
        const auto entry = builder.addFileInClusters(test::Fat32ImageBuilder::root(), name, file, clusters);
        builder.addFileInClusters(test::Fat32ImageBuilder::root(), name + ".GAP", test::makePattern(3 * clusterSize, 5),
                                  {first + 4, first + 5, first + 6});
        builder.deleteEntry(entry);
    };
    test::M4aOptions m4aOptions;
    m4aOptions.audio.frames = 60;
    const Bytes m4a = test::makeM4a(m4aOptions);
    test::Mp3Options mp3Options;
    mp3Options.frames = 60;
    const Bytes mp3 = test::makeMp3(mp3Options);
    test::WavOptions wavOptions;
    wavOptions.frames = 2000;
    const Bytes wav = test::makeWav(wavOptions);
    fragment(m4a, "SONG.M4A", 100);
    fragment(mp3, "SONG.MP3", 1000);
    fragment(wav, "MEMO.WAV", 2000);
    const Bytes volume = builder.build();
    test::MemoryStorageSource source(volume);
    RECOVERY_ASSERT_OK(source.open());

    const Carved carved = carveAudio(source);
    const auto first = [&](std::string_view format) {
        const auto found = std::find_if(carved.candidates.begin(), carved.candidates.end(),
                                        [&](const FileCandidate& c) { return c.formatId == format; });
        EXPECT_NE(found, carved.candidates.end()) << format;
        return found;
    };
    // The M4A and the MP3 are found, but carving them in one piece never gives
    // them back, and their structure says so.
    for (const auto& [format, original] : {std::pair{"m4a", &m4a}, std::pair{"mp3", &mp3}}) {
        const auto found = first(format);
        ASSERT_NE(found, carved.candidates.end());
        SCOPED_TRACE(testing::describe(*found));
        EXPECT_NE(found->validation.status, ValidationStatus::Valid);
        EXPECT_NE(carvedBytes(source, *found), *original);
    }
    // The WAV's samples have no structure: its size field ends it, the gap is
    // inside it, and it validates although its bytes are wrong (L71).
    const auto memo = first("wav");
    ASSERT_NE(memo, carved.candidates.end());
    EXPECT_EQ(memo->length, wav.size());
    EXPECT_EQ(memo->validation.status, ValidationStatus::Valid);
    EXPECT_NE(carvedBytes(source, *memo), wav);
}

TEST(AudioCarvingTest, CarvesFromADiskImageFile) {
    const test::TempDir directory;
    const std::filesystem::path path = directory / "audio.img";
    const std::vector<Planted> files = everyFormat();
    Bytes image(1 * kMiB, std::byte{0});
    std::vector<std::uint64_t> offsets;
    std::uint64_t offset = 512;
    for (const Planted& file : files) {
        offsets.push_back(offset);
        std::copy(file.bytes.begin(), file.bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += file.bytes.size() + 512 - (file.bytes.size() % 512);
    }
    ASSERT_LE(offset, image.size());
    test::writeFile(path, image);

    storage::DiskImageSource source(path);
    RECOVERY_ASSERT_OK(source.open());
    const Carved carved = carveAudio(source);
    EXPECT_EQ(carved.report.count(ValidationStatus::Valid), files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        SCOPED_TRACE(files[i].what);
        const FileCandidate* found = at(carved, offsets[i], files[i].formatId);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->length, files[i].bytes.size());
        EXPECT_TRUE(carvedBytes(source, *found) == files[i].bytes);
    }
}

}  // namespace
}  // namespace recovery::formats
