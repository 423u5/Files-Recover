// JPEG: intact files from the builder (baseline and progressive, every
// sampling, restart intervals, Exif thumbnails, comments, fill bytes, odd
// sizes) and from real encoders; truncation at every position; header
// rejection; damaged segments, tables, frames, scans and restart markers;
// overwritten and fragmented entropy-coded data; erased media; fuzzing.

#include "formats/jpeg_format.hpp"

#include "format_test_helpers.hpp"
#include "recovery/byte_order.hpp"
#include "support/image_builders.hpp"
#include "support/image_samples.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <utility>

namespace recovery::formats {
namespace {

using carving::EndStatus;
using carving::ValidationStatus;
using test::JpegOptions;
using test::JpegSampling;
using testing::Bytes;
using testing::concat;
using testing::endOf;
using testing::inserted;
using testing::isIntact;
using testing::isInvalid;
using testing::overwritten;
using testing::verdictOf;

const JpegFormat& format() {
    static const JpegFormat instance;
    return instance;
}

JpegOptions jpegOptions(int width, int height, JpegSampling sampling) {
    JpegOptions result;
    result.width = static_cast<std::uint16_t>(width);
    result.height = static_cast<std::uint16_t>(height);
    result.sampling = sampling;
    return result;
}

// The `skip`-th marker `code` of a builder file.
test::JpegMarker markerOf(const Bytes& file, std::uint8_t code, std::size_t skip = 0) {
    for (const test::JpegMarker& marker : test::jpegMarkers(file)) {
        if (marker.code == code && skip-- == 0) {
            return marker;
        }
    }
    ADD_FAILURE() << "no marker " << static_cast<int>(code);
    return {};
}

// Where the first scan's entropy-coded data starts, and where the marker that ends it is.
std::pair<std::size_t, std::size_t> firstScanData(const Bytes& file) {
    const std::vector<test::JpegMarker> markers = test::jpegMarkers(file);
    for (std::size_t i = 0; i + 1 < markers.size(); ++i) {
        if (markers[i].code == 0xDA) {
            std::size_t next = i + 1;
            while (next < markers.size() && markers[next].code >= 0xD0 && markers[next].code <= 0xD7) {
                ++next;
            }
            return {markers[i].offset + 2 + markers[i].length, markers[next].offset};
        }
    }
    ADD_FAILURE() << "no scan";
    return {0, 0};
}

// A file without the segment of the `skip`-th marker `code`.
Bytes withoutSegment(const Bytes& file, std::uint8_t code, std::size_t skip = 0) {
    const test::JpegMarker marker = markerOf(file, code, skip);
    return testing::erased(file, marker.offset, 2 + marker.length);
}

TEST(JpegFormatTest, DescriptorIsValid) {
    RECOVERY_EXPECT_OK(carving::validateDescriptor(format().descriptor()));
    EXPECT_EQ(format().descriptor().id, "jpeg");
    EXPECT_EQ(format().descriptor().extension, "jpg");
    EXPECT_EQ(format().descriptor().endDetection, carving::EndDetectionMethod::StructureWalk);
    EXPECT_EQ(&format().validator(), static_cast<const carving::FormatValidator*>(&format()));
}

TEST(JpegFormatTest, BuilderFilesOfEverySamplingAndSizeAreIntact) {
    const std::vector<std::pair<int, int>> sizes = {{64, 48}, {1, 1}, {17, 9}, {250, 3}, {8, 72}};
    for (const JpegSampling sampling :
         {JpegSampling::Gray, JpegSampling::Yuv444, JpegSampling::Yuv422, JpegSampling::Yuv420}) {
        for (const bool progressive : {false, true}) {
            for (const auto& [width, height] : sizes) {
                JpegOptions options = jpegOptions(width, height, sampling);
                options.progressive = progressive;
                SCOPED_TRACE(std::to_string(static_cast<int>(sampling)) + (progressive ? " progressive " : " ") +
                             std::to_string(width) + "x" + std::to_string(height));
                EXPECT_TRUE(isIntact(format(), test::makeJpeg(options)));
            }
        }
    }
}

TEST(JpegFormatTest, RestartIntervalsOfEveryLengthAreIntact) {
    // Intervals shorter and longer than the scan, and more than 8 intervals (RST7 wraps to RST0).
    for (const std::uint16_t interval : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{3}, std::uint16_t{7}, std::uint16_t{100}}) {
        for (const bool progressive : {false, true}) {
            for (const JpegSampling sampling : {JpegSampling::Gray, JpegSampling::Yuv420}) {
                JpegOptions options = jpegOptions(90, 70, sampling);
                options.restartInterval = interval;
                options.progressive = progressive;
                SCOPED_TRACE(std::to_string(interval) + (progressive ? " progressive" : "") +
                             (sampling == JpegSampling::Gray ? " gray" : " 4:2:0"));
                const Bytes file = test::makeJpeg(options);
                EXPECT_TRUE(isIntact(format(), file));
                const auto markers = test::jpegMarkers(file);
                const auto restarts = std::count_if(markers.begin(), markers.end(), [](const test::JpegMarker& m) {
                    return m.code >= 0xD0 && m.code <= 0xD7;
                });
                if (interval == 1) {
                    EXPECT_GT(restarts, 8);
                }
            }
        }
    }
}

TEST(JpegFormatTest, ExifThumbnailsCommentsAndFillBytesAreIntact) {
    JpegOptions options;
    options.exifThumbnail = true;
    options.comment = "a comment";
    const Bytes withThumbnail = test::makeJpeg(options);
    EXPECT_TRUE(isIntact(format(), withThumbnail));
    // The thumbnail is a complete JPEG inside APP1, and valid on its own. Its
    // EOI comes long before the file's end, which end detection does not stop at.
    const test::JpegMarker app1 = markerOf(withThumbnail, 0xE1);
    const std::size_t thumbnailStart = app1.offset + 4 + 6 + 44;
    const std::size_t thumbnailEnd = app1.offset + 2 + app1.length;
    const Bytes thumbnail(withThumbnail.begin() + static_cast<std::ptrdiff_t>(thumbnailStart),
                          withThumbnail.begin() + static_cast<std::ptrdiff_t>(thumbnailEnd));
    EXPECT_TRUE(isIntact(format(), thumbnail));
    EXPECT_EQ(endOf(format(), withThumbnail).length, withThumbnail.size());

    options = JpegOptions{};
    options.fillBytes = 5;
    options.restartInterval = 4;
    options.jfif = false;
    EXPECT_TRUE(isIntact(format(), test::makeJpeg(options)));
}

TEST(JpegFormatTest, FilesFromRealEncodersAreIntact) {
    std::size_t checked = 0;
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format != "jpeg") {
            continue;
        }
        SCOPED_TRACE(std::string(sample.name) + " by " + std::string(sample.producer));
        const Bytes file = sample.data();
        EXPECT_TRUE(isIntact(format(), file));
        EXPECT_TRUE(testing::prefixesAreTruncated(format(), file));
        ++checked;
    }
    // Baseline, progressive, restart intervals, gray and optimized, 4:4:4,
    // arithmetic coding, progressive with restarts, GDI+.
    EXPECT_GE(checked, 8u);
}

