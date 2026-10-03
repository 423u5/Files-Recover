// M4A: intact files from the builder (moov before and after mdat, 64-bit
// sizes, co64, metadata, audio and generic brands, several chunks) and from
// FFmpeg, faac (mp4v2), fdkaac and Media Foundation; truncation; header
// rejection; telling audio from video; the top-level walk; damaged boxes and
// sample tables; fragments; fuzzing.

#include "formats/m4a_format.hpp"

#include "format_test_helpers.hpp"
#include "support/audio_builders.hpp"
#include "support/audio_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::M4aOptions;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const M4aFormat& format() {
    static const M4aFormat instance;
    return instance;
}

Bytes be32(std::uint32_t value) {
    return Bytes{static_cast<std::byte>(value >> 24), static_cast<std::byte>((value >> 16) & 0xFF),
                 static_cast<std::byte>((value >> 8) & 0xFF), static_cast<std::byte>(value & 0xFF)};
}

Bytes text(std::string_view value) {
    Bytes out;
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

const test::BoxPosition& box(const std::vector<test::BoxPosition>& boxes, std::string_view path) {
    for (const test::BoxPosition& candidate : boxes) {
        if (candidate.path == path) {
            return candidate;
        }
    }
    throw std::invalid_argument("no box " + std::string(path));
}

bool isAudioBrand(const Bytes& file) {
    const std::string brand(reinterpret_cast<const char*>(file.data()) + 8, 4);
    return brand == "M4A " || brand == "M4B " || brand == "M4P " || brand == "F4A " || brand == "F4B ";
}

// The prefix lengths that are complete files: top-level box boundaries once
// both moov and mdat have been seen.
std::vector<std::size_t> completeLengths(const Bytes& file) {
    std::vector<std::size_t> lengths;
    bool moov = false;
    bool mdat = false;
    for (const test::BoxPosition& top : test::m4aBoxes(file)) {
        if (top.path.find('/') != std::string::npos) {
            continue;
        }
        if (moov && mdat) {
            lengths.push_back(top.offset);
        }
        moov = moov || top.path == "moov";
        mdat = mdat || top.path == "mdat";
    }
    return lengths;
}

// Every proper prefix is Truncated or, for a generic brand before its moov,
// rejected (Broken at 0); never Valid.
::testing::AssertionResult prefixesAreNeverValid(const Bytes& file) {
    const std::vector<std::size_t> complete = completeLengths(file);
    for (std::size_t length = 0; length < file.size(); length += length < 4096 ? 1 : 61) {
        if (std::find(complete.begin(), complete.end(), length) != complete.end()) {
            continue;
        }
        const Bytes part = testing::prefix(file, length);
        const carving::EndDetection end = endOf(format(), part);
        const bool rejected = end.status == EndStatus::Broken && end.length == 0;
        if (!(end.status == EndStatus::Truncated || (rejected && !isAudioBrand(file)))) {
            return ::testing::AssertionFailure() << "end of the first " << length << " bytes: " << testing::describe(end);
        }
        if (verdictOf(format(), part).status == ValidationStatus::Valid) {
            return ::testing::AssertionFailure() << "the first " << length << " bytes validate";
        }
    }
    return ::testing::AssertionSuccess();
}

TEST(M4aFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "m4a");
    ASSERT_EQ(format().descriptor().signatures.size(), 1u);
    EXPECT_EQ(format().descriptor().signatures[0].offset, 4u);
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
}

TEST(M4aFormatTest, BuilderFilesOfEveryShapeAreIntact) {
    for (const bool moovFirst : {false, true}) {
        for (const bool co64 : {false, true}) {
            for (const bool largeMdat : {false, true}) {
                for (const bool metadata : {false, true}) {
                    M4aOptions options;
                    options.moovFirst = moovFirst;
                    options.co64 = co64;
                    options.largeMdat = largeMdat;
                    options.metadata = metadata;
                    options.freeBox = !metadata;
                    SCOPED_TRACE(std::string(moovFirst ? "moov first" : "moov last") + (co64 ? " co64" : "") +
                                 (largeMdat ? " large mdat" : "") + (metadata ? " metadata" : ""));
                    EXPECT_TRUE(isIntact(format(), test::makeM4a(options)));
                }
            }
        }
    }
    // Brands: audio brands, and generic ones whose tracks are all sound.
    for (const std::string brand : {"M4A ", "M4B ", "F4A ", "mp42", "isom", "3gp4", "iso6"}) {
        M4aOptions options;
        options.majorBrand = brand;
        options.compatibleBrands = {brand, "isom"};
        SCOPED_TRACE(brand);
        EXPECT_TRUE(isIntact(format(), test::makeM4a(options)));
    }
    // Mono, other rates, one chunk per sample, one chunk for everything, no compatible brands.
    M4aOptions varied;
    varied.audio.channels = 1;
    varied.audio.sampleRate = 22050;
    varied.samplesPerChunk = 1;
    varied.compatibleBrands = {};
    EXPECT_TRUE(isIntact(format(), test::makeM4a(varied)));
    varied.samplesPerChunk = 100;
    EXPECT_TRUE(isIntact(format(), test::makeM4a(varied)));
    // An audio brand with a video track (as audiobooks carry chapter pictures).
    M4aOptions chapters;
    chapters.majorBrand = "M4B ";
    chapters.videoTrack = true;
    EXPECT_TRUE(isIntact(format(), test::makeM4a(chapters)));
}

