#pragma once

// What the two ISO base media carving formats share (private to
// recovery_formats): M4A (P10, audio) and MP4 (P12, video) files both start
// with an ftyp box, and a carve of either ends where its top-level boxes end.
// The walk here finds that end; the MP4 parser (mp4_parser.hpp) then
// validates the carved bytes, and mp4_analysis.hpp's classify() decides
// which of the two formats a file belongs to.
//
// The walk follows the top-level boxes from the ftyp box on, in any order:
// moov before or after mdat, any number of mdat, free, skip, wide, uuid and
// the boxes of fragmented files (moof, sidx, mfra, ...). Until both moov and
// mdat have been seen, any box with a printable type and a plausible size
// continues the file (Media Foundation writes a uuid box first); after that
// only the types that belong at the top level do. The next ftyp always ends
// the file: the walk never assumes that another ftyp is merely a box, or
// that moov precedes mdat.

#include "carving/content_reader.hpp"
#include "carving/format_validator.hpp"
#include "formats/mp4_analysis.hpp"
#include "formats/mp4_box.hpp"
#include "formats/mp4_parser.hpp"
#include "recovery/result.hpp"
#include "structure_walk.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recovery::formats::detail {

// The ftyp box a file starts with.
struct IsoStart {
    // Set when the ftyp box is unusable: the walk is over (Truncated or
    // Broken at 0).
    std::optional<Walk> early;
    // Otherwise the box's size and major brand.
    std::uint64_t ftypSize = 0;
    mp4::FourCc brand;
};

// Reads the ftyp box at offset 0: 16 to maxFtypSize bytes, a whole number of
// brands, a 32-bit size. Fails with the reader's errors.
[[nodiscard]] Result<IsoStart> readIsoStart(carving::IContentReader& content, std::uint32_t maxFtypSize);

enum class IsoStop : std::uint8_t {
    // The data ends right after a box.
    DataEnded,
    // What follows is not a box of the file: not a box at all, a second ftyp,
    // or, once moov and mdat have been seen, a type that does not belong at
    // the top level.
    NotABox,
    // A box of size 0 ("to the end of the file"): where it ends is unknown.
    SizeUnknown,
    // The data ends inside a box, or inside what could be a box header.
    Truncated,
};

struct IsoLayout {
    IsoStop stop = IsoStop::NotABox;
    // Where the walk stopped: after the last box, or at the box (or header)
    // it stopped at.
    std::uint64_t position = 0;
    // Why it stopped (never file content).
    std::string what;
    std::optional<mp4::BoxHeader> moov;
    // A second moov box, if any.
    std::optional<std::uint64_t> secondMoov;
    std::vector<mp4::BoxHeader> mediaData;
    // The box of size 0 (SizeUnknown).
    std::optional<mp4::BoxHeader> sizeUnknown;
    // The last whole box the walk passed.
    std::optional<mp4::BoxHeader> lastBox;
    std::uint64_t boxes = 0;
    // moov came before the first mdat.
    bool moovFirst = false;
    // A moof box was seen.
    bool fragments = false;

    // moov and mdat were seen and the walk ended at the end of the data or
    // before something that is not a box of the file.
    [[nodiscard]] bool complete() const noexcept {
        return moov.has_value() && !mediaData.empty() && (stop == IsoStop::DataEnded || stop == IsoStop::NotABox);
    }
};

// Walks the top-level boxes from `start` (the end of the ftyp box). Fails
// with the reader's errors.
[[nodiscard]] Result<IsoLayout> walkIsoTopLevel(carving::IContentReader& content, std::uint64_t start);

// The walk's outcome from the layout: Truncated or Broken where the layout
// stopped short, Complete at `layout.position` with `summary` otherwise. A
// movie fragment (moof) needs the mdat after it: when the data ends right
// after one, the file is Truncated; when something else follows it, the
// file is Broken before it.
[[nodiscard]] Walk finishIsoWalk(const IsoLayout& layout, std::string summary);

// A validation verdict from a parse of the whole content, for a file that
// must be of kind `wanted`: Valid, Truncated (the data ends before the
// structure does) or Invalid (the file is of another kind, the parse found
// issues, or an audio file has no sound track). With `beyondIsInvalid`
// (M4A, as P10 pinned it), a chunk beyond the end of a file whose top-level
// boxes are complete is Invalid; otherwise (MP4) it is Truncated, since the
// media data may continue elsewhere.
[[nodiscard]] carving::ValidationResult isoVerdict(const mp4::Mp4File& file, std::uint64_t contentSize,
                                                   mp4::MediaKind wanted, bool beyondIsInvalid);

}  // namespace recovery::formats::detail