TEST(JpegFormatTest, EveryPrefixIsTruncated) {
    JpegOptions options = jpegOptions(40, 24, JpegSampling::Yuv420);
    options.restartInterval = 2;
    options.exifThumbnail = true;
    options.fillBytes = 1;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeJpeg(options)));
    options.progressive = true;
    EXPECT_TRUE(testing::prefixesAreTruncated(format(), test::makeJpeg(options)));
}

TEST(JpegFormatTest, BytesAfterTheEndMakeTheContentInvalid) {
    const Bytes file = test::makeJpeg();
    const Bytes longer = concat({file, testing::noise(100, 5)});
    EXPECT_TRUE(isInvalid(format(), longer, file.size()));
    EXPECT_EQ(verdictOf(format(), longer).validBytes, file.size());
    const carving::EndDetection end = endOf(format(), longer);
    EXPECT_EQ(end.status, EndStatus::Found);
    EXPECT_EQ(end.length, file.size());
}

TEST(JpegFormatTest, HeaderCheckAcceptsWhatEncodersWrite) {
    for (const test::samples::Sample& sample : test::samples::all()) {
        if (sample.format == "jpeg") {
            EXPECT_TRUE(testing::headerOf(format(), sample.data()).plausible) << sample.name;
        }
    }
    // A header cut short (the carve will be truncated) is not refused.
    EXPECT_TRUE(testing::headerOf(format(), testing::prefix(test::makeJpeg(), 30)).plausible);
}

