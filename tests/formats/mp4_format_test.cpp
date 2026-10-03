// The MP4 carving format (P12): builder files of every layout (moov before,
// between and after the media data, 64-bit sizes, movie fragments, HEVC) and
// the embedded files of FFmpeg, GPAC and Media Foundation are intact;
// prefixes are truncated; the header check; how MP4 and M4A divide the ftyp
// files between them; where the top-level walk ends (the next ftyp, an mdat
// of size 0); corrupt metadata; samples that other data replaced (sample
// framing); fuzzing.

#include "formats/mp4_format.hpp"

#include "format_test_helpers.hpp"
#include "formats/m4a_format.hpp"
#include "mp4_test_helpers.hpp"
#include "support/audio_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::Mp4FragmentBase;
using test::Mp4MoovPlace;
using test::Mp4Options;
using test::Mp4TrackKind;
using test::Mp4TrackOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const Mp4Format& format() {
    static const Mp4Format instance;
    return instance;
}

const M4aFormat& audioFormat() {
    static const M4aFormat instance;
    return instance;
}

Bytes be32(std::uint64_t value) {
    return Bytes{static_cast<std::byte>((value >> 24) & 0xFF), static_cast<std::byte>((value >> 16) & 0xFF),
                 static_cast<std::byte>((value >> 8) & 0xFF), static_cast<std::byte>(value & 0xFF)};
}

Bytes text(std::string_view value) {
    Bytes out;
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

Mp4TrackOptions track(Mp4TrackKind kind, std::size_t samples, std::size_t perChunk = 4) {
    Mp4TrackOptions options;
    options.kind = kind;
    options.samples = samples;
    options.samplesPerChunk = perChunk;
    return options;
}

test::BoxPosition box(const Bytes& file, std::string_view path, std::size_t index = 0) {
    return test::findBox(test::mp4Boxes(file), path, index);
}

// Every builder layout the carver must take whole.
std::vector<std::pair<std::string, Mp4Options>> layouts() {
    std::vector<std::pair<std::string, Mp4Options>> all;
    all.emplace_back("moov last", Mp4Options{});
    Mp4Options options;
    options.moov = Mp4MoovPlace::First;
    all.emplace_back("moov first", options);
    options = {};
    options.moov = Mp4MoovPlace::Between;
    options.mediaDataBoxes = 3;
    options.wideBox = true;
    options.tracks = {track(Mp4TrackKind::Video, 20, 3), track(Mp4TrackKind::Audio, 30),
                      track(Mp4TrackKind::Text, 4, 1)};
    all.emplace_back("moov between three mdat boxes", options);
    options = {};
    options.largeMediaData = true;
    options.largeMoov = true;
    options.largeNested = true;
    options.co64 = true;
    all.emplace_back("64-bit sizes and offsets", options);
    options = {};
    options.reverseChildren = true;
    options.extraBoxes = true;
    options.interleave = false;
    options.uuidBox = true;
    options.freeBox = true;
    all.emplace_back("reversed children, uuid and free", options);
    options = {};
    options.majorBrand = "qt  ";
    options.compatibleBrands = {"qt  "};
    options.wideBox = true;
    all.emplace_back("QuickTime", options);
    options = {};
    options.majorBrand = "3gp6";
    options.compatibleBrands = {"3gp6", "isom"};
    all.emplace_back("3GP", options);
    options = {};
    options.majorBrand = "M4V ";
    options.tracks = {track(Mp4TrackKind::Audio, 10)};
    all.emplace_back("M4V brand, sound only", options);
    options = {};
    Mp4TrackOptions hevc = track(Mp4TrackKind::Video, 15);
    hevc.hevc = true;
    hevc.nalLengthSize = 2;
    options.tracks = {hevc};
    all.emplace_back("HEVC video only", options);
    options = {};
    options.moov = Mp4MoovPlace::First;
    options.mediaDataToEnd = true;
    all.emplace_back("an mdat of size 0 after moov", options);
    for (const Mp4FragmentBase base : {Mp4FragmentBase::Moof, Mp4FragmentBase::Explicit, Mp4FragmentBase::Implicit}) {
        options = {};
        options.moov = Mp4MoovPlace::First;
        options.fragmentSamples = 4;
        options.samplesInMoov = base == Mp4FragmentBase::Explicit ? 3 : 0;
        options.fragmentBase = base;
        all.emplace_back("movie fragments " + std::to_string(static_cast<int>(base)), options);
    }
    return all;
}

// The prefix lengths that are complete files: top-level box boundaries once moov and mdat were seen and
// the media data of moov's sample tables is there, except right after a moof (its mdat is missing).
std::vector<std::size_t> completeLengths(const Bytes& file) {
    const mp4::Mp4File parsed = testing::parseBytes(file);
    std::uint64_t needed = 0;
    for (const mp4::Track& t : parsed.movie->tracks) {
        for (const mp4::Chunk& chunk : t.chunks) {
            needed = std::max(needed, chunk.end());
        }
    }
    std::vector<std::size_t> lengths;
    bool moov = false;
    bool mdat = false;
    std::string previous;
    for (const test::BoxPosition& top : test::mp4Boxes(file)) {
        if (top.path.find('/') != std::string::npos) {
            continue;
        }
        if (moov && mdat && previous != "moof" && top.offset >= needed) {
            lengths.push_back(top.offset);
        }
        moov = moov || top.path == "moov";
        mdat = mdat || top.path == "mdat";
        previous = top.path;
    }
    return lengths;
}

bool rejects(const carving::IFileFormat& candidate, const Bytes& file) {
    if (!testing::headerOf(candidate, file).plausible) {
        return true;
    }
    const carving::EndDetection end = endOf(candidate, file);
    return end.status == EndStatus::Broken && end.length == 0;
}

TEST(Mp4FormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "mp4");
    EXPECT_EQ(format().descriptor().extension, "mp4");
    ASSERT_EQ(format().descriptor().signatures.size(), 1u);
    EXPECT_EQ(format().descriptor().signatures[0].offset, 4u);
    EXPECT_EQ(format().descriptor().maximumSize, 256 * kGiB);
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
    EXPECT_TRUE(format().options().checkSampleFraming);
}