TEST(M4aFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format != "m4a") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(prefixesAreNeverValid(file));
        ++checked;
    }
    EXPECT_GE(checked, 8u);
}

TEST(M4aFormatTest, PrefixesAreTruncatedOrRejected) {
    M4aOptions options;
    options.metadata = true;
    options.largeMdat = true;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeM4a(options)));
    options.moovFirst = true;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeM4a(options)));
    // With a generic brand, a prefix without moov cannot be told from a video.
    options.moovFirst = false;
    options.majorBrand = "mp42";
    EXPECT_TRUE(prefixesAreNeverValid(test::makeM4a(options)));
}

TEST(M4aFormatTest, HeaderCheckRejectsFalseSignaturesAndVideos) {
    const Bytes file = test::makeM4a();
    EXPECT_TRUE(testing::headerOf(format(), file).plausible);
    // ftyp sizes that are not a whole number of brands, or too large.
    for (const std::uint32_t size : {15U, 26U, 2048U, 0xFFFFFFFFU}) {
        EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 0, be32(size))).plausible) << size;
    }
    // Video and image brands, and brands that are not text.
    for (const std::string_view brand : {"M4V ", "qt  ", "heic", "avif"}) {
        EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 8, text(brand))).plausible) << brand;
    }
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, 8, {0x00, 0x01, 0x02, 0x03})).plausible);
    // No box header after ftyp.
    const std::size_t next = box(test::m4aBoxes(file), "ftyp").size;
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, next + 4, {0x00})).plausible);
    EXPECT_FALSE(testing::headerOf(format(), overwritten(file, next, be32(3))).plausible);
}

TEST(M4aFormatTest, OnlyAudioFilesAreM4a) {
    // A generic brand with a video track: an MP4 video, not audio (P12's).
    M4aOptions video;
    video.majorBrand = "isom";
    video.videoTrack = true;
    const carving::EndDetection end = endOf(format(), test::makeM4a(video));
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_EQ(end.length, 0u);
    EXPECT_NE(end.detail.find("video"), std::string::npos);
    // Video only.
    video.soundTrack = false;
    EXPECT_EQ(endOf(format(), test::makeM4a(video)).length, 0u);
    // Video only with an audio brand: the brand decides, but a sound track is missing.
    video.majorBrand = "M4A ";
    const Bytes silent = test::makeM4a(video);
    EXPECT_EQ(endOf(format(), silent).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), silent));
    // A generic brand whose moov lies beyond the data: nothing to classify by.
    M4aOptions generic;
    generic.majorBrand = "mp42";
    const Bytes last = test::makeM4a(generic);
    EXPECT_EQ(endOf(format(), testing::prefix(last, box(test::m4aBoxes(last), "moov").offset)).length, 0u);
}

TEST(M4aFormatTest, TheTopLevelWalkEndsWhereTheFileDoes) {
    M4aOptions options;
    const Bytes file = test::makeM4a(options);
    // The next file's ftyp ends this one, whatever brand it has.
    const Bytes both = concat({file, test::makeM4a(options)});
    EXPECT_EQ(endOf(format(), both).length, file.size());
    // Once moov and mdat are seen, only top-level box types continue the file.
    const Bytes unknown = concat({file, be32(16), text("abcd"), Bytes(8)});
    EXPECT_EQ(endOf(format(), unknown).length, file.size());
    const Bytes known = concat({file, be32(16), text("free"), Bytes(8)});
    EXPECT_TRUE(isIntact(format(), known));
    // Before that, other boxes are part of the file (Media Foundation writes a uuid box).
    const Bytes extra = testing::inserted(file, box(test::m4aBoxes(file), "ftyp").size,
                                          concat({be32(12), text("abcd"), Bytes(4)}));
    EXPECT_FALSE(isIntact(format(), extra));  // the chunk offsets moved
    EXPECT_EQ(endOf(format(), extra).length, extra.size());
    // A box running beyond the data, and a box of size 0.
    const std::size_t mdat = box(test::m4aBoxes(file), "mdat").offset;
    const Bytes beyond = overwritten(file, mdat, be32(0x7FFFFFF0));
    EXPECT_EQ(endOf(format(), beyond).status, EndStatus::Truncated);
    const Bytes unsized = overwritten(file, mdat, be32(0));
    const carving::EndDetection zero = endOf(format(), unsized);
    EXPECT_EQ(zero.status, EndStatus::Broken) << testing::describe(zero);
    EXPECT_EQ(zero.length, mdat);
}