TEST(JpegFormatTest, HeaderCheckRejectsFalseSignatures) {
    const Bytes jfif = test::makeJpeg();
    const std::vector<std::pair<Bytes, std::string>> cases = {
        {overwritten(jfif, 3, {0x00}), "no marker code"},
        {overwritten(jfif, 3, {0xD8}), "SOI after SOI"},
        {overwritten(jfif, 3, {0xD9}), "EOI after SOI"},
        {overwritten(jfif, 3, {0xDA}), "SOS first"},
        {overwritten(jfif, 3, {0x01}), "TEM first"},
        {overwritten(jfif, 3, {0xD0}), "RST first"},
        {overwritten(jfif, 3, {0x55}), "reserved marker"},
        {overwritten(jfif, 4, {0x00, 0x01}), "segment length 1"},
        {overwritten(jfif, 20, {0x42}), "no marker after APP0"},
        {overwritten(jfif, 20, {0xFF, 0xD9}), "EOI inside the header"},
        {Bytes(8, std::byte{0}), "no SOI"},
    };
    for (const auto& [bytes, what] : cases) {
        const carving::HeaderCheck check = testing::headerOf(format(), bytes);
        EXPECT_FALSE(check.plausible) << what;
        EXPECT_FALSE(check.reason.empty()) << what;
    }
}

TEST(JpegFormatTest, DamagedSegmentLengthsBreakTheStructure) {
    const Bytes file = test::makeJpeg();
    const test::JpegMarker dqt = markerOf(file, 0xDB);
    for (const std::uint16_t length : {std::uint16_t{0}, std::uint16_t{1}}) {
        Bytes damaged = file;
        storeBe16(damaged, dqt.offset + 2, length);
        const carving::EndDetection end = endOf(format(), damaged);
        EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
        EXPECT_EQ(end.length, dqt.offset);
        EXPECT_TRUE(isInvalid(format(), damaged, dqt.offset));
    }
    // A length that points into the middle of the next segment.
    Bytes longer = file;
    storeBe16(longer, dqt.offset + 2, static_cast<std::uint16_t>(dqt.length + 3));
    EXPECT_NE(endOf(format(), longer).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), longer));
    // A length running beyond the data.
    Bytes huge = testing::prefix(file, dqt.offset + 100);
    storeBe16(huge, dqt.offset + 2, 0xFFFF);
    EXPECT_EQ(endOf(format(), huge).status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), huge).status, ValidationStatus::Truncated);
}

TEST(JpegFormatTest, ReservedMarkersAndStrayBytesBreakTheStructure) {
    const Bytes file = test::makeJpeg();
    const test::JpegMarker dqt = markerOf(file, 0xDB);
    for (const std::uint8_t code : {std::uint8_t{0x02}, std::uint8_t{0x4F}, std::uint8_t{0xBF}, std::uint8_t{0x00}}) {
        const Bytes damaged = overwritten(file, dqt.offset + 1, {code});
        const carving::EndDetection end = endOf(format(), damaged);
        EXPECT_EQ(end.status, EndStatus::Broken) << static_cast<int>(code);
        EXPECT_EQ(end.length, dqt.offset);
    }
    const Bytes stray = overwritten(file, dqt.offset, {0x12});
    EXPECT_EQ(endOf(format(), stray).status, EndStatus::Broken);
    // Reserved extension markers (JPG, JPGn) are skipped by length but make the file invalid.
    const Bytes extension = overwritten(file, dqt.offset + 1, {0xF3});
    EXPECT_TRUE(isInvalid(format(), extension, dqt.offset));
}

TEST(JpegFormatTest, InconsistentFrameAndScanHeadersAreInvalid) {
    const Bytes file = test::makeJpeg(jpegOptions(64, 48, JpegSampling::Yuv420));
    const test::JpegMarker sof = markerOf(file, 0xC0);
    const test::JpegMarker sos = markerOf(file, 0xDA);
    const std::size_t frame = sof.offset + 4;  // P, Y, X, Nf, components
    const std::size_t scan = sos.offset + 4;   // Ns, components, Ss, Se, AhAl
    const std::vector<std::pair<Bytes, std::string>> cases = {
        {overwritten(file, frame, {12}), "baseline precision 12"},
        {overwritten(file, frame + 3, {0, 0}), "width 0"},
        {overwritten(file, frame + 6 + 1, {0x52}), "sampling factor 5"},
        {overwritten(file, frame + 6 + 2, {7}), "quantization table 7"},
        {overwritten(file, frame + 9, {1}), "component id repeated"},
        {overwritten(file, scan + 1, {9}), "scan names a component the frame lacks"},
        {overwritten(file, scan + 2, {0x55}), "entropy table 5"},
        {overwritten(file, scan + 1, {2}), "component repeated in the scan"},
        {overwritten(file, scan + 7, {70}), "spectral selection beyond 63"},
    };
    for (const auto& [damaged, what] : cases) {
        SCOPED_TRACE(what);
        // The structure itself still ends at EOI: end detection finds it.
        const carving::EndDetection end = endOf(format(), damaged);
        EXPECT_EQ(end.status, EndStatus::Found) << testing::describe(end);
        EXPECT_TRUE(isInvalid(format(), damaged, std::max(sof.offset, sos.offset)));
    }
    // Frame and scan lengths that disagree with their component counts.
    Bytes components = file;
    components[frame + 5] = std::byte{4};
    EXPECT_TRUE(isInvalid(format(), components));
    components = file;
    components[scan] = std::byte{5};
    EXPECT_TRUE(isInvalid(format(), components));
}

