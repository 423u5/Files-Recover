#include "formats/mp4_analysis.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace recovery::formats::mp4 {

namespace {

using carving::IContentReader;
using Bytes = std::span<const std::byte>;

constexpr std::array<FourCc, 5> kAudioBrands = {FourCc("M4A "), FourCc("M4B "), FourCc("M4P "), FourCc("F4A "),
                                                FourCc("F4B ")};
constexpr std::array<FourCc, 6> kVideoBrands = {FourCc("M4V "), FourCc("M4VH"), FourCc("M4VP"),
                                                FourCc("F4V "), FourCc("F4P "), FourCc("qt  ")};
constexpr std::array<FourCc, 13> kImageBrands = {FourCc("heic"), FourCc("heix"), FourCc("heim"), FourCc("heis"),
                                                 FourCc("hevc"), FourCc("hevx"), FourCc("mif1"), FourCc("msf1"),
                                                 FourCc("avif"), FourCc("avis"), FourCc("crx "), FourCc("jp2 "),
                                                 FourCc("mjp2")};

template <std::size_t N>
bool isOneOf(FourCc code, const std::array<FourCc, N>& codes) noexcept {
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

std::string quoted(FourCc code) {
    return "'" + code.text() + "'";
}

// Why a sample's NAL units do not fill it, or nothing when they do.
Result<std::optional<std::string>> checkSample(IContentReader& content, std::uint64_t offset, std::uint32_t size,
                                               std::uint8_t lengthSize) {
    std::uint64_t position = 0;
    while (position < size) {
        const std::uint64_t left = size - position;
        if (left < std::uint64_t{lengthSize} + 1) {
            return std::optional<std::string>("its last " + std::to_string(left) +
                                              (left == 1 ? " byte is" : " bytes are") + " too few for a NAL unit");
        }
        Result<Bytes> bytes = content.read(offset + position, std::size_t{lengthSize} + 1);
        if (!bytes.ok()) {
            return bytes.error();
        }
        std::uint64_t length = 0;
        for (std::size_t i = 0; i < lengthSize; ++i) {
            length = (length << 8) | loadU8(*bytes, i);
        }
        const std::string at = " at +" + std::to_string(position);
        if (length == 0) {
            return std::optional<std::string>("a NAL unit of length 0" + at);
        }
        if ((loadU8(*bytes, lengthSize) & 0x80U) != 0) {
            return std::optional<std::string>("a NAL unit whose forbidden zero bit is set" + at);
        }
        if (length > left - lengthSize) {
            return std::optional<std::string>("a NAL unit of " + std::to_string(length) + " bytes" + at +
                                              " runs past the sample's " + std::to_string(size) + " bytes");
        }
        position += lengthSize + length;
    }
    return std::optional<std::string>{};
}

}  // namespace

BrandClass classifyBrand(FourCc majorBrand) noexcept {
    if (isOneOf(majorBrand, kAudioBrands)) {
        return BrandClass::Audio;
    }
    if (isOneOf(majorBrand, kVideoBrands)) {
        return BrandClass::Video;
    }
    if (isOneOf(majorBrand, kImageBrands)) {
        return BrandClass::Image;
    }
    return BrandClass::Generic;
}

Classification classify(const std::optional<FileType>& fileType, const Movie* movie) {
    if (fileType.has_value()) {
        const FourCc major = fileType->majorBrand;
        if (!major.isPrintable()) {
            return {MediaKind::Neither, "the major brand is not printable"};
        }
        switch (classifyBrand(major)) {
        case BrandClass::Audio:
            return {MediaKind::Audio, "audio brand " + quoted(major)};
        case BrandClass::Video:
            return {MediaKind::Video, "video brand " + quoted(major)};
        case BrandClass::Image:
            return {MediaKind::Neither, "brand " + quoted(major) + " is an image's"};
        case BrandClass::Generic:
            break;
        }
    }
    const std::string brand = fileType.has_value() ? "brand " + quoted(fileType->majorBrand) : "no ftyp";
    if (movie == nullptr) {
        return {MediaKind::Video, brand + " without a moov box: audio cannot be told from video"};
    }
    const std::size_t video = movie->count(TrackKind::Video);
    const std::size_t sound = movie->count(TrackKind::Audio);
    if (sound > 0 && video == 0) {
        return {MediaKind::Audio, brand + " with only sound tracks"};
    }
    return {MediaKind::Video, video > 0 ? brand + " with a video track" : brand + " without a sound track"};
}

std::optional<MediaExtent> mediaExtent(const Movie& movie) {
    std::optional<MediaExtent> extent;
    for (const Track& track : movie.tracks) {
        for (const bool run : {false, true}) {
            for (const Chunk& chunk : run ? track.fragmentRuns : track.chunks) {
                if (chunk.size == 0) {
                    continue;
                }
                if (!extent.has_value()) {
                    extent = MediaExtent{chunk.offset, chunk.end(), 0, 0};
                }
                extent->begin = std::min(extent->begin, chunk.offset);
                extent->end = std::max(extent->end, chunk.end());
                extent->samples += chunk.sampleCount;
                extent->bytes += chunk.size;
            }
        }
    }
    return extent;
}

Result<std::optional<FoundMovie>> findMovie(IContentReader& content, std::uint64_t from, std::uint64_t to,
                                            const ParseLimits& limits, std::size_t maxAttempts) {
    if (Status valid = validate(limits); !valid.ok()) {
        return valid.error();
    }
    to = std::min(to, content.size());
    constexpr std::array<std::byte, 4> kType = {std::byte{'m'}, std::byte{'o'}, std::byte{'o'}, std::byte{'v'}};
    constexpr std::uint64_t kTypeAt = 4;
    constexpr std::uint64_t kSmallestMoov = 16;  // a header and one child header
    if (from >= to || to - from < kSmallestMoov) {
        return std::optional<FoundMovie>{};
    }
    std::uint64_t position = from + kTypeAt;
    std::size_t attempts = 0;
    while (attempts < maxAttempts && position < to) {
        Result<std::optional<std::uint64_t>> found = carving::findPattern(content, kType, position, to);
        if (!found.ok()) {
            return found.error();
        }
        if (!found->has_value()) {
            break;
        }
        const std::uint64_t start = **found - kTypeAt;
        position = **found + 1;
        Result<BoxRead> header = readBoxHeader(content, start, to);
        if (!header.ok()) {
            return header.error();
        }
        if (header->status != BoxStatus::Valid || header->header.sizeKind == BoxSize::ToEnd ||
            header->header.size < kSmallestMoov) {
            continue;
        }
        ++attempts;
        Result<Movie> movie = parseMovie(content, header->header, limits);
        if (!movie.ok()) {
            return movie.error();
        }
        const bool usable = std::any_of(movie->tracks.begin(), movie->tracks.end(),
                                        [](const Track& track) { return track.sampleTableValid; });
        if (usable) {
            return std::optional<FoundMovie>(FoundMovie{header->header, std::move(*movie)});
        }
    }
    return std::optional<FoundMovie>{};
}

std::uint64_t FramingCheck::checked() const noexcept {
    std::uint64_t total = 0;
    for (const TrackFraming& track : tracks) {
        total += track.checked;
    }
    return total;
}

std::uint64_t FramingCheck::bad() const noexcept {
    std::uint64_t total = 0;
    for (const TrackFraming& track : tracks) {
        total += track.bad;
    }
    return total;
}

std::string FramingCheck::describeFirstBad() const {
    for (const TrackFraming& track : tracks) {
        if (track.firstBadSample.has_value()) {
            return "sample " + std::to_string(*track.firstBadSample + 1) + " of track " + std::to_string(track.track) +
                   " at offset " + std::to_string(track.firstBadOffset) + ": " + track.firstBadDetail;
        }
    }
    return {};
}

Result<FramingCheck> checkSampleFraming(IContentReader& content, const Movie& movie) {
    FramingCheck check;
    for (std::size_t t = 0; t < movie.tracks.size(); ++t) {
        const Track& track = movie.tracks[t];
        const std::vector<SampleDescription>& descriptions = track.samples.descriptions;
        const bool framed = track.kind == TrackKind::Video &&
                            std::any_of(descriptions.begin(), descriptions.end(),
                                        [](const SampleDescription& d) { return d.nalLengthSize != 0; });
        if (!framed) {
            continue;
        }
        TrackFraming result;
        result.track = static_cast<std::uint32_t>(t + 1);
        for (const bool run : {false, true}) {
            const std::uint64_t base = run ? track.samples.sampleCount : 0;
            for (const Chunk& chunk : run ? track.fragmentRuns : track.chunks) {
                const bool placed = chunk.placement == ChunkPlacement::MediaData ||
                                    (chunk.placement == ChunkPlacement::Unchecked && chunk.end() <= content.size());
                if (!placed || chunk.sampleDescriptionIndex == 0 ||
                    chunk.sampleDescriptionIndex > descriptions.size()) {
                    continue;
                }
                const std::uint8_t lengthSize = descriptions[chunk.sampleDescriptionIndex - 1].nalLengthSize;
                if (lengthSize == 0) {
                    continue;
                }
                std::uint64_t offset = chunk.offset;
                for (std::uint32_t i = 0; i < chunk.sampleCount; ++i) {
                    const std::uint32_t index = chunk.firstSample + i;
                    const std::uint32_t size =
                        run ? track.fragmentSampleSizes[index] : track.samples.sampleSize(index);
                    Result<std::optional<std::string>> problem = checkSample(content, offset, size, lengthSize);
                    if (!problem.ok()) {
                        return problem.error();
                    }
                    ++result.checked;
                    if (problem->has_value()) {
                        if (result.bad++ == 0) {
                            result.firstBadSample = base + index;
                            result.firstBadOffset = offset;
                            result.firstBadDetail = std::move(**problem);
                        }
                    }
                    offset += size;
                }
            }
        }
        check.tracks.push_back(std::move(result));
    }
    return check;
}

}  // namespace recovery::formats::mp4