TEST(M4aFormatTest, MissingAndRepeatedBoxesAreCaught) {
    M4aOptions options;
    const Bytes file = test::makeM4a(options);
    const std::vector<test::BoxPosition> boxes = test::m4aBoxes(file);
    // No moov: the boxes stop before it (an audio brand keeps what it has).
    const std::size_t moov = box(boxes, "moov").offset;
    const Bytes noMoov = concat({testing::prefix(file, moov), Bytes(4096)});
    const carving::EndDetection end = endOf(format(), noMoov);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_EQ(end.length, moov);
    // No mdat: it is renamed to a free box.
    const Bytes noMdat = overwritten(file, box(boxes, "mdat").offset + 4, text("free"));
    EXPECT_NE(endOf(format(), noMdat).status, EndStatus::Found);
    // A second moov.
    const Bytes twice = concat({file, Bytes(file.begin() + static_cast<std::ptrdiff_t>(moov), file.end())});
    EXPECT_EQ(endOf(format(), twice).length, twice.size());
    EXPECT_TRUE(isInvalid(format(), twice));
}

TEST(M4aFormatTest, DamagedSampleTablesAreCaught) {
    M4aOptions options;
    const Bytes file = test::makeM4a(options);
    const std::vector<test::BoxPosition> boxes = test::m4aBoxes(file);
    const std::string stbl = "moov/trak/mdia/minf/stbl/";
    const auto expectInvalid = [&](const Bytes& damaged, const std::string& what) {
        SCOPED_TRACE(what);
        EXPECT_EQ(endOf(format(), damaged).length, file.size());
        EXPECT_TRUE(isInvalid(format(), damaged));
    };
    const test::BoxPosition& stco = box(boxes, stbl + "stco");
    expectInvalid(overwritten(file, stco.offset + 16, be32(0)), "a chunk offset before mdat");
    expectInvalid(overwritten(file, stco.offset + 16, be32(static_cast<std::uint32_t>(file.size()))),
                  "a chunk offset beyond mdat");
    expectInvalid(overwritten(file, stco.offset + 12, be32(1000)), "more chunk offsets than fit");
    for (const std::string_view name : {"stsd", "stts", "stsc", "stsz", "stco"}) {
        expectInvalid(overwritten(file, box(boxes, stbl + std::string(name)).offset + 4, text("xxxx")),
                      "no " + std::string(name));
    }
    expectInvalid(overwritten(file, box(boxes, stbl + "stsd").offset + 12, be32(0)), "no sample description");
    const test::BoxPosition& mdhd = box(boxes, "moov/trak/mdia/mdhd");
    expectInvalid(overwritten(file, mdhd.offset + 20, be32(0)), "time scale 0");
    expectInvalid(overwritten(file, box(boxes, "moov/mvhd").offset + 8, {0x02}), "mvhd version 2");
    expectInvalid(overwritten(file, box(boxes, "moov/trak/tkhd").offset, be32(20)), "boxes inside trak do not fit");
    expectInvalid(overwritten(file, box(boxes, "moov/trak/mdia/hdlr").offset + 4, text("xxxx")), "no handler");
}

TEST(M4aFormatTest, FragmentsAreNoticedThroughTheBoxes) {
    M4aOptions options;
    options.audio.frames = 60;
    const Bytes file = test::makeM4a(options);  // moov after mdat
    const std::size_t mdat = box(test::m4aBoxes(file), "mdat").offset;
    // Foreign data between fragments of mdat: moov is not where mdat's size says.
    const Bytes inserted = testing::inserted(file, mdat + 2000, testing::quietAudioNoise(4096, 7));
    const carving::EndDetection end = endOf(format(), inserted);
    EXPECT_NE(end.status, EndStatus::Found) << testing::describe(end);
    EXPECT_NE(verdictOf(format(), testing::prefix(inserted, end.length)).status, ValidationStatus::Valid);
    // Samples overwritten in place are not noticed: they are not decoded (L59).
    EXPECT_TRUE(isIntact(format(), overwritten(file, mdat + 2000, testing::noise(1000, 8))));
    // With moov first, an inserted gap moves the chunks away from their offsets.
    options.moovFirst = true;
    const Bytes first = test::makeM4a(options);
    const std::size_t media = box(test::m4aBoxes(first), "mdat").offset;
    const Bytes gap = testing::inserted(first, media + 100, testing::quietAudioNoise(4096, 9));
    EXPECT_EQ(endOf(format(), gap).status, EndStatus::Found);  // mdat's size still holds
}

TEST(M4aFormatTest, FuzzedFilesStayWithinTheirData) {
    M4aOptions options;
    options.metadata = true;
    options.co64 = true;
    options.audio.frames = 8;
    testing::fuzz(format(), test::makeM4a(options), 3000, 81);
    options.moovFirst = true;
    options.majorBrand = "mp42";
    options.videoTrack = true;
    testing::fuzz(format(), test::makeM4a(options), 1000, 82);
    for (const test::audio_samples::Sample& sample : test::audio_samples::all()) {
        if (sample.format == "m4a") {
            testing::fuzz(format(), sample.data(), 300, 83);
        }
    }
}

}  // namespace
}  // namespace recovery::formats