TEST(JpegFormatTest, ProgressiveScanParametersAreChecked) {
    JpegOptions options = jpegOptions(32, 32, JpegSampling::Yuv444);
    options.progressive = true;
    const Bytes file = test::makeJpeg(options);
    ASSERT_TRUE(isIntact(format(), file));
    const test::JpegMarker dcScan = markerOf(file, 0xDA, 0);
    const test::JpegMarker acScan = markerOf(file, 0xDA, 1);
    // A DC scan must have Se = 0; an AC scan must hold one component and Se >= Ss.
    const std::size_t dcSe = dcScan.offset + 4 + 1 + 2 * 3 + 1;
    const std::size_t acSs = acScan.offset + 4 + 1 + 2;
    EXPECT_TRUE(isInvalid(format(), overwritten(file, dcSe, {5})));
    EXPECT_TRUE(isInvalid(format(), overwritten(file, acSs, {40, 30})));
    EXPECT_TRUE(isInvalid(format(), overwritten(file, acSs + 2, {0xEE})));
}

TEST(JpegFormatTest, MissingOrDamagedTablesAreInvalid) {
    const Bytes file = test::makeJpeg();
    // Without DQT the scan's components have no quantization tables.
    const Bytes noQuantization = withoutSegment(file, 0xDB);
    EXPECT_EQ(endOf(format(), noQuantization).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), noQuantization));
    // DQT: precision 2; a table cut short.
    const test::JpegMarker dqt = markerOf(file, 0xDB);
    EXPECT_TRUE(isInvalid(format(), overwritten(file, dqt.offset + 4, {0x20})));
    Bytes shortTable = file;
    storeBe16(shortTable, dqt.offset + 2, static_cast<std::uint16_t>(dqt.length - 1));
    EXPECT_TRUE(isInvalid(format(), shortTable));
    // DHT: class 2; more codes of one length than the length allows; a DC symbol beyond 16.
    const test::JpegMarker dht = markerOf(file, 0xC4);
    const std::size_t table = dht.offset + 4;
    EXPECT_TRUE(isInvalid(format(), overwritten(file, table, {0x20})));
    EXPECT_TRUE(isInvalid(format(), overwritten(file, table + 1, {2})));  // two 1-bit codes: 0 and the all-ones 1
    EXPECT_TRUE(isInvalid(format(), overwritten(file, table + 17, {40})));
    // Huffman tables are not required: Motion-JPEG frames rely on the standard tables.
    EXPECT_EQ(verdictOf(format(), withoutSegment(file, 0xC4)).status, ValidationStatus::Valid);
}

TEST(JpegFormatTest, MarkerOrderIsChecked) {
    const Bytes file = test::makeJpeg();
    // EOI right after SOI.
    const Bytes empty = testing::overwritten(testing::prefix(file, 4), 2, {0xFF, 0xD9});
    EXPECT_EQ(endOf(format(), empty).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), empty));
    // A frame without a scan.
    const test::JpegMarker sos = markerOf(file, 0xDA);
    const Bytes noScan = concat({testing::prefix(file, sos.offset), test::jpegSegment(0xFE, {}), Bytes{std::byte{0xFF}, std::byte{0xD9}}});
    EXPECT_EQ(endOf(format(), noScan).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), noScan));
    // A scan before the frame header.
    const Bytes noFrame = withoutSegment(file, 0xC0);
    EXPECT_TRUE(isInvalid(format(), noFrame));
    // A second frame header after the first image's data: another image follows.
    const Bytes second = test::makeJpeg(jpegOptions(16, 16, JpegSampling::Gray));
    const Bytes joined = concat({testing::prefix(file, file.size() - 2),
                                 std::span<const std::byte>(second).subspan(2)});
    const carving::EndDetection end = endOf(format(), joined);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_GE(end.length, file.size() - 2);
    // Restart and TEM markers between segments are passed over, as decoders do.
    const test::JpegMarker dqt = markerOf(file, 0xDB);
    const Bytes stray = inserted(file, dqt.offset, Bytes{std::byte{0xFF}, std::byte{0xD3}, std::byte{0xFF}, std::byte{0x01}});
    EXPECT_TRUE(isIntact(format(), stray));
}