TEST(Mp4FormatTest, BuilderFilesOfEveryLayoutAreIntact) {
    for (const auto& [what, options] : layouts()) {
        SCOPED_TRACE(what);
        const Bytes file = test::makeMp4(options).bytes;
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(rejects(audioFormat(), file));
    }
}

TEST(Mp4FormatTest, FilesOfOtherWritersAreIntact) {
    std::size_t videos = 0;
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        bool video = false;
        for (const test::mp4_samples::Track& t : sample.tracks) {
            video = video || t.kind == "video";
        }
        // Each file is exactly one format's: video is MP4, sound only (with a generic brand) is M4A.
        const carving::IFileFormat& mine = video ? static_cast<const carving::IFileFormat&>(format()) : audioFormat();
        const carving::IFileFormat& other = video ? static_cast<const carving::IFileFormat&>(audioFormat()) : format();
        EXPECT_TRUE(isIntact(mine, file));
        EXPECT_TRUE(rejects(other, file));
        bool framed = false;
        for (const test::mp4_samples::Track& t : sample.tracks) {
            framed = framed || t.codec == "avc1" || t.codec == "hvc1" || t.codec == "hev1";
        }
        if (framed) {
            // Validation walked the NAL units of the AVC and HEVC samples (not MPEG-4 Part 2's).
            EXPECT_NE(verdictOf(format(), file).detail.find("fill them"), std::string::npos)
                << verdictOf(format(), file).detail;
        }
        videos += video ? 1 : 0;
    }
    EXPECT_GE(videos, 20u);
}

