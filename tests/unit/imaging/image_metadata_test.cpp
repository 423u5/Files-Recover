#include "imaging/image_metadata.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::imaging {
namespace {

ImageMetadata sampleMetadata() {
    ImageMetadata m;
    m.engineVersion = "0.1.0";
    m.state = ImageState::Cancelled;
    m.sourceType = storage::SourceType::PhysicalDisk;
    m.sourcePath = "\\\\.\\PhysicalDrive2";
    m.sourceVendor = "Vendor";
    m.sourceProduct = "Flash Disk";
    m.sourceSize = 8 * kMiB;
    m.sectorSize = 512;
    m.blockSize = 1 * kMiB;
    m.bytesCompleted = 4 * kMiB;
    m.startedUtc = "2026-09-19T08:00:00.000Z";
    m.updatedUtc = "2026-09-19T08:05:00.000Z";
    m.badRegions = {{4096, 512, 23}, {1 * kMiB, 1024, 27}};
    return m;
}

std::string serialized(const ImageMetadata& m) {
    Result<std::string> text = serializeImageMetadata(m);
    EXPECT_TRUE(text.ok());
    return text.ok() ? text.value() : std::string{};
}

TEST(ImageMetadataTest, RoundTrips) {
    const ImageMetadata original = sampleMetadata();
    const Result<ImageMetadata> parsed = parseImageMetadata(serialized(original));
    RECOVERY_ASSERT_OK(parsed);
    const ImageMetadata& m = parsed.value();
    EXPECT_EQ(m.engineVersion, original.engineVersion);
    EXPECT_EQ(m.state, original.state);
    EXPECT_EQ(m.sourceType, original.sourceType);
    EXPECT_EQ(m.sourcePath, original.sourcePath);
    EXPECT_EQ(m.sourceVendor, original.sourceVendor);
    EXPECT_EQ(m.sourceProduct, original.sourceProduct);
    EXPECT_EQ(m.sourceSize, original.sourceSize);
    EXPECT_EQ(m.sectorSize, original.sectorSize);
    EXPECT_EQ(m.blockSize, original.blockSize);
    EXPECT_EQ(m.bytesCompleted, original.bytesCompleted);
    EXPECT_EQ(m.startedUtc, original.startedUtc);
    EXPECT_EQ(m.updatedUtc, original.updatedUtc);
    EXPECT_EQ(m.badRegions, original.badRegions);
}

TEST(ImageMetadataTest, AcceptsCrLfAndIgnoresUnknownKeys) {
    std::string text = serialized(sampleMetadata());
    text += "future_field=something\n";
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    RECOVERY_EXPECT_OK(parseImageMetadata(crlf));
}

TEST(ImageMetadataTest, RejectsValuesWithLineBreaks) {
    ImageMetadata m = sampleMetadata();
    m.sourceProduct = "evil\nstate=completed";
    RECOVERY_EXPECT_ERROR(serializeImageMetadata(m), ErrorCode::InvalidInput);
}

struct MalformedCase {
    const char* name;
    std::string from;
    std::string to;
};

class ImageMetadataMalformedTest : public ::testing::TestWithParam<MalformedCase> {};

TEST_P(ImageMetadataMalformedTest, IsRejected) {
    std::string text = serialized(sampleMetadata());
    const MalformedCase& c = GetParam();
    const std::size_t at = text.find(c.from);
    ASSERT_NE(at, std::string::npos) << c.from;
    text.replace(at, c.from.size(), c.to);
    RECOVERY_EXPECT_ERROR(parseImageMetadata(text), ErrorCode::InvalidFormat);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, ImageMetadataMalformedTest,
    ::testing::Values(
        MalformedCase{"UnsupportedVersion", "format_version=1", "format_version=2"},
        MalformedCase{"MissingRequiredKey", "source_size=8388608\n", ""},
        MalformedCase{"DuplicateKey", "sector_size=512", "sector_size=512\nsector_size=4096"},
        MalformedCase{"NegativeNumber", "source_size=8388608", "source_size=-1"},
        MalformedCase{"NumberWithSuffix", "source_size=8388608", "source_size=8388608x"},
        MalformedCase{"NumberOverflow", "source_size=8388608", "source_size=99999999999999999999999"},
        MalformedCase{"HexNumber", "sector_size=512", "sector_size=0x200"},
        MalformedCase{"InvalidSectorSize", "sector_size=512", "sector_size=1000"},
        MalformedCase{"ProgressBeyondSource", "bytes_completed=4194304", "bytes_completed=9999999"},
        MalformedCase{"SizeBeyondAddressable", "source_size=8388608", "source_size=18446744073709551615"},
        MalformedCase{"UnknownState", "state=cancelled", "state=paused"},
        MalformedCase{"UnknownSourceType", "source_type=PhysicalDisk", "source_type=Floppy"},
        MalformedCase{"LineWithoutEquals", "block_size=1048576", "block_size"},
        MalformedCase{"EmptyKey", "block_size=1048576", "=1048576"},
        MalformedCase{"BadRegionMissingField", "bad_region=4096,512,23", "bad_region=4096,512"},
        MalformedCase{"BadRegionZeroLength", "bad_region=4096,512,23", "bad_region=4096,0,23"},
        MalformedCase{"BadRegionOverflow", "bad_region=4096,512,23", "bad_region=18446744073709551615,2,23"},
        MalformedCase{"BadRegionOutsideSource", "bad_region=4096,512,23", "bad_region=8388608,512,23"},
        MalformedCase{"LineTooLong", "engine_version=0.1.0", "engine_version=" + std::string(5000, 'x')}),
    [](const auto& info) { return std::string(info.param.name); });

TEST(ImageMetadataTest, RejectsOversizedInput) {
    const std::string huge(kMaxMetadataBytes + 1, '#');
    RECOVERY_EXPECT_ERROR(parseImageMetadata(huge), ErrorCode::InvalidFormat);
}

TEST(ImageMetadataTest, RejectsEmptyInput) {
    RECOVERY_EXPECT_ERROR(parseImageMetadata(""), ErrorCode::InvalidFormat);
}

TEST(ImageMetadataTest, MetadataPathAppendsExtension) {
    EXPECT_EQ(metadataPathFor(L"D:\\images\\usb.img").native(), L"D:\\images\\usb.img.imgmeta");
}

TEST(ImageMetadataTest, StateNamesRoundTrip) {
    for (const ImageState state :
         {ImageState::InProgress, ImageState::Completed, ImageState::Cancelled, ImageState::Failed}) {
        EXPECT_EQ(parseImageState(toString(state)), state);
    }
    EXPECT_FALSE(parseImageState("COMPLETED").has_value());
}

}  // namespace
}  // namespace recovery::imaging