TEST(JpegFormatTest, InvalidMarkersInEntropyCodedDataBreakTheStructure) {
    const Bytes file = test::makeJpeg(jpegOptions(64, 64, JpegSampling::Yuv420));
    const auto [data, dataEnd] = firstScanData(file);
    ASSERT_GT(dataEnd - data, 100u);
    const std::size_t at = data + (dataEnd - data) / 2;
    // A reserved marker, another file's SOI, and a second frame header (whose
    // length field is entropy-coded data, so it says nothing).
    for (const std::uint8_t code : {std::uint8_t{0x05}, std::uint8_t{0xD8}, std::uint8_t{0xC0}}) {
        SCOPED_TRACE(static_cast<int>(code));
        const Bytes damaged = overwritten(file, at, {0xFF, code});
        const carving::EndDetection end = endOf(format(), damaged);
        EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
        EXPECT_EQ(end.length, at);
        EXPECT_TRUE(isInvalid(format(), damaged, at));
        // What a carve keeps is consistent as far as it goes.
        EXPECT_EQ(verdictOf(format(), testing::prefix(damaged, at)).status, ValidationStatus::Truncated);
    }
}

TEST(JpegFormatTest, RestartMarkersAreCheckedForOrderAndCount) {
    JpegOptions options = jpegOptions(64, 64, JpegSampling::Yuv420);
    options.restartInterval = 1;  // 16 MCUs: RST0..RST7, RST0..RST6
    const Bytes file = test::makeJpeg(options);
    ASSERT_TRUE(isIntact(format(), file));
    const test::JpegMarker rst2 = markerOf(file, 0xD2);
    // Out of order.
    const Bytes swapped = overwritten(file, rst2.offset + 1, {0xD5});
    EXPECT_EQ(endOf(format(), swapped).status, EndStatus::Broken);
    EXPECT_EQ(endOf(format(), swapped).length, rst2.offset);
    EXPECT_TRUE(isInvalid(format(), swapped, rst2.offset));
    // Restart markers in a scan without a restart interval.
    const Bytes noInterval = withoutSegment(file, 0xDD);
    EXPECT_EQ(endOf(format(), noInterval).status, EndStatus::Broken);
    // An interval that does not match the number of markers.
    const test::JpegMarker dri = markerOf(file, 0xDD);
    Bytes wrongInterval = file;
    storeBe16(wrongInterval, dri.offset + 4, 2);
    EXPECT_EQ(endOf(format(), wrongInterval).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), wrongInterval));
    // The last interval missing: its data and marker cut out.
    const std::vector<test::JpegMarker> markers = test::jpegMarkers(file);
    const auto last = std::find_if(markers.rbegin(), markers.rend(), [](const test::JpegMarker& m) {
        return m.code >= 0xD0 && m.code <= 0xD7;
    });
    ASSERT_NE(last, markers.rend());
    const test::JpegMarker eoi = markers.back();
    const Bytes lastIntervalLost = testing::erased(file, last->offset, eoi.offset - last->offset);
    EXPECT_EQ(endOf(format(), lastIntervalLost).status, EndStatus::Found);
    EXPECT_TRUE(isInvalid(format(), lastIntervalLost));
}