TEST(Mp4FormatTest, PrefixesAreTruncated) {
    for (const auto& [what, options] : layouts()) {
        if (options.mediaDataToEnd) {
            continue;  // every prefix is an mdat to the end of the data (checked below)
        }
        SCOPED_TRACE(what);
        const Bytes file = test::makeMp4(options).bytes;
        const std::vector<std::size_t> complete = completeLengths(file);
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file, complete));
    }
    // An mdat to the end of the data: its samples say where it ends.
    Mp4Options toEnd;
    toEnd.moov = Mp4MoovPlace::First;
    toEnd.mediaDataToEnd = true;
    const Bytes file = test::makeMp4(toEnd).bytes;
    const std::size_t mdat = box(file, "mdat").offset;
    for (std::size_t length = mdat + 8; length < file.size(); length += 97) {
        const carving::EndDetection end = endOf(format(), testing::prefix(file, length));
        EXPECT_EQ(end.status, EndStatus::Truncated) << testing::describe(end);
        EXPECT_EQ(end.length, length);
    }
}

TEST(Mp4FormatTest, HeaderCheckRejectsFalseSignaturesAudioAndImages) {
    const Bytes file = test::makeMp4().bytes;
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    for (const std::uint32_t size : {15U, 26U, 2048U, 0xFFFFFFFFU}) {
        EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 0, be32(size))).plausible) << size;
    }
    for (const std::string_view brand : {"M4A ", "M4B ", "F4A ", "heic", "avif", "mif1", "crx "}) {
        EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 8, text(brand))).plausible) << brand;
    }
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 8, {0x00, 0x01, 0x02, 0x03})).plausible);
    for (const std::string_view brand : {"isom", "mp42", "qt  ", "M4V ", "3gp4", "dash", "XAVC"}) {
        EXPECT_TRUE(testing::headerOf(format(), overwritten(file, 8, text(brand))).plausible) << brand;
    }
    const std::size_t next = box(file, "ftyp").size;
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, next + 4, {0x00})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, next, be32(3))).plausible);
}

TEST(Mp4FormatTest, Mp4AndM4aDivideTheFtypFiles) {
    struct Case {
        std::string what;
        Bytes file;
        bool video;
    };
    std::vector<Case> cases;
    test::M4aOptions m4a;
    cases.push_back({"an M4A", test::makeM4a(m4a), false});
    m4a.majorBrand = "mp42";
    cases.push_back({"a generic brand with only sound", test::makeM4a(m4a), false});
    m4a.videoTrack = true;
    cases.push_back({"a generic brand with sound and video", test::makeM4a(m4a), true});
    m4a.majorBrand = "M4B ";
    cases.push_back({"an audio brand with video (chapter pictures)", test::makeM4a(m4a), false});
    Mp4Options mp4;
    cases.push_back({"an MP4", test::makeMp4(mp4).bytes, true});
    mp4.tracks = {track(Mp4TrackKind::Audio, 12)};
    cases.push_back({"an MP4 with only sound", test::makeMp4(mp4).bytes, false});
    mp4.tracks = {track(Mp4TrackKind::Text, 4, 1)};
    cases.push_back({"an MP4 with only text", test::makeMp4(mp4).bytes, true});
    mp4.majorBrand = "qt  ";
    mp4.tracks = {track(Mp4TrackKind::Audio, 12)};
    cases.push_back({"a QuickTime file with only sound", test::makeMp4(mp4).bytes, true});
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        const carving::IFileFormat& video = format();
        const carving::IFileFormat& audio = audioFormat();
        const carving::IFileFormat& mine = c.video ? video : audio;
        const carving::IFileFormat& other = c.video ? audio : video;
        EXPECT_TRUE(isIntact(mine, c.file));
        EXPECT_TRUE(rejects(other, c.file));
        EXPECT_TRUE(isInvalid(other, c.file));
    }
    // A generic brand whose moov is lost: MP4 takes it (L76), M4A does not.
    const Bytes last = test::makeMp4().bytes;
    const Bytes noMoov = testing::prefix(last, box(last, "moov").offset);
    EXPECT_EQ(endOf(format(), noMoov).status, EndStatus::Truncated);
    EXPECT_TRUE(rejects(audioFormat(), noMoov));
    // An image brand: neither.
    const Bytes image = overwritten(last, 8, text("heic"));
    EXPECT_TRUE(rejects(format(), image));
    EXPECT_TRUE(rejects(audioFormat(), image));
}

