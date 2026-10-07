// Invalid media (P17): metadata is read from content that is damaged, cut
// short, random, or built to mislead (counts and offsets that point outside,
// tags that claim more than they hold). Extraction never fails because of
// the content, never reads or hands out anything outside it, keeps its
// issues bounded, and reports problems as issues.

#include "metadata/media_metadata.hpp"

#include "carving/content_reader.hpp"
#include "metadata_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/audio_builders.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>

namespace recovery::metadata {
namespace {

using test::Bytes;
using test::extract;
using test::issuesText;

struct File {
    std::string name;
    std::string format;
    Bytes bytes;
};

// Files of every format, with the structures metadata reads: Exif and its
// thumbnail, animations, tags of every kind, cover art, fragments.
const std::vector<File>& files() {
    static const std::vector<File> all = [] {
        std::vector<File> made;
        ::recovery::test::JpegOptions jpeg;
        jpeg.exifThumbnail = true;
        jpeg.exifOrientation = 8;
        jpeg.exifMake = "Make";
        jpeg.exifModel = "Model";
        jpeg.exifDateTaken = "2020:01:02 03:04:05";
        jpeg.exifOffsetTime = "-03:00";
        made.push_back({"exif.jpg", "jpeg", ::recovery::test::makeJpeg(jpeg)});
        jpeg = {};
        jpeg.progressive = true;
        jpeg.restartInterval = 2;
        made.push_back({"progressive.jpg", "jpeg", ::recovery::test::makeJpeg(jpeg)});
        ::recovery::test::PngOptions png;
        png.animated = true;
        png.textChunks = true;
        made.push_back({"animated.png", "png", ::recovery::test::makePng(png)});
        ::recovery::test::GifOptions gif;
        gif.frames = 3;
        gif.loop = true;
        gif.comment = "comment";
        made.push_back({"animated.gif", "gif", ::recovery::test::makeGif(gif)});
        ::recovery::test::BmpOptions bmp;
        bmp.header = ::recovery::test::BmpHeader::V5;
        bmp.compression = ::recovery::test::BmpCompression::Bitfields;
        bmp.bitsPerPixel = 32;
        made.push_back({"bitfields.bmp", "bmp", ::recovery::test::makeBmp(bmp)});
        ::recovery::test::WebpOptions webp;
        webp.kind = ::recovery::test::WebpKind::Animated;
        made.push_back({"animated.webp", "webp", ::recovery::test::makeWebp(webp)});
        webp = {};
        webp.kind = ::recovery::test::WebpKind::LossyWithAlpha;
        webp.exifSize = 30;
        made.push_back({"alpha.webp", "webp", ::recovery::test::makeWebp(webp)});
        ::recovery::test::Mp3Options mp3;
        mp3.id3v2 = 3;
        mp3.picture = ::recovery::test::makePng({});
        mp3.id3v1 = true;
        mp3.frames = 8;
        made.push_back({"tagged.mp3", "mp3", ::recovery::test::makeMp3(mp3)});
        mp3 = {};
        mp3.id3v2 = 4;
        mp3.infoTag = ::recovery::test::Mp3InfoTag::None;
        mp3.frames = 8;
        made.push_back({"untagged.mp3", "mp3", ::recovery::test::makeMp3(mp3)});
        ::recovery::test::AacOptions adts;
        adts.id3v2 = 2;
        adts.id3v1 = true;
        adts.frames = 8;
        made.push_back({"tagged.aac", "aac", ::recovery::test::makeAdts(adts)});
        ::recovery::test::WavOptions wav;
        wav.listInfo = true;
        wav.frames = 200;
        wav.fact = true;
        made.push_back({"info.wav", "wav", ::recovery::test::makeWav(wav)});
        ::recovery::test::M4aOptions m4a;
        m4a.metadata = true;
        m4a.audio.frames = 8;
        made.push_back({"tagged.m4a", "m4a", ::recovery::test::makeM4a(m4a)});
        ::recovery::test::Mp4Options mp4;
        mp4.tracks[0].samples = 6;
        mp4.tracks[1].samples = 6;
        const Bytes udta = test::userData({test::ilstItem("\xA9" "nam", 1, test::text("Title")),
                                           test::ilstItem("covr", 13, ::recovery::test::makeJpeg({}))});
        made.push_back({"tagged.mp4", "mp4", test::withMoovChild(::recovery::test::makeMp4(mp4).bytes, udta)});
        mp4.moov = ::recovery::test::Mp4MoovPlace::First;
        mp4.fragmentSamples = 2;
        made.push_back({"fragmented.mp4", "mp4", ::recovery::test::makeMp4(mp4).bytes});
        return made;
    }();
    return all;
}

// What holds for any content: previews inside it, issues bounded.
void expectSane(const MediaMetadata& metadata, std::uint64_t size, const std::string& context) {
    EXPECT_LE(metadata.issues.size(), MediaMetadata::kMaxIssues) << context;
    EXPECT_GE(metadata.issueCount, metadata.issues.size()) << context;
    for (const PreviewSource& preview : metadata.previews) {
        EXPECT_GT(preview.length, 0U) << context;
        EXPECT_LE(preview.offset, size) << context;
        EXPECT_LE(preview.length, size - std::min<std::uint64_t>(preview.offset, size)) << context;
    }
    for (const MetadataIssue& issue : metadata.issues) {
        EXPECT_FALSE(issue.detail.empty()) << context;
    }
    if (metadata.duration.has_value()) {
        EXPECT_GE(metadata.duration->count(), 0) << context;
    }
}

TEST(MetadataRobustnessTest, IntactFilesHaveNoIssues) {
    for (const File& file : files()) {
        const MediaMetadata metadata = extract(file.bytes, file.format);
        // The WebP's EXIF chunk holds pattern bytes, not TIFF: its one issue.
        EXPECT_EQ(metadata.issueCount, file.name == "alpha.webp" ? 1U : 0U)
            << file.name << "\n" << issuesText(metadata);
        EXPECT_FALSE(metadata.previews.empty()) << file.name;
        expectSane(metadata, file.bytes.size(), file.name);
    }
}

TEST(MetadataRobustnessTest, GarbageAndEmptyContentAreIssues) {
    std::mt19937_64 random(0x6A2BA6E);
    for (const std::string_view format : {"jpeg", "png", "gif", "bmp", "webp", "mp3", "aac", "wav", "m4a", "mp4"}) {
        Bytes garbage(777);
        for (std::byte& b : garbage) {
            b = static_cast<std::byte>(random() & 0xFF);
        }
        for (const Bytes& content : {garbage, Bytes{}}) {
            const MediaMetadata metadata = extract(content, format);
            EXPECT_EQ(metadata.kind, kindOfFormat(format)) << format;
            EXPECT_FALSE(metadata.image.has_value()) << format;
            EXPECT_FALSE(metadata.audio.has_value()) << format;
            EXPECT_FALSE(metadata.video.has_value()) << format;
            EXPECT_TRUE(metadata.previews.empty()) << format;
            EXPECT_GE(metadata.issueCount, 1U) << format;
            expectSane(metadata, content.size(), std::string(format));
        }
    }
}

TEST(MetadataRobustnessTest, TruncationAtEveryPosition) {
    for (const File& file : files()) {
        const std::size_t step = std::max<std::size_t>(1, file.bytes.size() / 3000);
        for (std::size_t size = 0; size < file.bytes.size(); size += step) {
            const std::span<const std::byte> cut(file.bytes.data(), size);
            const MediaMetadata metadata = extract(cut, file.format);
            expectSane(metadata, size, file.name + " cut at " + std::to_string(size));
        }
    }
}

TEST(MetadataRobustnessTest, MutationFuzzing) {
    std::mt19937_64 random(0xF0221);
    for (const File& file : files()) {
        for (int round = 0; round < 300; ++round) {
            Bytes mutated = file.bytes;
            const int edits = 1 + static_cast<int>(random() % 8);
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = random() % mutated.size();
                switch (random() % 4) {
                    case 0:
                        mutated[at] = static_cast<std::byte>(random() & 0xFF);
                        break;
                    case 1:
                        mutated[at] ^= static_cast<std::byte>(1U << (random() % 8));
                        break;
                    case 2:
                        // A large count or size: four 0xFF bytes.
                        for (std::size_t i = at; i < std::min(mutated.size(), at + 4); ++i) {
                            mutated[i] = std::byte{0xFF};
                        }
                        break;
                    default:
                        for (std::size_t i = at; i < std::min(mutated.size(), at + 4); ++i) {
                            mutated[i] = std::byte{0};
                        }
                        break;
                }
            }
            const MediaMetadata metadata = extract(mutated, file.format);
            expectSane(metadata, mutated.size(), file.name + " round " + std::to_string(round));
        }
    }
}

// An Exif block put together by hand: little-endian TIFF, IFD0 with the
// given entries (tag, type, count, value or offset), then `extra` bytes.
Bytes exifJpeg(const std::vector<std::array<std::uint32_t, 4>>& entries, std::uint32_t next = 0,
               const Bytes& extra = {}) {
    Bytes tiff = test::text("II");
    tiff.resize(8);
    storeLe16(tiff, 2, 42);
    storeLe32(tiff, 4, 8);
    const std::size_t ifd = tiff.size();
    tiff.resize(ifd + 2 + 12 * entries.size() + 4);
    storeLe16(tiff, ifd, static_cast<std::uint16_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t at = ifd + 2 + 12 * i;
        storeLe16(tiff, at, static_cast<std::uint16_t>(entries[i][0]));
        storeLe16(tiff, at + 2, static_cast<std::uint16_t>(entries[i][1]));
        storeLe32(tiff, at + 4, entries[i][2]);
        storeLe32(tiff, at + 8, entries[i][3]);
    }
    storeLe32(tiff, ifd + 2 + 12 * entries.size(), next);
    tiff.insert(tiff.end(), extra.begin(), extra.end());
    Bytes payload = test::text(std::string_view("Exif\0\0", 6));
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    // SOI, APP1 with the block, then a whole JPEG's segments after its SOI.
    Bytes out = {std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE1}};
    out.push_back(static_cast<std::byte>((payload.size() + 2) >> 8));
    out.push_back(static_cast<std::byte>((payload.size() + 2) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    const Bytes image = ::recovery::test::makeJpeg({});
    out.insert(out.end(), image.begin() + 2, image.end());
    return out;
}

TEST(MetadataRobustnessTest, DamagedExifIsIssues) {
    // Orientation 9 does not exist; a make whose text lies outside the block.
    MediaMetadata metadata = extract(exifJpeg({{0x010F, 2, 50, 4000}, {0x0112, 3, 1, 9}}), "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_FALSE(metadata.image->orientation.has_value());
    EXPECT_TRUE(metadata.image->cameraMake.empty());
    EXPECT_EQ(metadata.issueCount, 2U) << issuesText(metadata);

    // An Exif IFD pointer outside the block, and an IFD1 whose thumbnail runs past it.
    metadata = extract(exifJpeg({{0x8769, 4, 1, 0x7FFFFFF0}}, 26, [] {
                           Bytes ifd1(2 + 24 + 4);
                           storeLe16(ifd1, 0, 2);
                           storeLe16(ifd1, 2, 0x0201);
                           storeLe16(ifd1, 4, 4);
                           storeLe32(ifd1, 6, 1);
                           storeLe32(ifd1, 10, 40);
                           storeLe16(ifd1, 14, 0x0202);
                           storeLe16(ifd1, 16, 4);
                           storeLe32(ifd1, 18, 1);
                           storeLe32(ifd1, 22, 100000);
                           return ifd1;
                       }()),
                       "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_EQ(metadata.previews.size(), 1U);
    EXPECT_EQ(metadata.issueCount, 2U) << issuesText(metadata);

    // An entry count that does not fit, and IFD1 pointing back at IFD0.
    Bytes looped = exifJpeg({{0x0112, 3, 1, 3}}, 8);
    metadata = extract(looped, "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_EQ(metadata.image->orientation, Orientation::Rotate180);
    EXPECT_EQ(metadata.issueCount, 0U) << issuesText(metadata);
    const std::size_t count = test::find(looped, test::text("II")) + 8;
    storeLe16(looped, count, 5000);
    metadata = extract(looped, "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_GE(metadata.issueCount, 1U) << issuesText(metadata);

    // Not a TIFF header at all.
    Bytes notTiff = exifJpeg({});
    notTiff[test::find(notTiff, test::text("II"))] = std::byte{'X'};
    metadata = extract(notTiff, "jpeg");
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);

    // A block larger than the tag limit is not read.
    MetadataOptions limits;
    limits.maxTagBytes = 16;
    metadata = extract(exifJpeg({{0x0112, 3, 1, 6}}), "jpeg", limits);
    ASSERT_TRUE(metadata.image.has_value());
    EXPECT_FALSE(metadata.image->orientation.has_value());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);
}

TEST(MetadataRobustnessTest, DamagedId3TagsAreIssues) {
    const Bytes frames = ::recovery::test::makeMp3({});
    const auto mp3 = [&](Bytes tag) {
        tag.insert(tag.end(), frames.begin(), frames.end());
        return tag;
    };
    const Bytes title = test::id3Text(0, test::text("Title"));

    // A frame whose size runs past the tag: the text there is read, an issue.
    Bytes tag = test::id3Tag(3, {{"TIT2", title}}, 0);
    storeBe32(tag, 14, 0x7FFF);
    MediaMetadata metadata = extract(mp3(tag), "mp3");
    EXPECT_EQ(metadata.tags.title, "Title");
    EXPECT_GE(metadata.issueCount, 1U) << issuesText(metadata);
    ASSERT_TRUE(metadata.audio.has_value());

    // A frame id that is not one ends the walk.
    tag = test::id3Tag(3, {{"T!T2", title}, {"TPE1", title}});
    metadata = extract(mp3(tag), "mp3");
    EXPECT_TRUE(metadata.tags.artist.empty());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);

    // An unknown text encoding.
    tag = test::id3Tag(4, {{"TIT2", test::id3Text(9, test::text("x"))}});
    metadata = extract(mp3(tag), "mp3");
    EXPECT_TRUE(metadata.tags.title.empty());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);

    // A tag that claims more than the content holds.
    tag = test::id3Tag(3, {{"TIT2", title}});
    tag[9] = std::byte{0x7F};
    tag[8] = std::byte{0x7F};
    metadata = extract(tag, "mp3");
    EXPECT_EQ(metadata.tags.title, "Title");
    EXPECT_FALSE(metadata.audio.has_value());
    EXPECT_GE(metadata.issueCount, 2U) << issuesText(metadata);

    // An unsynchronised tag beyond the tag limit is not read.
    MetadataOptions limits;
    limits.maxTagBytes = 8;
    metadata = extract(mp3(test::id3Tag(3, {{"TIT2", title}}, 16, true)), "mp3", limits);
    EXPECT_TRUE(metadata.tags.title.empty());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);

    // A picture frame whose picture is not one.
    Bytes apic = {std::byte{0}};
    const Bytes rest = test::text(std::string_view("image/png\0\x03\0not a picture", 25));
    apic.insert(apic.end(), rest.begin(), rest.end());
    metadata = extract(mp3(test::id3Tag(3, {{"APIC", apic}})), "mp3");
    EXPECT_EQ(metadata.previews.size(), 1U);
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);
}