TEST(JpegFormatTest, FragmentedFilesAreNotValid) {
    JpegOptions options = jpegOptions(96, 96, JpegSampling::Yuv420);
    const Bytes file = test::makeJpeg(options);
    const auto [data, dataEnd] = firstScanData(file);
    const std::size_t split = data + (dataEnd - data) / 3;
    // Another file's data (noise with 0xFF bytes) between the fragments.
    const Bytes foreign = inserted(file, split, testing::noise(4096, 77));
    const carving::EndDetection end = endOf(format(), foreign);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_GE(end.length, split);
    EXPECT_LT(end.length, split + 4096);
    EXPECT_TRUE(isInvalid(format(), foreign));
    // The start of another JPEG between the fragments.
    const Bytes other = inserted(file, split, test::makeJpeg(jpegOptions(16, 16, JpegSampling::Gray)));
    EXPECT_EQ(endOf(format(), other).status, EndStatus::Broken);
    EXPECT_EQ(endOf(format(), other).length, split);
    // Data overwritten in place (a cluster of another file, here zeros) takes
    // the restart markers of that part with it, and the count no longer fits.
    options.restartInterval = 2;
    const Bytes restarts = test::makeJpeg(options);
    const auto [rData, rDataEnd] = firstScanData(restarts);
    const std::size_t rSplit = rData + (rDataEnd - rData) / 3;
    const Bytes gap((rDataEnd - rSplit) / 2, std::byte{0});
    const Bytes overwrittenScan = testing::overwritten(restarts, rSplit, gap);
    EXPECT_TRUE(isInvalid(format(), overwrittenScan));
    // Marker-free data between two fragments of the same file, on the other
    // hand, reads as entropy-coded data and is not caught (L62): only
    // decoding the scan would show it.
    const Bytes zeros = inserted(restarts, rSplit, gap);
    EXPECT_EQ(verdictOf(format(), zeros).status, ValidationStatus::Valid);
}

TEST(JpegFormatTest, ErasedMediaAfterACutFileBreaksTheStructure) {
    const Bytes file = test::makeJpeg(jpegOptions(64, 64, JpegSampling::Yuv420));
    const auto [data, dataEnd] = firstScanData(file);
    const std::size_t cut = data + (dataEnd - data) / 2;
    // Erased flash reads as 0xFF: the scan runs into fill bytes, which are bounded.
    const Bytes erased = concat({testing::prefix(file, cut), Bytes(64 * 1024, std::byte{0xFF})});
    const carving::EndDetection end = endOf(format(), erased);
    EXPECT_EQ(end.status, EndStatus::Broken) << testing::describe(end);
    EXPECT_LE(end.length, cut + 1);
    // Fill bytes before a marker are fine up to the limit, and too many are not.
    const test::JpegMarker dqt = markerOf(file, 0xDB);
    const Bytes someFill = inserted(file, dqt.offset, Bytes(1000, std::byte{0xFF}));
    EXPECT_TRUE(isIntact(format(), someFill));
    const Bytes tooMuchFill = inserted(file, dqt.offset, Bytes(JpegFormat::kMaxFillBytes + 10, std::byte{0xFF}));
    EXPECT_EQ(endOf(format(), tooMuchFill).status, EndStatus::Broken);
    // Zeros after a cut scan are valid entropy-coded data: the file is truncated where the data ends (L61).
    const Bytes zeros = concat({testing::prefix(file, cut), Bytes(64 * 1024, std::byte{0})});
    EXPECT_EQ(endOf(format(), zeros).status, EndStatus::Truncated);
}

TEST(JpegFormatTest, FuzzedFilesStayWithinTheirData) {
    JpegOptions options = jpegOptions(40, 40, JpegSampling::Yuv420);
    options.restartInterval = 3;
    options.exifThumbnail = true;
    testing::fuzz(format(), test::makeJpeg(options), 1500, 11);
    options.progressive = true;
    testing::fuzz(format(), test::makeJpeg(options), 1500, 12);
    testing::fuzz(format(), test::samples::named("cjpeg_arithmetic.jpg").data(), 500, 13);
}

TEST(JpegFormatTest, HostileInputsEndQuickly) {
    const Bytes soi = {std::byte{0xFF}, std::byte{0xD8}};
    // Only fill bytes; only segment headers with length 2; a huge DHT count.
    const Bytes fill = concat({soi, Bytes(1 * kMiB, std::byte{0xFF})});
    EXPECT_EQ(endOf(format(), fill).status, EndStatus::Broken);
    Bytes empties = soi;
    for (int i = 0; i < 100'000; ++i) {
        empties.insert(empties.end(), {std::byte{0xFF}, std::byte{0xFE}, std::byte{0x00}, std::byte{0x02}});
    }
    EXPECT_EQ(endOf(format(), empties).status, EndStatus::Truncated);
    EXPECT_EQ(verdictOf(format(), empties).status, ValidationStatus::Truncated);
    Bytes dht = concat({soi, test::jpegSegment(0xC4, Bytes(17, std::byte{0xFF}))});
    EXPECT_EQ(verdictOf(format(), dht).status, ValidationStatus::Invalid);
}

}  // namespace
}  // namespace recovery::formats
