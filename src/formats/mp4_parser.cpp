#include "formats/mp4_parser.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace recovery::formats::mp4 {

namespace {

using carving::IContentReader;
using Bytes = std::span<const std::byte>;

constexpr std::size_t kFullBox = 4;  // version and flags
// Bytes of a table read at a time.
constexpr std::size_t kTableBatch = 64 * 1024;
// Largest ftyp payload read (a few dozen bytes in practice).
constexpr std::uint64_t kMaxFtypPayload = 4096;
// Sample entries (after the box header): reserved and data_reference_index,
// then the fields of VisualSampleEntry, AudioSampleEntry, and QuickTime's
// version 2 sound description.
constexpr std::size_t kSampleEntry = 8;
constexpr std::size_t kVisualEntry = 78;
constexpr std::size_t kAudioEntry = 28;
constexpr std::size_t kAudioEntryV2 = 64;

std::string plural(std::uint64_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

std::string quoted(FourCc type) {
    return "'" + type.text() + "'";
}

bool allZero(Bytes bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
}

// The container a known box belongs in (a zero code for the top level, and
// for types the parser does not know).
struct Home {
    FourCc type;
    FourCc parent;
};
constexpr FourCc kTopLevel{};
constexpr std::array<Home, 26> kHomes = {{{box::kFtyp, kTopLevel},  {box::kMoov, kTopLevel},  {box::kMdat, kTopLevel},
                                          {box::kMvhd, box::kMoov}, {box::kTrak, box::kMoov}, {box::kMvex, box::kMoov},
                                          {box::kTkhd, box::kTrak}, {box::kMdia, box::kTrak}, {box::kMdhd, box::kMdia},
                                          {box::kHdlr, box::kMdia}, {box::kMinf, box::kMdia}, {box::kStbl, box::kMinf},
                                          {box::kStsd, box::kStbl}, {box::kStts, box::kStbl}, {box::kStsc, box::kStbl},
                                          {box::kStsz, box::kStbl}, {box::kStz2, box::kStbl}, {box::kStco, box::kStbl},
                                          {box::kCo64, box::kStbl}, {box::kMoof, kTopLevel}, {box::kTrex, box::kMvex},
                                          {box::kMfhd, box::kMoof}, {box::kTraf, box::kMoof}, {box::kTfhd, box::kTraf},
                                          {box::kTfdt, box::kTraf}, {box::kTrun, box::kTraf}}};

// Flags of tfhd (ISO/IEC 14496-12 8.8.7) and trun (8.8.8).
constexpr std::uint32_t kBaseDataOffsetPresent = 0x000001;
constexpr std::uint32_t kDescriptionIndexPresent = 0x000002;
constexpr std::uint32_t kDefaultDurationPresent = 0x000008;
constexpr std::uint32_t kDefaultSizePresent = 0x000010;
constexpr std::uint32_t kDefaultFlagsPresent = 0x000020;
constexpr std::uint32_t kDefaultBaseIsMoof = 0x020000;
constexpr std::uint32_t kDataOffsetPresent = 0x000001;
constexpr std::uint32_t kFirstSampleFlagsPresent = 0x000004;
constexpr std::uint32_t kSampleDurationPresent = 0x000100;
constexpr std::uint32_t kSampleSizePresent = 0x000200;
constexpr std::uint32_t kSampleFields = 0x000F00;  // duration, size, flags, composition offset
// Sample indices of a track (sample tables and fragments together) stay below this.
constexpr std::uint64_t kMaxTrackSamples = 0xFFFFFFFFULL;

// Whether a box of type `child` may sit in `parent`. Unknown types may be
// anywhere; QuickTime puts a data handler (hdlr) in minf.
bool belongsIn(FourCc child, FourCc parent) noexcept {
    if (child == box::kHdlr && parent == box::kMinf) {
        return true;
    }
    const auto home = std::find_if(kHomes.begin(), kHomes.end(), [&](const Home& h) { return h.type == child; });
    return home == kHomes.end() || home->parent == parent;
}

// Calls visit(entry) for `count` entries of `entrySize` bytes from `offset`,
// reading them in batches. The caller has checked that they lie in the content.
template <typename Visit>
Status forEachEntry(IContentReader& content, std::uint64_t offset, std::uint64_t count, std::size_t entrySize,
                    Visit&& visit) {
    const std::uint64_t perBatch = kTableBatch / entrySize;
    while (count > 0) {
        const std::uint64_t batch = std::min(count, perBatch);
        Result<Bytes> bytes = content.read(offset, static_cast<std::size_t>(batch * entrySize));
        if (!bytes.ok()) {
            return bytes.error();
        }
        for (std::size_t i = 0; i < batch; ++i) {
            visit(bytes->subspan(i * entrySize, entrySize));
        }
        offset += batch * entrySize;
        count -= batch;
    }
    return success();
}

// Parses one moov box into a Movie. Layout faults stop the container they
// are in; everything else is noted and the parse goes on.
class MovieParser {
public:
    MovieParser(IContentReader& content, const ParseLimits& limits, std::uint64_t boxesUsed, Movie& movie) noexcept
        : content_(content), limits_(limits), boxes_(boxesUsed), movie_(movie) {}

    Status parse(const BoxHeader& moov);
    // The movie fragments (moof) of the layout, after parse(): their runs go
    // to the movie's tracks, their issues to the movie's.
    Status parseFragments(const Layout& layout, std::vector<MovieFragment>& fragments);

private:
    void malformed(std::uint64_t offset, const std::string& path, std::string detail) {
        movie_.issues.add(IssueKind::Malformed, offset, path, std::move(detail));
    }
    void exceeded(std::uint64_t offset, const std::string& path, std::string detail) {
        movie_.issues.add(IssueKind::LimitExceeded, offset, path, std::move(detail));
    }
    bool takeBox(std::uint64_t offset, const std::string& path);
    bool takeEntries(std::uint64_t count, std::uint64_t offset, const std::string& path);
    template <typename Visit>
    Status forEachChild(const BoxHeader& parent, std::uint64_t begin, const std::string& path, Visit&& visit);
    Status badChild(const BoxHeader& parent, const BoxRead& read, const std::string& path);
    void keep(std::optional<BoxHeader>& slot, const BoxHeader& child, const std::string& path);
    Result<std::optional<Bytes>> readFixed(const BoxHeader& box, std::size_t size, const std::string& path);
    Result<std::optional<Bytes>> readVersioned(const BoxHeader& box, std::size_t size0, std::size_t size1,
                                               const std::string& path);
    Result<std::optional<Bytes>> readExact(const BoxHeader& box, std::size_t size, const std::string& path);
    Result<std::optional<std::uint64_t>> tableCount(const BoxHeader& box, std::size_t fixed, std::size_t countAt,
                                                    std::size_t entrySize, const std::string& path);

    Status parseMovieHeader(const BoxHeader& box);
    Status parseTrack(const BoxHeader& trak, std::size_t number);
    Status parseTrackHeader(const BoxHeader& box, Track& track, const std::string& path);
    Status parseMedia(const BoxHeader& mdia, Track& track, const std::string& path);
    Status parseMediaHeader(const BoxHeader& box, Track& track, const std::string& path);
    Status parseHandler(const BoxHeader& box, Track& track, const std::string& path);
    Status parseMediaInformation(const BoxHeader& minf, Track& track, const std::string& path);
    Status parseSampleTable(const BoxHeader& stbl, Track& track, const std::string& path);
    Result<bool> parseDescriptions(const BoxHeader& stsd, Track& track, const std::string& path);
    Result<bool> parseDescription(const BoxHeader& entry, Track& track, const std::string& path);
    Result<std::uint8_t> readNalLengthSize(const BoxHeader& entry, const std::string& path);
    Result<bool> parseTimes(const BoxHeader& stts, SampleTable& table, const std::string& path);
    Result<bool> parseChunkMap(const BoxHeader& stsc, SampleTable& table, const std::string& path);
    Result<bool> parseSizes(const BoxHeader& stsz, SampleTable& table, const std::string& path);
    Result<bool> parseCompactSizes(const BoxHeader& stz2, SampleTable& table, const std::string& path);
    Result<bool> parseOffsets(const BoxHeader& box, SampleTable& table, const std::string& path);
    void deriveChunks(Track& track, const std::string& path);

    Status parseMovieExtends(const BoxHeader& mvex);
    Status parseFragment(const BoxHeader& moof, std::size_t number, std::optional<std::uint32_t>& lastSequence,
                         MovieFragment& fragment);
    Status parseTrackFragment(const BoxHeader& moof, const BoxHeader& traf, const std::string& path,
                              std::uint64_t& nextBase, MovieFragment& fragment);
    // Where the next run of the track fragment starts without a data offset.
    struct RunCursor {
        std::uint64_t base = 0;
        std::uint64_t position = 0;
        std::uint32_t descriptionIndex = 0;
        std::uint32_t defaultSize = 0;
    };
    Status parseRun(const BoxHeader& trun, const std::string& path, Track& track, RunCursor& cursor);

    IContentReader& content_;
    const ParseLimits& limits_;
    // Box headers read so far (including the caller's), and table entries held.
    std::uint64_t boxes_;
    std::uint64_t entries_ = 0;
    // The box budget ran out: nothing more is read.
    bool stopped_ = false;
    Movie& movie_;
};

bool MovieParser::takeBox(std::uint64_t offset, const std::string& path) {
    if (boxes_ >= limits_.maxBoxes) {
        if (!stopped_) {
            exceeded(offset, path,
                     "more than " + std::to_string(limits_.maxBoxes) + " boxes: the rest of the movie is not parsed");
        }
        stopped_ = true;
        return false;
    }
    ++boxes_;
    return true;
}

bool MovieParser::takeEntries(std::uint64_t count, std::uint64_t offset, const std::string& path) {
    if (count > limits_.maxTableEntries - entries_) {
        exceeded(offset, path,
                 plural(count, "table entry", "table entries") + " would pass the limit of " +
                     std::to_string(limits_.maxTableEntries) + " for the movie");
        return false;
    }
    entries_ += count;
    return true;
}

// Calls visit(child) for every box in [begin, parent.end()). A child that
// does not fit ends the walk of this parent (and is an issue).
template <typename Visit>
Status MovieParser::forEachChild(const BoxHeader& parent, std::uint64_t begin, const std::string& path,
                                 Visit&& visit) {
    BoxSequence sequence(content_, begin, parent.end());
    while (!stopped_) {
        Result<std::optional<BoxRead>> next = sequence.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value()) {
            return success();
        }
        const BoxRead& read = **next;
        if (read.status != BoxStatus::Valid || read.header.sizeKind == BoxSize::ToEnd) {
            return badChild(parent, read, path);
        }
        if (!takeBox(read.header.offset, path)) {
            return success();
        }
        if (!belongsIn(read.header.type, parent.type)) {
            malformed(read.header.offset, path,
                      "box " + quoted(read.header.type) + " does not belong in " + quoted(parent.type));
            continue;
        }
        if (Status visited = visit(read.header); !visited.ok()) {
            return visited;
        }
    }
    return success();
}

Status MovieParser::badChild(const BoxHeader& parent, const BoxRead& read, const std::string& path) {
    const BoxHeader& child = read.header;
    switch (read.status) {
    case BoxStatus::HeaderCut: {
        const std::uint64_t left = parent.end() - child.offset;
        if (left < 8) {
            // QuickTime ends some lists with a 32-bit zero.
            Result<Bytes> rest = content_.read(child.offset, static_cast<std::size_t>(left));
            if (!rest.ok()) {
                return rest.error();
            }
            if (allZero(*rest)) {
                return success();
            }
        }
        malformed(child.offset, path,
                  "the last " + plural(left, "byte", "bytes") + " of " + quoted(parent.type) + " are not a whole box");
        break;
    }
    case BoxStatus::TooLong:
        malformed(child.offset, path,
                  "box " + quoted(child.type) + " of " + std::to_string(child.size) + " bytes runs past the end of " +
                      quoted(parent.type));
        break;
    case BoxStatus::BadSize:
        malformed(child.offset, path,
                  "box " + quoted(child.type) + " has size " + std::to_string(child.size) +
                      ", smaller than its header");
        break;
    case BoxStatus::Valid:
        malformed(child.offset, path,
                  "box " + quoted(child.type) + " has size 0 (to the end of the file) inside " + quoted(parent.type));
        break;
    }
    return success();
}

void MovieParser::keep(std::optional<BoxHeader>& slot, const BoxHeader& child, const std::string& path) {
    if (slot.has_value()) {
        malformed(child.offset, path, "a second " + quoted(child.type) + " box");
        return;
    }
    slot = child;
}

Result<std::optional<Bytes>> MovieParser::readFixed(const BoxHeader& box, std::size_t size, const std::string& path) {
    if (box.payloadSize() < size) {
        malformed(box.offset, path,
                  plural(box.payloadSize(), "byte", "bytes") + " of payload, too short for the " +
                      std::to_string(size) + " it needs");
        return std::optional<Bytes>{};
    }
    Result<Bytes> bytes = content_.read(box.payloadOffset(), size);
    if (!bytes.ok()) {
        return bytes.error();
    }
    return std::optional<Bytes>(*bytes);
}

// A full box of version 0 or 1 whose fields fill exactly size0 or size1
// bytes of payload (version and flags included): mvhd, tkhd, mdhd. Bytes
// after the fields are an issue (the box then ran into the next one), but
// the fields are still returned.
Result<std::optional<Bytes>> MovieParser::readVersioned(const BoxHeader& box, std::size_t size0, std::size_t size1,
                                                        const std::string& path) {
    if (box.payloadSize() < kFullBox) {
        malformed(box.offset, path, "too short for a version");
        return std::optional<Bytes>{};
    }
    Result<Bytes> head = content_.read(box.payloadOffset(), 1);
    if (!head.ok()) {
        return head.error();
    }
    const std::uint8_t version = loadU8(*head, 0);
    if (version > 1) {
        malformed(box.offset, path, "version " + std::to_string(version) + " is not defined");
        return std::optional<Bytes>{};
    }
    const std::size_t size = version == 0 ? size0 : size1;
    if (box.payloadSize() > size) {
        malformed(box.offset, path,
                  plural(box.payloadSize() - size, "byte", "bytes") + " after the fields of version " +
                      std::to_string(version));
    }
    return readFixed(box, size, path);
}

// A full box whose fields fill exactly `size` bytes of payload (version and
// flags included): trex, mfhd, tfhd. Bytes after the fields are an issue, as
// for readVersioned(), but the fields are still returned.
Result<std::optional<Bytes>> MovieParser::readExact(const BoxHeader& box, std::size_t size, const std::string& path) {
    if (box.payloadSize() > size) {
        malformed(box.offset, path, plural(box.payloadSize() - size, "byte", "bytes") + " after its fields");
    }
    return readFixed(box, size, path);
}

// The entry count of a table box, once it is checked against the bytes that
// hold the entries (after `fixed` bytes of payload) and the entry budget.
// Empty when the table is malformed or too large (an issue).
Result<std::optional<std::uint64_t>> MovieParser::tableCount(const BoxHeader& box, std::size_t fixed,
                                                             std::size_t countAt, std::size_t entrySize,
                                                             const std::string& path) {
    Result<std::optional<Bytes>> head = readFixed(box, fixed, path);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return std::optional<std::uint64_t>{};
    }
    const std::uint64_t count = loadBe32(**head, countAt);
    const std::uint64_t room = box.payloadSize() - fixed;
    if (count > room / entrySize) {
        malformed(box.offset, path,
                  plural(count, "entry", "entries") + " of " + std::to_string(entrySize) + " bytes do not fit in " +
                      plural(room, "byte", "bytes"));
        return std::optional<std::uint64_t>{};
    }
    if (!takeEntries(count, box.offset, path)) {
        return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>(count);
}

Status MovieParser::parse(const BoxHeader& moov) {
    movie_.box = moov;
    std::optional<BoxHeader> header;
    std::optional<BoxHeader> extends;
    std::vector<BoxHeader> traks;
    bool tooMany = false;
    Status visited = forEachChild(moov, moov.payloadOffset(), "moov", [&](const BoxHeader& child) -> Status {
        if (child.type == box::kMvhd) {
            keep(header, child, "moov/mvhd");
        } else if (child.type == box::kTrak) {
            if (traks.size() < limits_.maxTracks) {
                traks.push_back(child);
            } else if (!tooMany) {
                tooMany = true;
                exceeded(child.offset, "moov",
                         "more than " + std::to_string(limits_.maxTracks) + " tracks: the rest are not parsed");
            }
        } else if (child.type == box::kMvex) {
            movie_.fragmented = true;
            keep(extends, child, "moov/mvex");
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (stopped_) {
        return success();
    }
    if (!header.has_value()) {
        malformed(moov.offset, "moov", "no 'mvhd' box");
    } else if (Status parsed = parseMovieHeader(*header); !parsed.ok()) {
        return parsed;
    }
    if (traks.empty()) {
        malformed(moov.offset, "moov", "no tracks");
    }
    for (std::size_t i = 0; i < traks.size() && !stopped_; ++i) {
        if (Status parsed = parseTrack(traks[i], i + 1); !parsed.ok()) {
            return parsed;
        }
    }
    for (std::size_t i = 0; i < movie_.tracks.size(); ++i) {
        const std::uint32_t id = movie_.tracks[i].header.trackId;
        for (std::size_t j = 0; j < i && id != 0; ++j) {
            if (movie_.tracks[j].header.trackId == id) {
                malformed(movie_.tracks[i].box.offset, "moov/trak[" + std::to_string(i + 1) + "]/tkhd",
                          "track id " + std::to_string(id) + " is also track " + std::to_string(j + 1) + "'s");
                break;
            }
        }
    }
    if (extends.has_value() && !stopped_) {
        return parseMovieExtends(*extends);
    }
    return success();
}

// mvex: one trex per track, with the defaults of its movie fragments.
Status MovieParser::parseMovieExtends(const BoxHeader& mvex) {
    const std::string path = "moov/mvex/trex";
    std::vector<BoxHeader> boxes;
    bool tooMany = false;
    Status visited = forEachChild(mvex, mvex.payloadOffset(), "moov/mvex", [&](const BoxHeader& child) -> Status {
        if (child.type == box::kTrex) {
            if (boxes.size() < limits_.maxTracks) {
                boxes.push_back(child);
            } else if (!tooMany) {
                tooMany = true;
                exceeded(child.offset, path,
                         "more than " + std::to_string(limits_.maxTracks) + " 'trex' boxes: the rest are not parsed");
            }
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    for (const BoxHeader& trex : boxes) {
        Result<std::optional<Bytes>> bytes = readExact(trex, kFullBox + 20, path);
        if (!bytes.ok()) {
            return bytes.error();
        }
        if (!bytes->has_value()) {
            continue;
        }
        const Bytes fields = **bytes;
        const TrackExtends defaults{loadBe32(fields, 4), loadBe32(fields, 8), loadBe32(fields, 12),
                                    loadBe32(fields, 16), loadBe32(fields, 20)};
        const auto sameTrack = [&](const TrackExtends& other) { return other.trackId == defaults.trackId; };
        const auto track = [&](const Track& t) { return t.header.trackId == defaults.trackId; };
        if (std::any_of(movie_.trackExtends.begin(), movie_.trackExtends.end(), sameTrack)) {
            malformed(trex.offset, path, "a second 'trex' box for track " + std::to_string(defaults.trackId));
        } else if (std::none_of(movie_.tracks.begin(), movie_.tracks.end(), track)) {
            malformed(trex.offset, path, "defaults for track " + std::to_string(defaults.trackId) +
                                             ", which no track of moov has");
        } else {
            movie_.trackExtends.push_back(defaults);
        }
    }
    return success();
}

Status MovieParser::parseFragments(const Layout& layout, std::vector<MovieFragment>& fragments) {
    std::optional<std::uint32_t> lastSequence;
    std::size_t number = 0;
    for (const BoxHeader& moof : layout.boxes) {
        if (stopped_) {
            break;
        }
        if (moof.type != box::kMoof) {
            continue;
        }
        ++number;
        const std::string path = "moof[" + std::to_string(number) + "]";
        if (number == 1 && !movie_.fragmented) {
            malformed(moof.offset, path, "a movie fragment, but moov has no 'mvex' box");
        }
        // A fragment is one more entry in memory.
        if (!takeEntries(1, moof.offset, path)) {
            stopped_ = true;
            break;
        }
        MovieFragment fragment;
        if (Status parsed = parseFragment(moof, number, lastSequence, fragment); !parsed.ok()) {
            return parsed;
        }
        fragments.push_back(fragment);
    }
    return success();
}

Status MovieParser::parseFragment(const BoxHeader& moof, std::size_t number,
                                  std::optional<std::uint32_t>& lastSequence, MovieFragment& fragment) {
    const std::string path = "moof[" + std::to_string(number) + "]";
    fragment.box = moof;
    std::optional<BoxHeader> header;
    std::vector<BoxHeader> trafs;
    Status visited = forEachChild(moof, moof.payloadOffset(), path, [&](const BoxHeader& child) -> Status {
        if (child.type == box::kMfhd) {
            keep(header, child, path + "/mfhd");
        } else if (child.type == box::kTraf) {
            trafs.push_back(child);
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (stopped_) {
        return success();
    }
    if (!header.has_value()) {
        malformed(moof.offset, path, "no 'mfhd' box");
    } else {
        Result<std::optional<Bytes>> fields = readExact(*header, kFullBox + 4, path + "/mfhd");
        if (!fields.ok()) {
            return fields.error();
        }
        if (fields->has_value()) {
            fragment.sequenceNumber = loadBe32(**fields, kFullBox);
            if (lastSequence.has_value() && fragment.sequenceNumber <= *lastSequence) {
                malformed(header->offset, path + "/mfhd",
                          "sequence number " + std::to_string(fragment.sequenceNumber) + " does not follow " +
                              std::to_string(*lastSequence));
            }
            lastSequence = fragment.sequenceNumber;
        }
    }
    // Without a base data offset of its own, a track fragment's data starts
    // at the moof box (the first one), or where the previous one's ended.
    std::uint64_t nextBase = moof.offset;
    for (std::size_t i = 0; i < trafs.size() && !stopped_; ++i) {
        const std::string trafPath = path + "/traf[" + std::to_string(i + 1) + "]";
        if (Status parsed = parseTrackFragment(moof, trafs[i], trafPath, nextBase, fragment); !parsed.ok()) {
            return parsed;
        }
    }
    fragment.trackFragments = static_cast<std::uint32_t>(std::min<std::size_t>(trafs.size(), 0xFFFFFFFFU));
    return success();
}

Status MovieParser::parseTrackFragment(const BoxHeader& moof, const BoxHeader& traf, const std::string& path,
                                       std::uint64_t& nextBase, MovieFragment& fragment) {
    std::optional<BoxHeader> header;
    std::optional<BoxHeader> decodeTime;
    std::vector<BoxHeader> runs;
    Status visited = forEachChild(traf, traf.payloadOffset(), path, [&](const BoxHeader& child) -> Status {
        if (child.type == box::kTfhd) {
            keep(header, child, path + "/tfhd");
        } else if (child.type == box::kTfdt) {
            keep(decodeTime, child, path + "/tfdt");
        } else if (child.type == box::kTrun) {
            runs.push_back(child);
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (stopped_) {
        return success();
    }
    if (!header.has_value()) {
        malformed(traf.offset, path, "no 'tfhd' box");
        return success();
    }
    const std::string headerPath = path + "/tfhd";
    Result<std::optional<Bytes>> head = readFixed(*header, kFullBox + 4, headerPath);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return success();
    }
    const std::uint32_t flags = loadBe32(**head, 0) & 0xFFFFFFU;
    const std::uint32_t trackId = loadBe32(**head, kFullBox);
    const std::size_t optional =
        4 * static_cast<std::size_t>(std::popcount(flags & (kDescriptionIndexPresent | kDefaultDurationPresent |
                                                            kDefaultSizePresent | kDefaultFlagsPresent)));
    const std::size_t needed = kFullBox + 4 + ((flags & kBaseDataOffsetPresent) != 0 ? 8 : 0) + optional;
    Result<std::optional<Bytes>> read = readExact(*header, needed, headerPath);
    if (!read.ok()) {
        return read.error();
    }
    if (!read->has_value()) {
        return success();
    }
    const Bytes fields = **read;
    std::size_t at = kFullBox + 4;
    std::optional<std::uint64_t> baseDataOffset;
    std::optional<std::uint32_t> descriptionIndex;
    std::optional<std::uint32_t> defaultSize;
    if ((flags & kBaseDataOffsetPresent) != 0) {
        baseDataOffset = loadBe64(fields, at);
        at += 8;
    }
    if ((flags & kDescriptionIndexPresent) != 0) {
        descriptionIndex = loadBe32(fields, at);
        at += 4;
    }
    if ((flags & kDefaultDurationPresent) != 0) {
        at += 4;
    }
    if ((flags & kDefaultSizePresent) != 0) {
        defaultSize = loadBe32(fields, at);
    }

    const auto track = std::find_if(movie_.tracks.begin(), movie_.tracks.end(),
                                    [&](const Track& t) { return t.header.trackId == trackId; });
    if (track == movie_.tracks.end() || trackId == 0) {
        malformed(header->offset, headerPath, "track id " + std::to_string(trackId) + ", which no track of moov has");
        return success();
    }
    const auto defaults = std::find_if(movie_.trackExtends.begin(), movie_.trackExtends.end(),
                                       [&](const TrackExtends& t) { return t.trackId == trackId; });
    if (defaults == movie_.trackExtends.end()) {
        malformed(header->offset, headerPath, "track " + std::to_string(trackId) + " has no 'trex' box in moov/mvex");
        return success();
    }
    RunCursor cursor;
    cursor.base = baseDataOffset.has_value()              ? *baseDataOffset
                  : (flags & kDefaultBaseIsMoof) != 0     ? moof.offset
                                                          : nextBase;
    cursor.position = cursor.base;
    cursor.descriptionIndex = descriptionIndex.value_or(defaults->sampleDescriptionIndex);
    cursor.defaultSize = defaultSize.value_or(defaults->sampleSize);
    if (cursor.descriptionIndex == 0 || cursor.descriptionIndex > track->samples.descriptions.size()) {
        malformed(header->offset, headerPath,
                  "uses sample description " + std::to_string(cursor.descriptionIndex) + " of " +
                      std::to_string(track->samples.descriptions.size()));
        return success();
    }
    for (std::size_t k = 0; k < runs.size() && !stopped_; ++k) {
        const std::string runPath = path + "/trun[" + std::to_string(k + 1) + "]";
        if (Status parsed = parseRun(runs[k], runPath, *track, cursor); !parsed.ok()) {
            return parsed;
        }
        ++fragment.runs;
    }
    nextBase = cursor.position;
    return success();
}

Status MovieParser::parseRun(const BoxHeader& trun, const std::string& path, Track& track, RunCursor& cursor) {
    Result<std::optional<Bytes>> head = readFixed(trun, kFullBox + 4, path);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return success();
    }
    const std::uint32_t flags = loadBe32(**head, 0) & 0xFFFFFFU;
    const std::uint64_t count = loadBe32(**head, kFullBox);
    const std::size_t fixed = kFullBox + 4 + ((flags & kDataOffsetPresent) != 0 ? 4 : 0) +
                              ((flags & kFirstSampleFlagsPresent) != 0 ? 4 : 0);
    const auto entry = static_cast<std::size_t>(4 * std::popcount(flags & kSampleFields));
    Result<std::optional<Bytes>> read = readFixed(trun, fixed, path);
    if (!read.ok()) {
        return read.error();
    }
    if (!read->has_value()) {
        return success();
    }
    const std::uint64_t room = trun.payloadSize() - fixed;
    if (entry > 0 && count > room / entry) {
        malformed(trun.offset, path,
                  plural(count, "sample", "samples") + " of " + std::to_string(entry) + " bytes do not fit in " +
                      plural(room, "byte", "bytes"));
        return success();
    }
    if (count > kMaxTrackSamples - track.totalSamples()) {
        exceeded(trun.offset, path, "the track would hold 2^32 samples or more");
        return success();
    }
    if (!takeEntries(count, trun.offset, path)) {
        return success();
    }

    // Where the run starts: its data offset from the base, or after the previous run.
    std::uint64_t offset = cursor.position;
    if ((flags & kDataOffsetPresent) != 0) {
        const auto dataOffset = static_cast<std::int32_t>(loadBe32(**read, kFullBox + 4));
        if (dataOffset < 0) {
            const std::uint64_t back = static_cast<std::uint64_t>(-static_cast<std::int64_t>(dataOffset));
            if (back > cursor.base) {
                malformed(trun.offset, path,
                          "data offset " + std::to_string(dataOffset) + " points before the start of the file");
                return success();
            }
            offset = cursor.base - back;
        } else {
            const std::optional<std::uint64_t> sum = checkedAdd(cursor.base, static_cast<std::uint64_t>(dataOffset));
            if (!sum.has_value()) {
                malformed(trun.offset, path,
                          "data offset " + std::to_string(dataOffset) + " points beyond the largest offset");
                return success();
            }
            offset = *sum;
        }
    }

    std::vector<std::uint32_t>& sizes = track.fragmentSampleSizes;
    const std::size_t first = sizes.size();
    std::uint64_t total = 0;  // fewer than 2^32 samples of fewer than 2^32 bytes: no overflow
    if ((flags & kSampleSizePresent) != 0) {
        const std::size_t sizeAt = (flags & kSampleDurationPresent) != 0 ? 4 : 0;
        sizes.reserve(first + static_cast<std::size_t>(count));
        Status entries = forEachEntry(content_, trun.payloadOffset() + fixed, count, entry, [&](Bytes fields) {
            const std::uint32_t size = loadBe32(fields, sizeAt);
            sizes.push_back(size);
            total += size;
        });
        if (!entries.ok()) {
            return entries;
        }
    } else {
        sizes.insert(sizes.end(), static_cast<std::size_t>(count), cursor.defaultSize);
        total = count * cursor.defaultSize;
    }
    const std::optional<std::uint64_t> end = checkedAdd(offset, total);
    if (!end.has_value()) {
        sizes.resize(first);
        malformed(trun.offset, path,
                  "a run at offset " + std::to_string(offset) + " with " + plural(total, "byte", "bytes") +
                      " ends beyond the largest offset");
        return success();
    }
    if (count > 0) {
        track.fragmentRuns.push_back(Chunk{offset, total, static_cast<std::uint32_t>(first),
                                           static_cast<std::uint32_t>(count), cursor.descriptionIndex,
                                           ChunkPlacement::Unchecked});
    }
    cursor.position = *end;
    return success();
}

Status MovieParser::parseMovieHeader(const BoxHeader& box) {
    Result<std::optional<Bytes>> bytes = readVersioned(box, kFullBox + 96, kFullBox + 108, "moov/mvhd");
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return success();
    }
    const Bytes fields = **bytes;
    MovieHeader& header = movie_.header;
    header.version = loadU8(fields, 0);
    if (header.version == 0) {
        header.timescale = loadBe32(fields, 12);
        header.duration = loadBe32(fields, 16);
        header.nextTrackId = loadBe32(fields, 96);
    } else {
        header.timescale = loadBe32(fields, 20);
        header.duration = loadBe64(fields, 24);
        header.nextTrackId = loadBe32(fields, 108);
    }
    if (header.timescale == 0) {
        malformed(box.offset, "moov/mvhd", "a time scale of 0");
    }
    return success();
}

Status MovieParser::parseTrack(const BoxHeader& trak, std::size_t number) {
    Track track;
    track.box = trak;
    const std::string path = "moov/trak[" + std::to_string(number) + "]";
    std::optional<BoxHeader> header;
    std::optional<BoxHeader> media;
    Status visited = forEachChild(trak, trak.payloadOffset(), path, [&](const BoxHeader& child) -> Status {
        if (child.type == box::kTkhd) {
            keep(header, child, path + "/tkhd");
        } else if (child.type == box::kMdia) {
            keep(media, child, path + "/mdia");
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (!stopped_) {
        if (!header.has_value()) {
            malformed(trak.offset, path, "no 'tkhd' box");
        } else if (Status parsed = parseTrackHeader(*header, track, path + "/tkhd"); !parsed.ok()) {
            return parsed;
        }
        if (!media.has_value()) {
            malformed(trak.offset, path, "no 'mdia' box");
        } else if (Status parsed = parseMedia(*media, track, path + "/mdia"); !parsed.ok()) {
            return parsed;
        }
    }
    movie_.tracks.push_back(std::move(track));
    return success();
}

Status MovieParser::parseTrackHeader(const BoxHeader& box, Track& track, const std::string& path) {
    Result<std::optional<Bytes>> bytes = readVersioned(box, kFullBox + 80, kFullBox + 92, path);
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return success();
    }
    const Bytes fields = **bytes;
    TrackHeader& header = track.header;
    header.version = loadU8(fields, 0);
    header.flags = loadBe32(fields, 0) & 0xFFFFFFU;
    const std::size_t shift = header.version == 0 ? 0 : 12;
    header.trackId = loadBe32(fields, header.version == 0 ? 12 : 20);
    header.duration = header.version == 0 ? loadBe32(fields, 20) : loadBe64(fields, 28);
    header.width = loadBe32(fields, 76 + shift);
    header.height = loadBe32(fields, 80 + shift);
    if (header.trackId == 0) {
        malformed(box.offset, path, "track id 0");
    }
    return success();
}

Status MovieParser::parseMedia(const BoxHeader& mdia, Track& track, const std::string& path) {
    std::optional<BoxHeader> header;
    std::optional<BoxHeader> handler;
    std::optional<BoxHeader> information;
    Status visited = forEachChild(mdia, mdia.payloadOffset(), path, [&](const BoxHeader& child) -> Status {
        if (child.type == box::kMdhd) {
            keep(header, child, path + "/mdhd");
        } else if (child.type == box::kHdlr) {
            keep(handler, child, path + "/hdlr");
        } else if (child.type == box::kMinf) {
            keep(information, child, path + "/minf");
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (stopped_) {
        return success();
    }
    if (!header.has_value()) {
        malformed(mdia.offset, path, "no 'mdhd' box");
    } else if (Status parsed = parseMediaHeader(*header, track, path + "/mdhd"); !parsed.ok()) {
        return parsed;
    }
    // The handler first: it says how to read the sample descriptions.
    if (!handler.has_value()) {
        malformed(mdia.offset, path, "no 'hdlr' box");
    } else if (Status parsed = parseHandler(*handler, track, path + "/hdlr"); !parsed.ok()) {
        return parsed;
    }
    if (!information.has_value()) {
        malformed(mdia.offset, path, "no 'minf' box");
        return success();
    }
    return parseMediaInformation(*information, track, path + "/minf");
}

Status MovieParser::parseMediaHeader(const BoxHeader& box, Track& track, const std::string& path) {
    Result<std::optional<Bytes>> bytes = readVersioned(box, kFullBox + 20, kFullBox + 32, path);
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return success();
    }
    const Bytes fields = **bytes;
    MediaHeader& header = track.media;
    header.version = loadU8(fields, 0);
    if (header.version == 0) {
        header.timescale = loadBe32(fields, 12);
        header.duration = loadBe32(fields, 16);
        header.language = loadBe16(fields, 20);
    } else {
        header.timescale = loadBe32(fields, 20);
        header.duration = loadBe64(fields, 24);
        header.language = loadBe16(fields, 32);
    }
    if (header.timescale == 0) {
        malformed(box.offset, path, "a time scale of 0");
    }
    return success();
}

Status MovieParser::parseHandler(const BoxHeader& box, Track& track, const std::string& path) {
    // version and flags, pre_defined (QuickTime: component type), handler type, 12 reserved bytes, name.
    constexpr std::size_t kHandlerFields = kFullBox + 20;
    Result<std::optional<Bytes>> bytes = readFixed(box, kFullBox + 8, path);
    if (!bytes.ok()) {
        return bytes.error();
    }
    if (!bytes->has_value()) {
        return success();
    }
    track.handler = FourCc::at(**bytes, kFullBox + 4);
    if (track.handler == FourCc("vide")) {
        track.kind = TrackKind::Video;
    } else if (track.handler == FourCc("soun")) {
        track.kind = TrackKind::Audio;
    }
    if (box.payloadSize() < kHandlerFields) {
        malformed(box.offset, path,
                  plural(box.payloadSize(), "byte", "bytes") + " of payload, too short for the " +
                      std::to_string(kHandlerFields) + " it needs");
    }
    return success();
}

Status MovieParser::parseMediaInformation(const BoxHeader& minf, Track& track, const std::string& path) {
    std::optional<BoxHeader> table;
    Status visited = forEachChild(minf, minf.payloadOffset(), path, [&](const BoxHeader& child) -> Status {
        if (child.type == box::kStbl) {
            keep(table, child, path + "/stbl");
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (stopped_) {
        return success();
    }
    if (!table.has_value()) {
        malformed(minf.offset, path, "no 'stbl' box");
        return success();
    }
    return parseSampleTable(*table, track, path + "/stbl");
}

Status MovieParser::parseSampleTable(const BoxHeader& stbl, Track& track, const std::string& path) {
    std::optional<BoxHeader> descriptions;
    std::optional<BoxHeader> times;
    std::optional<BoxHeader> chunkMap;
    std::optional<BoxHeader> sizes;
    std::optional<BoxHeader> offsets;
    const auto keepOneOf = [&](std::optional<BoxHeader>& slot, const BoxHeader& child, std::string_view what) {
        if (slot.has_value()) {
            malformed(child.offset, path + "/" + child.type.text(),
                      "a second " + std::string(what) + " box (" + quoted(slot->type) + " and " +
                          quoted(child.type) + ")");
            return;
        }
        slot = child;
    };
    Status visited = forEachChild(stbl, stbl.payloadOffset(), path, [&](const BoxHeader& child) -> Status {
        const std::string childPath = path + "/" + child.type.text();
        if (child.type == box::kStsd) {
            keep(descriptions, child, childPath);
        } else if (child.type == box::kStts) {
            keep(times, child, childPath);
        } else if (child.type == box::kStsc) {
            keep(chunkMap, child, childPath);
        } else if (child.type == box::kStsz || child.type == box::kStz2) {
            keepOneOf(sizes, child, "sample size");
        } else if (child.type == box::kStco || child.type == box::kCo64) {
            keepOneOf(offsets, child, "chunk offset");
        }
        return success();
    });
    if (!visited.ok()) {
        return visited;
    }
    if (stopped_) {
        return success();
    }
    if (!descriptions.has_value()) {
        malformed(stbl.offset, path, "no 'stsd' box (sample descriptions)");
    }
    if (!times.has_value()) {
        malformed(stbl.offset, path, "no 'stts' box (decoding times)");
    }
    if (!chunkMap.has_value()) {
        malformed(stbl.offset, path, "no 'stsc' box (samples per chunk)");
    }
    if (!sizes.has_value()) {
        malformed(stbl.offset, path, "no 'stsz' or 'stz2' box (sample sizes)");
    }
    if (!offsets.has_value()) {
        malformed(stbl.offset, path, "no 'stco' or 'co64' box (chunk offsets)");
    }

    SampleTable& table = track.samples;
    bool wellFormed = descriptions && times && chunkMap && sizes && offsets;
    const auto note = [&](Result<bool> parsed) -> Status {
        if (!parsed.ok()) {
            return parsed.error();
        }
        wellFormed = wellFormed && *parsed;
        return success();
    };
    if (descriptions.has_value()) {
        if (Status s = note(parseDescriptions(*descriptions, track, path + "/stsd")); !s.ok()) {
            return s;
        }
    }
    if (times.has_value()) {
        if (Status s = note(parseTimes(*times, table, path + "/stts")); !s.ok()) {
            return s;
        }
    }
    if (chunkMap.has_value()) {
        if (Status s = note(parseChunkMap(*chunkMap, table, path + "/stsc")); !s.ok()) {
            return s;
        }
    }
    if (sizes.has_value()) {
        const std::string sizesPath = path + "/" + sizes->type.text();
        Result<bool> parsed = sizes->type == box::kStsz ? parseSizes(*sizes, table, sizesPath)
                                                          : parseCompactSizes(*sizes, table, sizesPath);
        if (Status s = note(std::move(parsed)); !s.ok()) {
            return s;
        }
    }
    if (offsets.has_value()) {
        if (Status s = note(parseOffsets(*offsets, table, path + "/" + offsets->type.text())); !s.ok()) {
            return s;
        }
    }
    if (wellFormed && !stopped_) {
        deriveChunks(track, path);
    }
    return success();
}

Result<bool> MovieParser::parseDescriptions(const BoxHeader& stsd, Track& track, const std::string& path) {
    Result<std::optional<Bytes>> head = readFixed(stsd, kFullBox + 4, path);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return false;
    }
    const std::uint32_t count = loadBe32(**head, kFullBox);
    if (count == 0) {
        malformed(stsd.offset, path, "no sample descriptions");
        return false;
    }
    if (count > limits_.maxSampleDescriptions) {
        exceeded(stsd.offset, path,
                 plural(count, "sample description", "sample descriptions") + ", more than the limit of " +
                     std::to_string(limits_.maxSampleDescriptions));
        return false;
    }
    bool wellFormed = true;
    BoxSequence entries(content_, stsd.payloadOffset() + kFullBox + 4, stsd.end());
    for (std::uint32_t found = 0; found < count;) {
        Result<std::optional<BoxRead>> next = entries.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value()) {
            malformed(stsd.offset, path,
                      "holds " + plural(found, "sample description", "sample descriptions") + ", its count says " +
                          std::to_string(count));
            return false;
        }
        const BoxRead& read = **next;
        if (read.status != BoxStatus::Valid || read.header.sizeKind == BoxSize::ToEnd) {
            const std::uint64_t before = movie_.issues.count();
            if (Status bad = badChild(stsd, read, path); !bad.ok()) {
                return bad.error();
            }
            if (movie_.issues.count() != before) {
                return false;
            }
            continue;  // a zero terminator: the count decides
        }
        if (!takeBox(read.header.offset, path)) {
            return false;
        }
        Result<bool> parsed = parseDescription(read.header, track, path);
        if (!parsed.ok()) {
            return parsed.error();
        }
        wellFormed = wellFormed && *parsed;
        ++found;
    }
    // The entries fill the box (a QuickTime zero terminator aside).
    const std::uint64_t left = stsd.end() - entries.position();
    if (left > 0) {
        bool terminator = false;
        if (left < 8) {
            Result<Bytes> rest = content_.read(entries.position(), static_cast<std::size_t>(left));
            if (!rest.ok()) {
                return rest.error();
            }
            terminator = allZero(*rest);
        }
        if (!terminator) {
            malformed(stsd.offset, path, plural(left, "byte", "bytes") + " after the sample descriptions");
            wellFormed = false;
        }
    }
    return wellFormed;
}

Result<bool> MovieParser::parseDescription(const BoxHeader& entry, Track& track, const std::string& path) {
    SampleDescription description;
    description.format = entry.type;
    description.offset = entry.offset;
    description.size = entry.size;
    const std::uint64_t payload = entry.payloadSize();
    const std::size_t fields = track.kind == TrackKind::Video   ? kVisualEntry
                               : track.kind == TrackKind::Audio ? kAudioEntryV2
                                                                : kSampleEntry;
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(payload, fields));
    const std::string what = "sample description " + quoted(entry.type);
    if (length < kSampleEntry) {
        malformed(entry.offset, path, what + " is too short for a data reference index");
        return false;
    }
    Result<Bytes> read = content_.read(entry.payloadOffset(), length);
    if (!read.ok()) {
        return read.error();
    }
    const Bytes bytes = *read;
    description.dataReferenceIndex = loadBe16(bytes, 6);
    bool wellFormed = true;
    if (track.kind == TrackKind::Video) {
        if (length < kVisualEntry) {
            malformed(entry.offset, path,
                      what + " of a video track holds " + plural(length, "byte", "bytes") + ", not " +
                          std::to_string(kVisualEntry));
            wellFormed = false;
        } else {
            description.width = loadBe16(bytes, 24);
            description.height = loadBe16(bytes, 26);
            Result<std::uint8_t> lengthSize = readNalLengthSize(entry, path);
            if (!lengthSize.ok()) {
                return lengthSize.error();
            }
            description.nalLengthSize = *lengthSize;
        }
    } else if (track.kind == TrackKind::Audio) {
        if (length < kAudioEntry) {
            malformed(entry.offset, path,
                      what + " of an audio track holds " + plural(length, "byte", "bytes") + ", not " +
                          std::to_string(kAudioEntry));
            wellFormed = false;
        } else if (loadBe16(bytes, 8) == 2) {
            // QuickTime version 2: the classic fields hold placeholders.
            if (length < kAudioEntryV2) {
                malformed(entry.offset, path, what + " of version 2 is too short for its fields");
                wellFormed = false;
            } else {
                const double rate = std::bit_cast<double>(loadBe64(bytes, 32));
                if (!std::isfinite(rate) || rate < 0 || rate >= 4294967296.0) {
                    malformed(entry.offset, path, what + " has no valid sample rate");
                    wellFormed = false;
                } else {
                    description.sampleRate = static_cast<std::uint32_t>(rate);
                }
                description.channelCount = loadBe32(bytes, 40);
                description.sampleSize =
                    static_cast<std::uint16_t>(std::min<std::uint32_t>(loadBe32(bytes, 48), 0xFFFF));
            }
        } else {
            description.channelCount = loadBe16(bytes, 16);
            description.sampleSize = loadBe16(bytes, 18);
            description.sampleRate = loadBe32(bytes, 24) >> 16;
        }
    }
    track.samples.descriptions.push_back(description);
    return wellFormed;
}

// The NAL unit length size of a video sample entry's AVC (avcC) or HEVC
// (hvcC) configuration, among the boxes after its fixed fields: 1, 2 or 4,
// or 0 when there is none (or it is damaged: that is not the parser's to
// judge, the codec configuration is not otherwise read).
Result<std::uint8_t> MovieParser::readNalLengthSize(const BoxHeader& entry, const std::string& path) {
    // configurationVersion, then the byte whose low two bits are lengthSizeMinusOne.
    constexpr std::size_t kAvcLengthByte = 4;
    constexpr std::size_t kHevcLengthByte = 21;
    BoxSequence children(content_, entry.payloadOffset() + kVisualEntry, entry.end());
    while (!stopped_) {
        Result<std::optional<BoxRead>> next = children.next();
        if (!next.ok()) {
            return next.error();
        }
        if (!next->has_value() || (*next)->status != BoxStatus::Valid || (*next)->header.sizeKind == BoxSize::ToEnd) {
            return std::uint8_t{0};
        }
        const BoxHeader& child = (*next)->header;
        if (!takeBox(child.offset, path)) {
            return std::uint8_t{0};
        }
        std::size_t at = 0;
        if (child.type == box::kAvcC) {
            at = kAvcLengthByte;
        } else if (child.type == box::kHvcC) {
            at = kHevcLengthByte;
        } else {
            continue;
        }
        if (child.payloadSize() <= at) {
            return std::uint8_t{0};
        }
        Result<Bytes> bytes = content_.read(child.payloadOffset(), at + 1);
        if (!bytes.ok()) {
            return bytes.error();
        }
        const auto size = static_cast<std::uint8_t>((loadU8(*bytes, at) & 3U) + 1);
        return loadU8(*bytes, 0) == 1 && size != 3 ? size : std::uint8_t{0};
    }
    return std::uint8_t{0};
}

Result<bool> MovieParser::parseTimes(const BoxHeader& stts, SampleTable& table, const std::string& path) {
    constexpr std::size_t kEntry = 8;
    Result<std::optional<std::uint64_t>> count = tableCount(stts, kFullBox + 4, kFullBox, kEntry, path);
    if (!count.ok()) {
        return count.error();
    }
    if (!count->has_value()) {
        return false;
    }
    table.timeToSample.reserve(static_cast<std::size_t>(**count));
    Status read = forEachEntry(content_, stts.payloadOffset() + kFullBox + 4, **count, kEntry, [&](Bytes entry) {
        table.timeToSample.push_back(TimeToSample{loadBe32(entry, 0), loadBe32(entry, 4)});
    });
    if (!read.ok()) {
        return read.error();
    }
    return true;
}

Result<bool> MovieParser::parseChunkMap(const BoxHeader& stsc, SampleTable& table, const std::string& path) {
    constexpr std::size_t kEntry = 12;
    Result<std::optional<std::uint64_t>> count = tableCount(stsc, kFullBox + 4, kFullBox, kEntry, path);
    if (!count.ok()) {
        return count.error();
    }
    if (!count->has_value()) {
        return false;
    }
    table.sampleToChunk.reserve(static_cast<std::size_t>(**count));
    Status read = forEachEntry(content_, stsc.payloadOffset() + kFullBox + 4, **count, kEntry, [&](Bytes entry) {
        table.sampleToChunk.push_back(SampleToChunk{loadBe32(entry, 0), loadBe32(entry, 4), loadBe32(entry, 8)});
    });
    if (!read.ok()) {
        return read.error();
    }
    return true;
}

Result<bool> MovieParser::parseSizes(const BoxHeader& stsz, SampleTable& table, const std::string& path) {
    Result<std::optional<Bytes>> head = readFixed(stsz, kFullBox + 8, path);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return false;
    }
    table.uniformSampleSize = loadBe32(**head, kFullBox);
    table.sampleCount = loadBe32(**head, kFullBox + 4);
    if (table.uniformSampleSize != 0) {
        return true;
    }
    constexpr std::size_t kEntry = 4;
    Result<std::optional<std::uint64_t>> count = tableCount(stsz, kFullBox + 8, kFullBox + 4, kEntry, path);
    if (!count.ok()) {
        return count.error();
    }
    if (!count->has_value()) {
        return false;
    }
    table.sampleSizes.reserve(static_cast<std::size_t>(**count));
    Status read = forEachEntry(content_, stsz.payloadOffset() + kFullBox + 8, **count, kEntry,
                               [&](Bytes entry) { table.sampleSizes.push_back(loadBe32(entry, 0)); });
    if (!read.ok()) {
        return read.error();
    }
    return true;
}

Result<bool> MovieParser::parseCompactSizes(const BoxHeader& stz2, SampleTable& table, const std::string& path) {
    Result<std::optional<Bytes>> head = readFixed(stz2, kFullBox + 8, path);
    if (!head.ok()) {
        return head.error();
    }
    if (!head->has_value()) {
        return false;
    }
    table.compactSampleSizes = true;
    const std::uint8_t fieldSize = loadU8(**head, kFullBox + 3);
    const std::uint32_t count = loadBe32(**head, kFullBox + 4);
    if (fieldSize != 4 && fieldSize != 8 && fieldSize != 16) {
        malformed(stz2.offset, path, "a field size of " + std::to_string(fieldSize) + " bits (4, 8 or 16)");
        return false;
    }
    const std::uint64_t bytesNeeded = (std::uint64_t{count} * fieldSize + 7) / 8;
    const std::uint64_t room = stz2.payloadSize() - (kFullBox + 8);
    if (bytesNeeded > room) {
        malformed(stz2.offset, path,
                  plural(count, "size", "sizes") + " of " + std::to_string(fieldSize) + " bits do not fit in " +
                      plural(room, "byte", "bytes"));
        return false;
    }
    if (!takeEntries(count, stz2.offset, path)) {
        return false;
    }
    table.sampleCount = count;
    table.sampleSizes.reserve(count);
    const std::size_t entry = fieldSize == 16 ? 2 : 1;
    Status read = forEachEntry(content_, stz2.payloadOffset() + kFullBox + 8, bytesNeeded / entry, entry,
                               [&](Bytes field) {
                                   if (fieldSize == 16) {
                                       table.sampleSizes.push_back(loadBe16(field, 0));
                                   } else if (fieldSize == 8) {
                                       table.sampleSizes.push_back(loadU8(field, 0));
                                   } else {
                                       table.sampleSizes.push_back(loadU8(field, 0) >> 4);
                                       if (table.sampleSizes.size() < count) {
                                           table.sampleSizes.push_back(loadU8(field, 0) & 0x0FU);
                                       }
                                   }
                               });
    if (!read.ok()) {
        return read.error();
    }
    return true;
}

Result<bool> MovieParser::parseOffsets(const BoxHeader& box, SampleTable& table, const std::string& path) {
    const bool wide = box.type == box::kCo64;
    const std::size_t entry = wide ? 8 : 4;
    table.wideChunkOffsets = wide;
    Result<std::optional<std::uint64_t>> count = tableCount(box, kFullBox + 4, kFullBox, entry, path);
    if (!count.ok()) {
        return count.error();
    }
    if (!count->has_value()) {
        return false;
    }
    table.chunkOffsets.reserve(static_cast<std::size_t>(**count));
    Status read = forEachEntry(content_, box.payloadOffset() + kFullBox + 4, **count, entry, [&](Bytes field) {
        table.chunkOffsets.push_back(wide ? loadBe64(field, 0) : loadBe32(field, 0));
    });
    if (!read.ok()) {
        return read.error();
    }
    return true;
}

// Checks that stts, stsc, stsz and stco agree, and if they do, fills the
// track's chunks. Every loop is bounded by the chunks and samples held.
void MovieParser::deriveChunks(Track& track, const std::string& path) {
    const SampleTable& table = track.samples;
    const std::uint64_t chunkCount = table.chunkOffsets.size();
    const std::uint64_t sampleCount = table.sampleCount;
    const std::string sizesBox = table.compactSampleSizes ? "'stz2'" : "'stsz'";
    const std::string offsetsPath = path + (table.wideChunkOffsets ? "/co64" : "/stco");
    const std::string mapPath = path + "/stsc";
    const std::uint64_t at = track.box.offset;

    std::uint64_t timed = 0;
    for (const TimeToSample& entry : table.timeToSample) {
        timed += entry.sampleCount;  // fewer than 2^32 entries of fewer than 2^32: no overflow
    }
    if (timed != sampleCount) {
        malformed(at, path + "/stts",
                  "times " + plural(timed, "sample", "samples") + ", the track has " + std::to_string(sampleCount));
        return;
    }
    const std::vector<SampleToChunk>& runs = table.sampleToChunk;
    if (runs.empty()) {
        if (chunkCount != 0 || sampleCount != 0) {
            malformed(at, mapPath,
                      "no entries for " + plural(chunkCount, "chunk", "chunks") + " and " +
                          plural(sampleCount, "sample", "samples"));
            return;
        }
        track.sampleTableValid = true;
        return;
    }
    for (std::size_t k = 0; k < runs.size(); ++k) {
        const SampleToChunk& run = runs[k];
        const std::string entry = "entry " + std::to_string(k + 1);
        if (k == 0 && run.firstChunk != 1) {
            malformed(at, mapPath, "the first entry starts at chunk " + std::to_string(run.firstChunk) + ", not 1");
            return;
        }
        if (k > 0 && run.firstChunk <= runs[k - 1].firstChunk) {
            malformed(at, mapPath,
                      entry + " starts at chunk " + std::to_string(run.firstChunk) + ", not after chunk " +
                          std::to_string(runs[k - 1].firstChunk));
            return;
        }
        if (run.firstChunk > chunkCount) {
            malformed(at, mapPath,
                      entry + " starts at chunk " + std::to_string(run.firstChunk) + " of " +
                          std::to_string(chunkCount));
            return;
        }
        if (run.samplesPerChunk == 0) {
            malformed(at, mapPath, entry + " has chunks of no samples");
            return;
        }
        if (run.sampleDescriptionIndex == 0 || run.sampleDescriptionIndex > table.descriptions.size()) {
            malformed(at, mapPath,
                      entry + " uses sample description " + std::to_string(run.sampleDescriptionIndex) + " of " +
                          std::to_string(table.descriptions.size()));
            return;
        }
    }
    if (!takeEntries(chunkCount, at, path)) {
        return;
    }
    std::vector<Chunk> chunks;
    chunks.reserve(static_cast<std::size_t>(chunkCount));
    std::uint64_t sample = 0;
    for (std::size_t k = 0; k < runs.size(); ++k) {
        const std::uint64_t last = k + 1 < runs.size() ? runs[k + 1].firstChunk : chunkCount + 1;
        const std::uint64_t perChunk = runs[k].samplesPerChunk;
        for (std::uint64_t number = runs[k].firstChunk; number < last; ++number) {
            if (perChunk > sampleCount - sample) {
                malformed(at, mapPath,
                          "chunk " + std::to_string(number) + " would hold samples beyond the " +
                              plural(sampleCount, "sample", "samples") + " of " + sizesBox);
                return;
            }
            std::uint64_t size = 0;  // fewer than 2^32 samples of fewer than 2^32 bytes: no overflow
            if (table.uniformSampleSize != 0) {
                size = perChunk * table.uniformSampleSize;
            } else {
                for (std::uint64_t i = sample; i < sample + perChunk; ++i) {
                    size += table.sampleSizes[static_cast<std::size_t>(i)];
                }
            }
            const std::uint64_t offset = table.chunkOffsets[static_cast<std::size_t>(number - 1)];
            if (!checkedAdd(offset, size).has_value()) {
                malformed(at, offsetsPath,
                          "chunk " + std::to_string(number) + " at offset " + std::to_string(offset) + " with " +
                              plural(size, "byte", "bytes") + " ends beyond the largest offset");
                return;
            }
            chunks.push_back(Chunk{offset, size, static_cast<std::uint32_t>(sample),
                                   static_cast<std::uint32_t>(perChunk), runs[k].sampleDescriptionIndex,
                                   ChunkPlacement::Unchecked});
            sample += perChunk;
        }
    }
    if (sample != sampleCount) {
        malformed(at, mapPath,
                  "the chunks hold " + plural(sample, "sample", "samples") + ", " + sizesBox + " has " +
                      std::to_string(sampleCount));
        return;
    }
    track.chunks = std::move(chunks);
    track.sampleTableValid = true;
}

// Sets the placement of each chunk and fragment run against the file's media
// data boxes and notes those outside them and those that overlap.
void placeChunks(std::uint64_t contentSize, const Layout& layout, Movie& movie, IssueList& issues) {
    struct Range {
        std::uint64_t begin;
        std::uint64_t end;
    };
    std::vector<Range> media;
    for (const BoxHeader& b : layout.boxes) {
        if (b.type == box::kMdat) {
            media.push_back(Range{b.payloadOffset(), b.end()});
        }
    }
    if (layout.cutBox.has_value() && layout.cutBox->type == box::kMdat) {
        media.push_back(Range{layout.cutBox->payloadOffset(), layout.cutBox->end()});
    }
    struct Extent {
        std::uint64_t begin;
        std::uint64_t end;
        std::uint32_t track;
        std::uint32_t number;
        bool run;
    };
    std::vector<Extent> extents;
    const auto what = [](bool run) { return run ? std::string("run") : std::string("chunk"); };
    for (std::size_t t = 0; t < movie.tracks.size(); ++t) {
        Track& track = movie.tracks[t];
        const std::string path = "moov/trak[" + std::to_string(t + 1) + "]";
        for (const bool run : {false, true}) {
            std::vector<Chunk>& pieces = run ? track.fragmentRuns : track.chunks;
            std::uint64_t outside = 0;
            const Chunk* firstOutside = nullptr;
            std::size_t firstOutsideNumber = 0;
            for (std::size_t c = 0; c < pieces.size(); ++c) {
                Chunk& chunk = pieces[c];
                if (chunk.end() > contentSize) {
                    chunk.placement = ChunkPlacement::BeyondData;
                } else {
                    // The last media data box that starts at or before the chunk.
                    const auto startsAfter = [](std::uint64_t value, const Range& r) { return value < r.begin; };
                    const auto after = std::upper_bound(media.begin(), media.end(), chunk.offset, startsAfter);
                    const bool inside = after != media.begin() && chunk.end() <= std::prev(after)->end;
                    chunk.placement = inside ? ChunkPlacement::MediaData : ChunkPlacement::OutsideMediaData;
                    if (!inside && outside++ == 0) {
                        firstOutside = &chunk;
                        firstOutsideNumber = c + 1;
                    }
                }
                if (chunk.size > 0) {
                    extents.push_back(Extent{chunk.offset, chunk.end(), static_cast<std::uint32_t>(t + 1),
                                             static_cast<std::uint32_t>(c + 1), run});
                }
            }
            if (firstOutside != nullptr) {
                issues.add(IssueKind::Malformed, track.box.offset, path,
                           plural(outside, what(run) + " lies", what(run) + "s lie") +
                               " outside the media data, the first " + what(run) + " " +
                               std::to_string(firstOutsideNumber) + " at offset " +
                               std::to_string(firstOutside->offset) + " (" +
                               plural(firstOutside->size, "byte", "bytes") + ")");
            }
        }
    }
    // Sorted by start, extents that do not overlap also end in order, so the
    // first overlap shows between neighbours.
    std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.begin < b.begin; });
    for (std::size_t i = 1; i < extents.size(); ++i) {
        const Extent& previous = extents[i - 1];
        const Extent& current = extents[i];
        if (current.begin < previous.end) {
            issues.add(IssueKind::Malformed, current.begin, "moov",
                       what(current.run) + " " + std::to_string(current.number) + " of track " +
                           std::to_string(current.track) + " overlaps " + what(previous.run) + " " +
                           std::to_string(previous.number) + " of track " + std::to_string(previous.track) +
                           " at offset " + std::to_string(current.begin));
            return;
        }
    }
}

std::string summary(const Mp4File& file) {
    const Movie& movie = *file.movie;
    std::string text = file.fileType.has_value() ? "brand " + quoted(file.fileType->majorBrand) : "no ftyp";
    text += ", " + plural(movie.tracks.size(), "track", "tracks") + " (" +
            std::to_string(movie.count(TrackKind::Video)) + " video, " +
            std::to_string(movie.count(TrackKind::Audio)) + " audio)";
    const std::vector<BoxHeader> media = file.layout.all(box::kMdat);
    if (media.empty()) {
        text += ", no media data";
    } else {
        text += std::string(", moov ") + (movie.box.offset < media.front().offset ? "before" : "after") +
                " the media data (" + plural(media.size(), "mdat box", "mdat boxes") + ")";
    }
    if (movie.fragmented) {
        text += ", fragmented (" + plural(file.fragments.size(), "movie fragment", "movie fragments") + ")";
    }
    return text;
}

}  // namespace

std::string Issue::describe() const {
    return (path.empty() ? std::string() : path + " ") + "at " + std::to_string(offset) + ": " + detail;
}

void IssueList::add(IssueKind kind, std::uint64_t offset, std::string path, std::string detail) {
    ++count_;
    if (recorded_.size() < kMaxRecorded) {
        recorded_.push_back(Issue{kind, offset, std::move(path), std::move(detail)});
    }
}

void IssueList::append(const IssueList& other) {
    for (const Issue& issue : other.recorded_) {
        if (recorded_.size() < kMaxRecorded) {
            recorded_.push_back(issue);
        }
    }
    count_ += other.count_;
}

bool IssueList::contains(IssueKind kind) const noexcept {
    return std::any_of(recorded_.begin(), recorded_.end(), [&](const Issue& issue) { return issue.kind == kind; });
}

Status validate(const ParseLimits& limits) {
    if (limits.maxBoxes == 0 || limits.maxTracks == 0 || limits.maxSampleDescriptions == 0 ||
        limits.maxTableEntries == 0) {
        return makeError(ErrorCode::InvalidInput, "MP4 parse limits must not be 0");
    }
    return success();
}

std::string MediaHeader::isoLanguage() const {
    if (language < 0x400 || (language & 0x8000U) != 0) {
        return {};
    }
    std::string code;
    for (int shift = 10; shift >= 0; shift -= 5) {
        const auto c = static_cast<char>(((language >> shift) & 0x1FU) + 0x60);
        if (c < 'a' || c > 'z') {
            return {};
        }
        code += c;
    }
    return code;
}

std::size_t Movie::count(TrackKind kind) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(tracks.begin(), tracks.end(), [&](const Track& track) { return track.kind == kind; }));
}

Result<FileType> parseFileType(carving::IContentReader& content, const BoxHeader& ftyp, IssueList& issues) {
    if (ftyp.type != box::kFtyp || ftyp.size < ftyp.headerSize ||
        !rangeWithin(ftyp.offset, ftyp.size, content.size())) {
        return makeError(ErrorCode::InvalidInput, "not an ftyp box inside the content");
    }
    FileType type;
    const std::uint64_t payload = ftyp.payloadSize();
    if (payload < 8) {
        issues.add(IssueKind::Malformed, ftyp.offset, "ftyp",
                   plural(payload, "byte", "bytes") + " of payload, too short for a brand and a version");
        return type;
    }
    if ((payload - 8) % 4 != 0) {
        issues.add(IssueKind::Malformed, ftyp.offset, "ftyp",
                   plural(payload - 8, "byte", "bytes") + " of compatible brands, not a whole number of brands");
    }
    if (payload > kMaxFtypPayload) {
        issues.add(IssueKind::LimitExceeded, ftyp.offset, "ftyp",
                   plural(payload, "byte", "bytes") + " of payload; only the first " +
                       std::to_string(kMaxFtypPayload) + " are read");
    }
    const auto length = static_cast<std::size_t>(std::min(payload, kMaxFtypPayload));
    Result<Bytes> bytes = content.read(ftyp.payloadOffset(), length);
    if (!bytes.ok()) {
        return bytes.error();
    }
    type.majorBrand = FourCc::at(*bytes, 0);
    type.minorVersion = loadBe32(*bytes, 4);
    for (std::size_t i = 8; i + 4 <= length; i += 4) {
        type.compatibleBrands.push_back(FourCc::at(*bytes, i));
    }
    return type;
}

Result<Movie> parseMovie(carving::IContentReader& content, const BoxHeader& moov, const ParseLimits& limits) {
    if (Status valid = validate(limits); !valid.ok()) {
        return valid.error();
    }
    if (moov.type != box::kMoov || moov.size < moov.headerSize ||
        !rangeWithin(moov.offset, moov.size, content.size())) {
        return makeError(ErrorCode::InvalidInput, "not a moov box inside the content");
    }
    Movie movie;
    MovieParser parser(content, limits, 0, movie);
    if (Status parsed = parser.parse(moov); !parsed.ok()) {
        return parsed.error();
    }
    return movie;
}

Result<Mp4File> parseFile(carving::IContentReader& content, const ParseLimits& limits) {
    if (Status valid = validate(limits); !valid.ok()) {
        return valid.error();
    }
    Mp4File file;
    Result<Layout> layout = scanTopLevel(content, 0, limits.maxBoxes);
    if (!layout.ok()) {
        return layout.error();
    }
    file.layout = std::move(*layout);
    const Layout& boxes = file.layout;
    IssueList& issues = file.issues;
    if (boxes.end == LayoutEnd::NotABox) {
        issues.add(IssueKind::Malformed, boxes.stoppedAt, "", boxes.detail);
    } else if (boxes.end == LayoutEnd::BoxLimit) {
        issues.add(IssueKind::LimitExceeded, boxes.stoppedAt, "", boxes.detail);
    }

    const std::vector<BoxHeader> types = boxes.all(box::kFtyp);
    if (!types.empty()) {
        if (boxes.boxes.front().type != box::kFtyp) {
            issues.add(IssueKind::Malformed, types.front().offset, "ftyp", "ftyp is not the first box");
        }
        Result<FileType> type = parseFileType(content, types.front(), issues);
        if (!type.ok()) {
            return type.error();
        }
        file.fileType = std::move(*type);
        for (std::size_t i = 1; i < types.size(); ++i) {
            issues.add(IssueKind::Malformed, types[i].offset, "ftyp",
                       "a second ftyp box (another file may start here)");
        }
    }

    const std::vector<BoxHeader> movies = boxes.all(box::kMoov);
    for (std::size_t i = 1; i < movies.size(); ++i) {
        issues.add(IssueKind::Malformed, movies[i].offset, "moov", "a second moov box");
    }
    if (!movies.empty()) {
        Movie movie;
        MovieParser parser(content, limits, boxes.boxes.size(), movie);
        if (Status parsed = parser.parse(movies.front()); !parsed.ok()) {
            return parsed.error();
        }
        if (Status parsed = parser.parseFragments(boxes, file.fragments); !parsed.ok()) {
            return parsed.error();
        }
        issues.append(movie.issues);
        placeChunks(content.size(), boxes, movie, issues);
        file.movie = std::move(movie);
    }

    if (!issues.empty()) {
        file.status = FileStatus::Invalid;
        file.detail = issues.recorded().front().describe();
        if (issues.count() > 1) {
            file.detail += " (and " + plural(issues.count() - 1, "more issue", "more issues") + ")";
        }
        return file;
    }
    if (boxes.end == LayoutEnd::Truncated) {
        file.status = FileStatus::Truncated;
        file.detail = boxes.detail;
        return file;
    }
    if (!file.movie.has_value()) {
        file.status = FileStatus::Truncated;
        file.detail = "the data ends before a moov box";
        return file;
    }
    for (std::size_t t = 0; t < file.movie->tracks.size(); ++t) {
        const Track& track = file.movie->tracks[t];
        for (const bool run : {false, true}) {
            for (const Chunk& chunk : run ? track.fragmentRuns : track.chunks) {
                if (chunk.placement == ChunkPlacement::BeyondData) {
                    file.status = FileStatus::Truncated;
                    file.detail = "the data ends before the media data of track " + std::to_string(t + 1) + " (a " +
                                  (run ? "fragment run" : "chunk") + " at offset " + std::to_string(chunk.offset) +
                                  ")";
                    return file;
                }
            }
        }
    }
    file.status = FileStatus::Valid;
    file.detail = summary(file);
    return file;
}

}  // namespace recovery::formats::mp4