TEST(MetadataRobustnessTest, IssuesAreCapped) {
    std::vector<test::Id3Frame> bad;
    for (int i = 0; i < 40; ++i) {
        bad.push_back({"TDRC", test::id3Text(0, test::text("never"))});
    }
    Bytes mp3 = test::id3Tag(4, bad);
    const Bytes frames = ::recovery::test::makeMp3({});
    mp3.insert(mp3.end(), frames.begin(), frames.end());
    const MediaMetadata metadata = extract(mp3, "mp3");
    EXPECT_EQ(metadata.issueCount, 40U);
    EXPECT_EQ(metadata.issues.size(), MediaMetadata::kMaxIssues);
}

TEST(MetadataRobustnessTest, WavWithoutFormatOrData) {
    Bytes wav = test::text("RIFF");
    wav.resize(8);
    const Bytes wave = test::text("WAVE");
    wav.insert(wav.end(), wave.begin(), wave.end());
    const Bytes data = ::recovery::test::riffChunk("data", ::recovery::test::makePattern(100));
    wav.insert(wav.end(), data.begin(), data.end());
    storeLe32(wav, 4, static_cast<std::uint32_t>(wav.size() - 8));
    const MediaMetadata metadata = extract(wav, "wav");
    EXPECT_FALSE(metadata.audio.has_value());
    EXPECT_EQ(metadata.issueCount, 1U) << issuesText(metadata);
    EXPECT_TRUE(metadata.previews.empty());
}

// A reader that fails after a number of reads, as a source can.
class FailingReader final : public carving::IContentReader {
public:
    FailingReader(std::span<const std::byte> bytes, int reads) : inner_(bytes), reads_(reads) {}
    [[nodiscard]] std::uint64_t size() const noexcept override { return inner_.size(); }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override {
        if (reads_-- <= 0) {
            return makeError(ErrorCode::IoError, "the source failed");
        }
        return inner_.read(offset, length);
    }

private:
    carving::MemoryContentReader inner_;
    int reads_;
};

TEST(MetadataRobustnessTest, ReaderErrorsAreErrors) {
    for (const File& file : files()) {
        for (int reads = 0; reads < 6; ++reads) {
            FailingReader reader(file.bytes, reads);
            const Result<MediaMetadata> metadata = extractMetadata(reader, file.format);
            if (!metadata.ok()) {
                EXPECT_EQ(metadata.error().code, ErrorCode::IoError) << file.name;
            }
        }
        FailingReader never(file.bytes, 0);
        RECOVERY_EXPECT_ERROR(extractMetadata(never, file.format), ErrorCode::IoError);
    }
}

}  // namespace
}  // namespace recovery::metadata