TEST(Mp4FormatTest, TheTopLevelWalkEndsWhereTheFileDoes) {
    const Bytes file = test::makeMp4().bytes;
    // The next file's ftyp ends this one.
    EXPECT_EQ(endOf(format(), concat({file, test::makeMp4().bytes})).length, file.size());
    // Once moov and mdat are seen, only top-level box types continue the file.
    EXPECT_EQ(endOf(format(), concat({file, be32(16), text("abcd"), Bytes(8)})).length, file.size());
    EXPECT_TRUE(isIntact(format(), concat({file, be32(16), text("free"), Bytes(8)})));
    // A box running beyond the data.
    const std::size_t mdat = box(file, "mdat").offset;
    const carving::EndDetection beyond = endOf(format(), overwritten(file, mdat, be32(0x7FFFFFF0)));
    EXPECT_EQ(beyond.status, EndStatus::Truncated) << testing::describe(beyond);
    // An mdat of size 0 before moov: nothing says where it ends.
    const carving::EndDetection zero = endOf(format(), overwritten(file, mdat, be32(0)));
    EXPECT_EQ(zero.status, EndStatus::Broken) << testing::describe(zero);
    EXPECT_EQ(zero.length, mdat);
    // After moov, the sample tables do, whatever follows.
    Mp4Options first;
    first.moov = Mp4MoovPlace::First;
    const Bytes fast = test::makeMp4(first).bytes;
    const std::size_t media = box(fast, "mdat").offset;
    const Bytes open = overwritten(fast, media, be32(0));
    const carving::EndDetection tables = endOf(format(), concat({open, testing::noise(5000, 3)}));
    EXPECT_EQ(tables.status, EndStatus::Found) << testing::describe(tables);
    EXPECT_EQ(tables.length, fast.size());
    EXPECT_NE(tables.detail.find("size 0"), std::string::npos) << tables.detail;
    EXPECT_EQ(verdictOf(format(), open).status, ValidationStatus::Valid);
}

