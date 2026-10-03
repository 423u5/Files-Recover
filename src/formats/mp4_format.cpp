#include "formats/mp4_format.hpp"

#include "formats/mp4_analysis.hpp"
#include "iso_walk.hpp"
#include "recovery/byte_order.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;
using detail::Walk;
using detail::WalkStatus;
using mp4::BrandClass;
using mp4::FourCc;

constexpr std::size_t kBoxHeader = 8;
constexpr std::size_t kFtypFields = 8;  // major brand and minor version

std::string name(FourCc code) {
    return "'" + code.text() + "'";
}

std::string plural(std::uint64_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

// Walks an MP4 file's top-level boxes and reads its movie: whether the file
// is video, and where an mdat of unknown size ends (iso_walk.hpp).
Result<Walk> walkMp4(IContentReader& content, const mp4::ParseLimits& limits) {
    Walk walk;
    Result<detail::IsoStart> start = detail::readIsoStart(content, Mp4Format::kMaxFtypSize);
    if (!start.ok()) {
        return start.error();
    }
    if (start->early.has_value()) {
        return *start->early;
    }
    const FourCc brand = start->brand;
    const BrandClass brandClass = mp4::classifyBrand(brand);
    if (!brand.isPrintable() || brandClass == BrandClass::Audio || brandClass == BrandClass::Image) {
        return walk.finish(WalkStatus::Broken, 0, "brand " + name(brand) + " is not a video file's");
    }
    Result<detail::IsoLayout> layout = detail::walkIsoTopLevel(content, start->ftypSize);
    if (!layout.ok()) {
        return layout.error();
    }
    std::optional<mp4::Movie> movie;
    if (layout->moov.has_value()) {
        Result<mp4::Movie> parsed = mp4::parseMovie(content, *layout->moov, limits);
        if (!parsed.ok()) {
            return parsed.error();
        }
        movie = std::move(*parsed);
    }
    if (brandClass == BrandClass::Generic) {
        const mp4::Classification kind =
            mp4::classify(mp4::FileType{brand, 0, {}}, movie.has_value() ? &*movie : nullptr);
        if (kind.kind != mp4::MediaKind::Video) {
            return walk.finish(WalkStatus::Broken, 0, kind.reason + " (an audio file: M4A)");
        }
    }

    std::string summary = "brand " + name(brand);
    if (movie.has_value()) {
        summary += ", " + plural(movie->tracks.size(), "track", "tracks") + " (" +
                   std::to_string(movie->count(mp4::TrackKind::Video)) + " video, " +
                   std::to_string(movie->count(mp4::TrackKind::Audio)) + " audio)";
    }
    summary += std::string(", moov ") + (layout->moovFirst ? "before" : "after") + " the media data (" +
               plural(layout->mediaData.size(), "mdat box", "mdat boxes") + ")";
    if (layout->fragments) {
        summary += ", fragmented";
    }

    // An mdat of size 0 after moov ("to the end of the file"): the sample
    // tables say where its media data ends.
    const std::optional<mp4::BoxHeader>& open = layout->sizeUnknown;
    if (layout->stop == detail::IsoStop::SizeUnknown && open.has_value() && open->type == mp4::box::kMdat &&
        movie.has_value() && layout->moov->offset < open->offset) {
        const std::optional<mp4::MediaExtent> extent = mp4::mediaExtent(*movie);
        const std::uint64_t end = std::max(open->payloadOffset(), extent.has_value() ? extent->end : 0);
        const std::string what = "mdat at " + std::to_string(open->offset) +
                                 " has size 0 (to the end of the file); the sample tables end its media data at " +
                                 std::to_string(end);
        if (end > content.size()) {
            return walk.finish(WalkStatus::Truncated, content.size(), what + ", beyond the data");
        }
        return walk.finish(WalkStatus::Complete, end, summary + "; " + what);
    }
    // The boxes are complete, but the sample tables need media data after
    // the last of them (a later mdat lost, or stored elsewhere on the disk).
    if (layout->complete() && movie.has_value()) {
        std::uint64_t needed = 0;
        for (const mp4::Track& track : movie->tracks) {
            for (const mp4::Chunk& chunk : track.chunks) {
                needed = std::max(needed, chunk.end());
            }
        }
        if (needed > layout->position) {
            const std::string what = "the sample tables need media data up to offset " + std::to_string(needed);
            if (layout->stop == detail::IsoStop::DataEnded) {
                return walk.finish(WalkStatus::Truncated, content.size(), "the data ends before " + what);
            }
            return walk.finish(WalkStatus::Broken, layout->position,
                               "the boxes stop at " + std::to_string(layout->position) + ", but " + what);
        }
    }
    return detail::finishIsoWalk(*layout, std::move(summary));
}

}  // namespace

