#include "formats/m4a_format.hpp"

#include "formats/mp4_analysis.hpp"
#include "formats/mp4_parser.hpp"
#include "iso_walk.hpp"
#include "recovery/byte_order.hpp"
#include "structure_walk.hpp"

#include <optional>
#include <string>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using detail::Walk;
using detail::WalkStatus;
using mp4::BrandClass;
using mp4::FourCc;

constexpr std::size_t kBoxHeader = 8;
constexpr std::size_t kFtypFields = 8;  // major brand and minor version

std::string name(FourCc code) {
    return "'" + code.text() + "'";
}

// Walks an M4A file's top-level boxes and, to tell audio from video, reads
// its movie (iso_walk.hpp).
Result<Walk> walkM4a(IContentReader& content) {
    Walk walk;
    Result<detail::IsoStart> start = detail::readIsoStart(content, M4aFormat::kMaxFtypSize);
    if (!start.ok()) {
        return start.error();
    }
    if (start->early.has_value()) {
        return *start->early;
    }
    const FourCc brand = start->brand;
    const BrandClass brandClass = mp4::classifyBrand(brand);
    if (!brand.isPrintable() || brandClass == BrandClass::Video || brandClass == BrandClass::Image) {
        return walk.finish(WalkStatus::Broken, 0, "brand " + name(brand) + " is not an audio file's");
    }
    Result<detail::IsoLayout> layout = detail::walkIsoTopLevel(content, start->ftypSize);
    if (!layout.ok()) {
        return layout.error();
    }

    // The tracks tell audio from video, whatever the walk's outcome.
    std::optional<mp4::Movie> movie;
    if (layout->moov.has_value()) {
        Result<mp4::Movie> parsed = mp4::parseMovie(content, *layout->moov);
        if (!parsed.ok()) {
            return parsed.error();
        }
        movie = std::move(*parsed);
    }
    if (brandClass == BrandClass::Generic) {
        const mp4::Classification kind =
            mp4::classify(mp4::FileType{brand, 0, {}}, movie.has_value() ? &*movie : nullptr);
        if (kind.kind != mp4::MediaKind::Audio) {
            return walk.finish(WalkStatus::Broken, 0, kind.reason + " (not an audio file)");
        }
    }

    std::string summary = "brand " + name(brand);
    if (movie.has_value()) {
        const std::size_t tracks = movie->tracks.size();
        summary += ", " + std::to_string(tracks) + (tracks == 1 ? " track (" : " tracks (") +
                   std::to_string(movie->count(mp4::TrackKind::Audio)) + " sound)";
    }
    summary += std::string(", moov ") + (layout->moovFirst ? "before" : "after") + " the media data";
    if (layout->fragments) {
        summary += ", fragmented";
    }
    return detail::finishIsoWalk(*layout, std::move(summary));
}

}  // namespace

M4aFormat::M4aFormat() {
    descriptor_.id = "m4a";
    descriptor_.name = "MPEG-4 audio (M4A)";
    descriptor_.extension = "m4a";
    descriptor_.signatures = {carving::textSignature("ISO BMFF ftyp box", "ftyp", 4)};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
}

HeaderCheck M4aFormat::checkHeader(std::span<const std::byte> header) const {
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
    if (brandClass == BrandClass::Video || brandClass == BrandClass::Image) {
        return HeaderCheck::reject("brand " + name(major) + " is a video or image brand");
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

Result<EndDetection> M4aFormat::findEnd(IContentReader& content) const {
    Result<Walk> walk = walkM4a(content);
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> M4aFormat::validate(IContentReader& content) const {
    Result<mp4::Mp4File> file = mp4::parseFile(content);
    if (!file.ok()) {
        return file.error();
    }
    return detail::isoVerdict(*file, content.size(), mp4::MediaKind::Audio, true);
}

}  // namespace recovery::formats