TEST(Mp4FormatTest, CorruptMetadataIsCaught) {
    const Bytes file = test::makeMp4().bytes;
    const std::vector<test::BoxPosition> boxes = test::mp4Boxes(file);
    const std::string stbl = "moov/trak/mdia/minf/stbl/";
    const auto expectInvalid = [&](const Bytes& damaged, const std::string& what) {
        SCOPED_TRACE(what);
        EXPECT_EQ(endOf(format(), damaged).length, file.size());
        EXPECT_TRUE(isInvalid(format(), damaged));
    };
    const test::BoxPosition stco = test::findBox(boxes, stbl + "stco");
    expectInvalid(overwritten(file, stco.offset + 16, be32(0)), "a chunk offset before mdat");
    expectInvalid(overwritten(file, stco.offset + 12, be32(1000)), "more chunk offsets than fit");
    // A chunk beyond the file: media data that is not there (it may be elsewhere on the disk).
    const Bytes beyond = overwritten(file, stco.offset + 16, be32(file.size()));
    const carving::EndDetection cut = endOf(format(), concat({beyond, testing::noise(3000, 4)}));
    EXPECT_EQ(cut.status, EndStatus::Broken) << testing::describe(cut);
    EXPECT_EQ(cut.length, file.size());
    EXPECT_EQ(endOf(format(), beyond).status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), beyond).status, ValidationStatus::Truncated);
    const test::BoxPosition stsz = test::findBox(boxes, stbl + "stsz");
    expectInvalid(overwritten(file, stsz.offset + 20, be32(60000)), "a sample size that overruns its chunk");
    for (const std::string_view name : {"stsd", "stts", "stsc", "stsz", "stco"}) {
        expectInvalid(overwritten(file, test::findBox(boxes, stbl + std::string(name)).offset + 4, text("xxxx")),
                      "no " + std::string(name));
    }
    expectInvalid(overwritten(file, test::findBox(boxes, "moov/mvhd").offset + 8, {0x02}), "mvhd version 2");
    // The video track's boxes broken before its handler: the file looks like sound only, and M4A takes it.
    const Bytes lost = overwritten(file, test::findBox(boxes, "moov/trak/tkhd").offset, be32(20));
    EXPECT_EQ(endOf(format(), lost).length, 0u);
    EXPECT_EQ(endOf(audioFormat(), lost).length, file.size());
    EXPECT_TRUE(isInvalid(audioFormat(), lost));
    // A generic brand whose tracks were damaged into sound only: not a video file.
    const test::BoxPosition hdlr = test::findBox(boxes, "moov/trak/mdia/hdlr");
    const Bytes deaf = overwritten(file, hdlr.offset + 16, text("soun"));
    EXPECT_EQ(endOf(format(), deaf).length, 0u);
    // mdat's size shrunk: the walk stops inside the media data.
    const test::BoxPosition mdat = test::findBox(boxes, "mdat");
    const Bytes shrunk = overwritten(file, mdat.offset, be32(mdat.size - 1000));
    const carving::EndDetection end = endOf(format(), shrunk);
    EXPECT_NE(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_FALSE(isIntact(format(), shrunk));
}

TEST(Mp4FormatTest, SamplesThatAreNotTheirOwnAreCaught) {
    const test::Mp4Built built = test::makeMp4();
    const Bytes& file = built.bytes;
    const test::Mp4Sample& sample = built.samples[0][6];
    // Another file's data over part of the media data: the boxes are intact, the samples are not.
    const Bytes overwrittenData = overwritten(file, sample.offset, testing::noise(3000, 17));
    EXPECT_EQ(endOf(format(), overwrittenData).status, EndStatus::Found);
    const carving::ValidationResult verdict = verdictOf(format(), overwrittenData);
    EXPECT_EQ(verdict.status, ValidationStatus::Invalid) << testing::describe(verdict);
    EXPECT_NE(verdict.detail.find("do not fill them"), std::string::npos) << verdict.detail;
    EXPECT_EQ(verdict.validBytes, sample.offset);
    // Without the framing check only the structure is validated.
    Mp4FormatOptions options;
    options.checkSampleFraming = false;
    EXPECT_EQ(verdictOf(Mp4Format(options), overwrittenData).status, ValidationStatus::Valid);
    // A gap inserted into the media data of a file with moov first (fragmented on disk): the chunks no
    // longer find their samples.
    Mp4Options first;
    first.moov = Mp4MoovPlace::First;
    const test::Mp4Built fast = test::makeMp4(first);
    const Bytes gap = testing::inserted(fast.bytes, fast.samples[0][2].offset, testing::noise(4096, 9));
    const carving::EndDetection end = endOf(format(), gap);
    EXPECT_NE(verdictOf(format(), testing::prefix(gap, end.length)).status, ValidationStatus::Valid);
}

TEST(Mp4FormatTest, FuzzedFilesStayWithinTheirData) {
    std::uint64_t seed = 210;
    for (const auto& [what, options] : layouts()) {
        SCOPED_TRACE(what);
        testing::fuzz(format(), test::makeMp4(options).bytes, 400, seed++);
    }
    for (const test::mp4_samples::Sample& sample : test::mp4_samples::all()) {
        testing::fuzz(format(), sample.data(), 200, seed++);
    }
}

}  // namespace
}  // namespace recovery::formats
