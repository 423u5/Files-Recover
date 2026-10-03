#include "iso_walk.hpp"

#include "recovery/byte_order.hpp"

#include <algorithm>
#include <utility>

namespace recovery::formats::detail {

namespace {

using carving::IContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;
using mp4::BoxHeader;
using mp4::FourCc;
using Bytes = std::span<const std::byte>;

constexpr std::size_t kBoxHeader = 8;
constexpr std::size_t kFtypFields = 8;  // major brand and minor version

std::string name(FourCc code) {
    return "'" + code.text() + "'";
}

// Whether `bytes`, fewer than a whole header, could be the start of a box
// header: a size of 0, 1 or at least 8, and printable type bytes as far as
// there are any.
bool couldStartBox(Bytes bytes) noexcept {
    if (bytes.size() >= 4) {
        const std::uint32_t size = loadBe32(bytes, 0);
        if (size != 0 && size != 1 && size < kBoxHeader) {
            return false;
        }
    }
    for (std::size_t i = 4; i < std::min<std::size_t>(bytes.size(), kBoxHeader); ++i) {
        const auto c = static_cast<std::uint8_t>(bytes[i]);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

// The first chunk or fragment run that lies beyond the data.
struct Beyond {
    std::uint64_t offset = 0;
    std::size_t track = 0;
};

std::optional<Beyond> firstBeyondData(const mp4::Movie& movie) {
    for (std::size_t t = 0; t < movie.tracks.size(); ++t) {
        for (const bool run : {false, true}) {
            for (const mp4::Chunk& chunk : run ? movie.tracks[t].fragmentRuns : movie.tracks[t].chunks) {
                if (chunk.placement == mp4::ChunkPlacement::BeyondData) {
                    return Beyond{chunk.offset, t + 1};
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

Result<IsoStart> readIsoStart(IContentReader& content, std::uint32_t maxFtypSize) {
    IsoStart start;
    const auto early = [&](WalkStatus status, std::string detail) {
        Walk walk;
        walk.finish(status, 0, std::move(detail));
        start.early = std::move(walk);
        return start;
    };
    Result<std::optional<Bytes>> header = readIfAvailable(content, 0, kBoxHeader);
    if (!header.ok()) {
        return header.error();
    }
    if (!header->has_value()) {
        return early(WalkStatus::Truncated, "the data ends inside the ftyp box header");
    }
    const std::uint32_t size = loadBe32(**header, 0);
    if (FourCc::at(**header, 4) != mp4::box::kFtyp || size < kBoxHeader + kFtypFields || size > maxFtypSize ||
        (size - kBoxHeader - kFtypFields) % 4 != 0) {
        return early(WalkStatus::Broken, "no valid ftyp box");
    }
    if (size > content.size()) {
        return early(WalkStatus::Truncated, "the data ends inside the ftyp box");
    }
    Result<Bytes> brand = content.read(kBoxHeader, 4);
    if (!brand.ok()) {
        return brand.error();
    }
    start.ftypSize = size;
    start.brand = FourCc::at(*brand, 0);
    return start;
}

Result<IsoLayout> walkIsoTopLevel(IContentReader& content, std::uint64_t start) {
    IsoLayout layout;
    const std::uint64_t size = content.size();
    std::uint64_t position = start;
    const auto stop = [&](IsoStop how, std::string what) -> Result<IsoLayout> {
        layout.stop = how;
        layout.position = position;
        layout.what = std::move(what);
        return std::move(layout);
    };
    for (;;) {
        if (position >= size) {
            return stop(IsoStop::DataEnded, {});
        }
        const bool complete = layout.moov.has_value() && !layout.mediaData.empty();
        Result<mp4::BoxRead> read = mp4::readBoxHeader(content, position, size);
        if (!read.ok()) {
            return read.error();
        }
        const BoxHeader& box = read->header;
        if (read->status == mp4::BoxStatus::HeaderCut) {
            // The start of a box, cut off by the end of the data?
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(size - position, 16));
            Result<Bytes> rest = content.read(position, length);
            if (!rest.ok()) {
                return rest.error();
            }
            const bool plausible = length >= kBoxHeader ? box.type.isPrintable() : couldStartBox(*rest);
            return stop(!complete || plausible ? IsoStop::Truncated : IsoStop::NotABox,
                        "the data ends inside a box header");
        }
        if (read->status == mp4::BoxStatus::BadSize || !box.type.isPrintable() || box.type == mp4::box::kFtyp ||
            (complete && !mp4::isTopLevelType(box.type))) {
            return stop(IsoStop::NotABox, "the next bytes are not a box of the file");
        }
        if (box.sizeKind == mp4::BoxSize::ToEnd) {
            layout.sizeUnknown = box;
            return stop(IsoStop::SizeUnknown,
                        "box " + name(box.type) + " has size 0 (to the end of the file): its length is unknown");
        }
        if (read->status == mp4::BoxStatus::TooLong) {
            return stop(IsoStop::Truncated, "the data ends inside box " + name(box.type));
        }
        ++layout.boxes;
        layout.lastBox = box;
        if (box.type == mp4::box::kMoov) {
            if (layout.moov.has_value()) {
                layout.secondMoov = layout.secondMoov.value_or(position);
            } else {
                layout.moov = box;
                layout.moovFirst = layout.mediaData.empty();
            }
        } else if (box.type == mp4::box::kMdat) {
            layout.mediaData.push_back(box);
        } else if (box.type == mp4::box::kMoof) {
            layout.fragments = true;
        }
        position = box.end();
    }
}

Walk finishIsoWalk(const IsoLayout& layout, std::string summary) {
    Walk walk;
    switch (layout.stop) {
    case IsoStop::Truncated:
        return walk.finish(WalkStatus::Truncated, layout.position, layout.what);
    case IsoStop::SizeUnknown:
        return walk.finish(WalkStatus::Broken, layout.position, layout.what);
    case IsoStop::DataEnded:
    case IsoStop::NotABox:
        break;
    }
    if (!layout.complete()) {
        const std::string missing = !layout.moov.has_value() ? "moov" : "mdat";
        if (layout.stop == IsoStop::DataEnded) {
            return walk.finish(WalkStatus::Truncated, layout.position, "the data ends before the " + missing + " box");
        }
        return walk.finish(WalkStatus::Broken, layout.position, "the boxes stop before the " + missing + " box");
    }
    if (layout.lastBox.has_value() && layout.lastBox->type == mp4::box::kMoof) {
        if (layout.stop == IsoStop::DataEnded) {
            return walk.finish(WalkStatus::Truncated, layout.position,
                               "the data ends after a moof box, before its media data");
        }
        return walk.finish(WalkStatus::Broken, layout.lastBox->offset,
                           "the moof box at " + std::to_string(layout.lastBox->offset) +
                               " is not followed by its media data");
    }
    return walk.finish(WalkStatus::Complete, layout.position, std::move(summary));
}

ValidationResult isoVerdict(const mp4::Mp4File& file, std::uint64_t contentSize, mp4::MediaKind wanted,
                            bool beyondIsInvalid) {
    const mp4::Movie* movie = file.movie.has_value() ? &*file.movie : nullptr;
    const mp4::Classification kind = mp4::classify(file.fileType, movie);
    // Without an ftyp or a movie (the data ends inside the ftyp box) there is
    // nothing to tell the kind by yet.
    const bool known = file.fileType.has_value() || movie != nullptr;
    if (known && kind.kind != wanted) {
        return ValidationResult{ValidationStatus::Invalid, 0,
                                kind.reason + (wanted == mp4::MediaKind::Audio ? " (not an audio file)"
                                                                               : " (not a video file)")};
    }
    if (wanted == mp4::MediaKind::Audio && movie != nullptr && movie->count(mp4::TrackKind::Audio) == 0) {
        return ValidationResult{ValidationStatus::Invalid, std::min(movie->box.offset, contentSize), "no sound track"};
    }
    switch (file.status) {
    case mp4::FileStatus::Valid:
        // As for end detection, a file needs its media data: a fragmented
        // movie cut after moov (whose tables are empty) parses, but its
        // samples are missing.
        if (file.layout.first(mp4::box::kMdat) == nullptr) {
            return ValidationResult{ValidationStatus::Truncated, contentSize, "the data ends before any mdat box"};
        }
        return ValidationResult{ValidationStatus::Valid, contentSize, file.detail};
    case mp4::FileStatus::Truncated: {
        // The top-level boxes are complete, so media data beyond them is not
        // missing: the chunk offset is wrong. (A last moof's mdat is missing.)
        const mp4::Layout& layout = file.layout;
        const bool complete = layout.end == mp4::LayoutEnd::EndOfData && layout.first(mp4::box::kMdat) != nullptr &&
                              layout.boxes.back().sizeKind != mp4::BoxSize::ToEnd &&
                              layout.boxes.back().type != mp4::box::kMoof;
        if (beyondIsInvalid && complete && movie != nullptr) {
            if (const std::optional<Beyond> beyond = firstBeyondData(*movie); beyond.has_value()) {
                return ValidationResult{ValidationStatus::Invalid, std::min(beyond->offset, contentSize),
                                        "media data of track " + std::to_string(beyond->track) + " at offset " +
                                            std::to_string(beyond->offset) + " lies beyond the end of the file (" +
                                            std::to_string(contentSize) + " bytes), after its last box"};
            }
        }
        return ValidationResult{ValidationStatus::Truncated, contentSize, file.detail};
    }
    case mp4::FileStatus::Invalid:
        break;
    }
    const std::uint64_t at = file.issues.recorded().empty() ? 0 : file.issues.recorded().front().offset;
    return ValidationResult{ValidationStatus::Invalid, std::min(at, contentSize), file.detail};
}

}  // namespace recovery::formats::detail