Mp4Format::Mp4Format(Mp4FormatOptions options) : options_(options) {
    descriptor_.id = "mp4";
    descriptor_.name = "MPEG-4 video (MP4, MOV, M4V, 3GP)";
    descriptor_.extension = "mp4";
    descriptor_.signatures = {carving::textSignature("ISO BMFF ftyp box", "ftyp", 4)};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
}

HeaderCheck Mp4Format::checkHeader(std::span<const std::byte> header) const {
    if (header.size() < kBoxHeader + kFtypFields) {
        return HeaderCheck::reject("the data ends inside the ftyp box");
    }
    const std::uint32_t size = loadBe32(header, 0);
    if (size < kBoxHeader + kFtypFields || size > kMaxFtypSize || (size - kBoxHeader - kFtypFields) % 4 != 0) {
        return HeaderCheck::reject("ftyp size " + std::to_string(size) + " is not a whole number of brands");
    }
    const FourCc major = FourCc::at(header, kBoxHeader);
    if (!major.isPrintable()) {
        return HeaderCheck::reject("the major brand is not printable");
    }
    const BrandClass brandClass = mp4::classifyBrand(major);
    if (brandClass == BrandClass::Audio || brandClass == BrandClass::Image) {
        return HeaderCheck::reject("brand " + name(major) + " is an audio or image brand");
    }
    if (header.size() >= std::size_t{size} + kBoxHeader) {
        const std::uint32_t nextSize = loadBe32(header, size);
        const FourCc next = FourCc::at(header, size + 4U);
        if (!next.isPrintable() || (nextSize != 0 && nextSize != 1 && nextSize < kBoxHeader)) {
            return HeaderCheck::reject("no box after the ftyp box");
        }
    }
    return HeaderCheck::accept();
}

Result<EndDetection> Mp4Format::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkMp4(content, options_.limits);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> Mp4Format::validate(IContentReader& content) const {
    Result<mp4::Mp4File> file = mp4::parseFile(content, options_.limits);
    if (!file.ok()) {
        return file.error();
    }
    std::optional<mp4::FramingCheck> framing;
    const ValidationStatus parsed = detail::isoVerdict(*file, content.size(), mp4::MediaKind::Video, false).status;
    const bool consistent = parsed == ValidationStatus::Valid || parsed == ValidationStatus::Truncated;
    if (options_.checkSampleFraming && consistent && file->movie.has_value()) {
        Result<mp4::FramingCheck> checked = mp4::checkSampleFraming(content, *file->movie);
        if (!checked.ok()) {
            return checked.error();
        }
        framing = std::move(*checked);
    }
    return mp4Verdict(*file, content.size(), framing);
}

ValidationResult mp4Verdict(const mp4::Mp4File& file, std::uint64_t contentSize,
                            const std::optional<mp4::FramingCheck>& framing) {
    ValidationResult verdict = detail::isoVerdict(file, contentSize, mp4::MediaKind::Video, false);
    const bool consistent = verdict.status == ValidationStatus::Valid || verdict.status == ValidationStatus::Truncated;
    if (!consistent || !framing.has_value()) {
        return verdict;
    }
    if (framing->bad() > 0) {
        std::uint64_t at = contentSize;
        for (const mp4::TrackFraming& track : framing->tracks) {
            if (track.firstBadSample.has_value()) {
                at = std::min(at, track.firstBadOffset);
            }
        }
        return ValidationResult{ValidationStatus::Invalid, at,
                                "the NAL units of " + plural(framing->bad(), "sample", "samples") + " of " +
                                    std::to_string(framing->checked()) + " do not fill them; the first: " +
                                    framing->describeFirstBad()};
    }
    if (framing->checked() > 0) {
        verdict.detail += "; the NAL units of all " + plural(framing->checked(), "video sample", "video samples") +
                          " fill them";
    }
    return verdict;
}

}  // namespace recovery::formats
