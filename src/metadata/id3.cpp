// ID3 tags (id3.org): ID3v2.2, 2.3 and 2.4 before MP3 and ADTS frames and
// inside WAV "id3 " chunks, and ID3v1 at the end of a file. Only the common
// text frames and the pictures are read. A tag is walked frame by frame
// through the reader: text frames are read up to what the text limit needs,
// and a picture is only located (its first bytes tell its format), so a tag
// with a large cover is never read whole. A tag that is unsynchronised as a
// whole (ID3v2.2 and 2.3) is read into memory and restored first; its
// pictures do not lie in the content as they are, and are not offered.

#include "extraction.hpp"

#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"

#include <array>
#include <string>

namespace recovery::metadata::detail {

namespace {

using carving::IContentReader;

// Winamp's list (ID3v1 genres 0-79 and the extensions up to 191), with
// FFmpeg's spellings; 133 under its present name.
constexpr std::array<std::string_view, 192> kGenres = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
    "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel",
    "Noise", "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock",
    "National Folk", "Swing", "Fast Fusion", "Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A cappella", "Euro-House",
    "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore Techno", "Terror", "Indie", "BritPop",
    "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop",
    "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM",
    "Eclectic", "Electro", "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient",
    "Industro-Goth", "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
    "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical",
    "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
    "Garage Rock", "Psybient",
};

constexpr std::size_t kTagHeaderSize = 10;
constexpr std::uint32_t kMaxFrames = 4096;
// Bytes of a picture frame read to find where its picture starts.
constexpr std::size_t kPictureHead = 4096;

std::optional<std::uint32_t> smallNumber(std::string_view text) {
    if (text.empty() || text.size() > 3) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

std::uint32_t syncsafe(std::span<const std::byte> bytes, std::size_t at) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 7) | (loadU8(bytes, at + i) & 0x7FU);
    }
    return value;
}

bool isSyncsafe(std::span<const std::byte> bytes, std::size_t at) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if ((loadU8(bytes, at + i) & 0x80U) != 0) {
            return false;
        }
    }
    return true;
}

bool isFrameIdChar(std::byte b) noexcept {
    const auto c = static_cast<std::uint8_t>(b);
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Undoes unsynchronisation: every 0xFF 0x00 becomes 0xFF.
std::vector<std::byte> resynchronised(std::span<const std::byte> bytes) {
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(bytes[i]);
        if (bytes[i] == std::byte{0xFF} && i + 1 < bytes.size() && bytes[i + 1] == std::byte{0}) {
            ++i;
        }
    }
    return out;
}

// What one tag's frames say, before it is merged into the caller's tags.
struct Found {
    MediaTags tags;
    std::string year;  // ID3v2.3 TYER, ID3v2.2 TYE
    std::string day;   // TDAT, TDA: DDMM
    std::string time;  // TIME, TIM: HHMM
};

class FrameWalk {
public:
    FrameWalk(Extraction& x, IContentReader& reader, std::uint8_t version, bool inContent, std::uint64_t base)
        : x_(x), reader_(reader), version_(version), inContent_(inContent), base_(base) {}

    // Walks the frames in [begin, end) of the reader.
    [[nodiscard]] Status walk(std::uint64_t begin, std::uint64_t end, Found& found);

private:
    [[nodiscard]] std::size_t headerSize() const noexcept { return version_ == 2 ? 6 : 10; }
    // Whether a frame (or the padding, or the end) can start at `at`.
    [[nodiscard]] Result<bool> plausibleNext(std::uint64_t at, std::uint64_t end);
    [[nodiscard]] Status text(std::string_view id, std::uint64_t at, std::uint64_t length, bool unsync, Found& found);
    [[nodiscard]] Status picture(std::uint64_t at, std::uint64_t length);
    [[nodiscard]] std::uint64_t contentOffset(std::uint64_t at) const noexcept { return base_ + at; }

    Extraction& x_;
    IContentReader& reader_;
    std::uint8_t version_;
    // Reader offsets are content offsets (minus base_): pictures can be offered.
    bool inContent_;
    std::uint64_t base_;
};

Result<bool> FrameWalk::plausibleNext(std::uint64_t at, std::uint64_t end) {
    if (at == end) {
        return true;
    }
    if (at > end) {
        return false;
    }
    Result<std::span<const std::byte>> next = readUpTo(reader_, at, 4);
    if (!next.ok()) {
        return next.error();
    }
    if (next->empty() || (*next)[0] == std::byte{0}) {
        return true;
    }
    const std::size_t idLength = version_ == 2 ? 3 : 4;
    if (next->size() < idLength) {
        return false;
    }
    for (std::size_t i = 0; i < idLength; ++i) {
        if (!isFrameIdChar((*next)[i])) {
            return false;
        }
    }
    return true;
}

Status FrameWalk::walk(std::uint64_t begin, std::uint64_t end, Found& found) {
    std::uint64_t pos = begin;
    for (std::uint32_t frames = 0; frames < kMaxFrames; ++frames) {
        if (end - pos < headerSize()) {
            return success();
        }
        Result<std::span<const std::byte>> read = readUpTo(reader_, pos, headerSize());
        if (!read.ok()) {
            return read.error();
        }
        const std::span<const std::byte> header = *read;
        if (header.size() < headerSize() || header[0] == std::byte{0}) {
            return success();  // padding, or the end of the data
        }
        const std::size_t idLength = version_ == 2 ? 3 : 4;
        for (std::size_t i = 0; i < idLength; ++i) {
            if (!isFrameIdChar(header[i])) {
                x_.issue(contentOffset(pos), "ID3v2 frame with an invalid id (the rest of the tag not read)");
                return success();
            }
        }
        const std::string id(reinterpret_cast<const char*>(header.data()), idLength);
        std::uint64_t size = 0;
        std::uint16_t flags = 0;
        if (version_ == 2) {
            size = (std::uint64_t{loadU8(header, 3)} << 16) | loadBe16(header, 4);
        } else {
            flags = loadBe16(header, 8);
            size = loadBe32(header, 4);
            if (version_ == 4 && isSyncsafe(header, 4)) {
                // Sizes are syncsafe in 2.4; some writers (old iTunes) wrote
                // plain ones. The plain size is taken when only it leads to
                // the next frame.
                const std::uint64_t safe = syncsafe(header, 4);
                if (safe != size) {
                    Result<bool> safeFits = plausibleNext(pos + headerSize() + safe, end);
                    if (!safeFits.ok()) {
                        return safeFits.error();
                    }
                    Result<bool> plainFits = plausibleNext(pos + headerSize() + size, end);
                    if (!plainFits.ok()) {
                        return plainFits.error();
                    }
                    if (*safeFits || !*plainFits) {
                        size = safe;
                    }
                } else {
                    size = safe;
                }
            }
        }
        const std::uint64_t body = pos + headerSize();
        bool last = false;
        if (size > end - body) {
            x_.issue(contentOffset(pos), "ID3v2 frame " + id + " runs past the end of the tag");
            size = end - body;
            last = true;
        }
        // Frame flags: what comes before the data, and what makes it unreadable.
        std::uint64_t skip = 0;
        bool unreadable = false;
        bool unsync = false;
        if (version_ == 3) {
            unreadable = (flags & 0x00C0U) != 0;  // compressed or encrypted
            skip += (flags & 0x0020U) != 0 ? 1 : 0;
        } else if (version_ == 4) {
            unreadable = (flags & 0x000CU) != 0;
            skip += (flags & 0x0040U) != 0 ? 1 : 0;
            skip += (flags & 0x0001U) != 0 ? 4 : 0;
            unsync = (flags & 0x0002U) != 0;
        }
        if (!unreadable && skip <= size) {
            const std::uint64_t data = body + skip;
            const std::uint64_t length = size - skip;
            const bool isText = id == "TIT2" || id == "TT2" || id == "TPE1" || id == "TP1" || id == "TALB" ||
                                id == "TAL" || id == "TCON" || id == "TCO" || id == "TRCK" || id == "TRK" ||
                                id == "TDRC" || id == "TYER" || id == "TYE" || id == "TDAT" || id == "TDA" ||
                                id == "TIME" || id == "TIM";
            if (isText) {
                if (Status decoded = text(id, data, length, unsync, found); !decoded.ok()) {
                    return decoded;
                }
            } else if ((id == "APIC" || id == "PIC") && inContent_ && !unsync) {
                if (Status located = picture(data, length); !located.ok()) {
                    return located;
                }
            }
        }
        if (last) {
            return success();
        }
        pos = body + size;
    }
    x_.issue(contentOffset(pos), "more than " + std::to_string(kMaxFrames) + " ID3v2 frames (the rest not read)");
    return success();
}

Status FrameWalk::text(std::string_view id, std::uint64_t at, std::uint64_t length, bool unsync, Found& found) {
    if (length < 1) {
        return success();
    }
    // Enough bytes for the longest text kept in any encoding.
    const auto want = static_cast<std::size_t>(
        std::min<std::uint64_t>(length, std::uint64_t{x_.options.maxTextLength} * 4 + 16));
    Result<std::vector<std::byte>> read = readBlock(reader_, at, want);
    if (!read.ok()) {
        return read.error();
    }
    std::vector<std::byte> bytes = unsync ? resynchronised(*read) : std::move(*read);
    if (bytes.empty()) {
        return success();
    }
    TextEncoding encoding = TextEncoding::Latin1;
    switch (static_cast<std::uint8_t>(bytes[0])) {
        case 0:
            encoding = TextEncoding::Latin1;
            break;
        case 1:
            encoding = TextEncoding::Utf16;
            break;
        case 2:
            encoding = TextEncoding::Utf16Be;
            break;
        case 3:
            encoding = TextEncoding::Utf8;
            break;
        default:
            x_.issue(contentOffset(at), "ID3v2 frame " + std::string(id) + " has an unknown text encoding");
            return success();
    }
    const std::string value =
        decodeText(std::span<const std::byte>(bytes).subspan(1), encoding, x_.options.maxTextLength);
    if (value.empty()) {
        return success();
    }
    MediaTags& tags = found.tags;
    if (id == "TIT2" || id == "TT2") {
        tags.title = value;
    } else if (id == "TPE1" || id == "TP1") {
        tags.artist = value;
    } else if (id == "TALB" || id == "TAL") {
        tags.album = value;
    } else if (id == "TCON" || id == "TCO") {
        tags.genre = genreName(value);
    } else if (id == "TRCK" || id == "TRK") {
        parseTrackNumber(value, tags);
    } else if (id == "TDRC") {
        tags.date = parseIsoDateTime(value);
        if (!tags.date.has_value()) {
            x_.issue(contentOffset(at), "ID3v2 recording time is not a date");
        }
    } else if (id == "TYER" || id == "TYE") {
        found.year = value;
    } else if (id == "TDAT" || id == "TDA") {
        found.day = value;
    } else {
        found.time = value;
    }
    return success();
}

Status FrameWalk::picture(std::uint64_t at, std::uint64_t length) {
    Result<std::span<const std::byte>> read =
        readUpTo(reader_, at, static_cast<std::size_t>(std::min<std::uint64_t>(length, kPictureHead)));
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> head = *read;
    if (head.empty()) {
        return success();
    }
    const auto encoding = static_cast<std::uint8_t>(head[0]);
    const bool wide = encoding == 1 || encoding == 2;
    std::size_t pos = 1;
    if (version_ == 2) {
        pos += 3;  // image format: "JPG", "PNG"
    } else {
        const std::size_t mime = textEnd(head.subspan(pos), false);
        if (pos + mime >= head.size()) {
            x_.issue(contentOffset(at), "ID3v2 picture frame without a picture");
            return success();
        }
        pos += mime;
    }
    if (pos >= head.size()) {
        return success();
    }
    const std::uint8_t pictureType = loadU8(head, pos);
    ++pos;
    const std::size_t description = textEnd(head.subspan(pos), wide);
    const std::size_t width = wide ? 2 : 1;
    const bool terminated = description >= width && head[pos + description - 1] == std::byte{0} &&
                            (!wide || head[pos + description - 2] == std::byte{0});
    if (!terminated && head.size() < length) {
        x_.issue(contentOffset(at), "ID3v2 picture description longer than the engine reads");
        return success();
    }
    pos += description;
    if (pos >= length) {
        x_.issue(contentOffset(at), "ID3v2 picture frame without a picture");
        return success();
    }
    Result<std::optional<PreviewSource>> preview =
        probePicture(x_, PreviewKind::CoverArt, contentOffset(at + pos), length - pos, "the ID3v2 picture");
    if (!preview.ok()) {
        return preview.error();
    }
    if (preview->has_value()) {
        (*preview)->pictureType = pictureType;
        x_.out.previews.push_back(std::move(**preview));
    }
    return success();
}

}  // namespace

std::string_view id3v1Genre(std::uint32_t index) noexcept {
    return index < kGenres.size() ? kGenres[index] : std::string_view{};
}

std::string genreName(std::string_view text) {
    if (const std::optional<std::uint32_t> number = smallNumber(text); number.has_value()) {
        const std::string_view name = id3v1Genre(*number);
        return name.empty() ? std::string(text) : std::string(name);
    }
    if (text.size() >= 3 && text[0] == '(' && text[1] != '(') {
        const std::size_t close = text.find(')');
        if (close != std::string_view::npos) {
            const std::string_view reference = text.substr(1, close - 1);
            const std::string_view refinement = text.substr(close + 1);
            // "(4)Eurodisco": the refinement names it more precisely.
            if (!refinement.empty() && refinement[0] != '(') {
                return std::string(refinement);
            }
            if (reference == "RX") {
                return "Remix";
            }
            if (reference == "CR") {
                return "Cover";
            }
            if (const std::optional<std::uint32_t> number = smallNumber(reference); number.has_value()) {
                const std::string_view name = id3v1Genre(*number);
                if (!name.empty()) {
                    return std::string(name);
                }
            }
        }
    }
    if (text.size() >= 2 && text[0] == '(' && text[1] == '(') {
        return std::string(text.substr(1));
    }
    return std::string(text);
}

Result<Id3v2Result> readId3v2(Extraction& x, std::uint64_t offset, MediaTags& tags) {
    Id3v2Result result;
    result.end = offset;
    Result<std::span<const std::byte>> read = readUpTo(x.content, offset, kTagHeaderSize);
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> header = *read;
    if (header.size() < kTagHeaderSize || !startsWith(header, "ID3")) {
        return result;
    }
    const std::uint8_t version = loadU8(header, 3);
    const std::uint8_t revision = loadU8(header, 4);
    const std::uint8_t flags = loadU8(header, 5);
    if (version < 2 || version > 4 || revision == 0xFF || !isSyncsafe(header, 6)) {
        return result;
    }
    const std::uint64_t bodySize = syncsafe(header, 6);
    const bool footer = version == 4 && (flags & 0x10U) != 0;
    const std::uint64_t bodyBegin = offset + kTagHeaderSize;
    std::uint64_t bodyEnd = bodyBegin + bodySize;
    result.found = true;
    result.end = bodyEnd + (footer ? kTagHeaderSize : 0);
    if (result.end > x.content.size()) {
        x.issue(offset, "the ID3v2 tag runs past the end of the content");
        bodyEnd = std::min(bodyEnd, x.content.size());
    }
    if (version == 2 && (flags & 0x40U) != 0) {
        x.issue(offset, "the ID3v2.2 tag is compressed: not read");
        return result;
    }
    Found found;
    const bool wholeTagUnsync = (flags & 0x80U) != 0 && version < 4;
    if (wholeTagUnsync) {
        if (bodyEnd - bodyBegin > x.options.maxTagBytes) {
            x.issue(offset, "the unsynchronised ID3v2 tag is larger than the tag limit: not read");
            return result;
        }
        Result<std::vector<std::byte>> body = readBlock(x.content, bodyBegin, bodyEnd - bodyBegin);
        if (!body.ok()) {
            return body.error();
        }
        const std::vector<std::byte> restored = resynchronised(*body);
        carving::MemoryContentReader memory(restored);
        std::uint64_t begin = 0;
        if (version == 3 && (flags & 0x40U) != 0 && restored.size() >= 4) {
            begin = std::uint64_t{4} + loadBe32(restored, 0);
        }
        FrameWalk frames(x, memory, version, false, bodyBegin);
        if (begin <= restored.size()) {
            if (Status walked = frames.walk(begin, restored.size(), found); !walked.ok()) {
                return walked.error();
            }
        }
    } else {
        std::uint64_t begin = bodyBegin;
        if ((flags & 0x40U) != 0 && bodyEnd - bodyBegin >= 4) {
            // The extended header: its size excludes itself in 2.3, and is
            // syncsafe and includes itself in 2.4.
            Result<std::span<const std::byte>> extended = readUpTo(x.content, bodyBegin, 4);
            if (!extended.ok()) {
                return extended.error();
            }
            if (extended->size() == 4) {
                begin += version == 3 ? std::uint64_t{4} + loadBe32(*extended, 0) : syncsafe(*extended, 0);
            }
        }
        FrameWalk frames(x, x.content, version, true, 0);
        if (begin <= bodyEnd) {
            if (Status walked = frames.walk(begin, bodyEnd, found); !walked.ok()) {
                return walked.error();
            }
        } else {
            x.issue(offset, "the ID3v2 extended header runs past the tag");
        }
    }
    // ID3v2.3 dates come in parts: TYER (YYYY), TDAT (DDMM), TIME (HHMM).
    if (!found.tags.date.has_value() && found.year.size() == 4) {
        std::string iso = found.year;
        if (found.day.size() == 4) {
            iso += "-" + found.day.substr(2, 2) + "-" + found.day.substr(0, 2);
            if (found.time.size() == 4) {
                iso += "T" + found.time.substr(0, 2) + ":" + found.time.substr(2, 2);
            }
        }
        found.tags.date = parseIsoDateTime(iso);
        if (!found.tags.date.has_value()) {
            found.tags.date = parseIsoDateTime(found.year);
        }
    }
    fillTags(tags, found.tags);
    return result;
}

Result<std::uint64_t> readId3v1(Extraction& x, std::uint64_t end, MediaTags& tags) {
    constexpr std::size_t kSize = 128;
    if (end < kSize || end > x.content.size()) {
        return end;
    }
    Result<std::span<const std::byte>> read = readUpTo(x.content, end - kSize, kSize);
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> tag = *read;
    if (tag.size() < kSize || !startsWith(tag, "TAG")) {
        return end;
    }
    const std::size_t limit = x.options.maxTextLength;
    MediaTags v1;
    v1.title = decodeText(tag.subspan(3, 30), TextEncoding::Utf8OrLatin1, limit);
    v1.artist = decodeText(tag.subspan(33, 30), TextEncoding::Utf8OrLatin1, limit);
    v1.album = decodeText(tag.subspan(63, 30), TextEncoding::Utf8OrLatin1, limit);
    const std::string year = decodeText(tag.subspan(93, 4), TextEncoding::Latin1, limit);
    if (year.size() == 4) {
        v1.date = parseIsoDateTime(year);
    }
    // ID3v1.1: a zero byte, then the track number, ends the comment.
    if (tag[125] == std::byte{0} && tag[126] != std::byte{0}) {
        v1.track = loadU8(tag, 126);
    }
    v1.genre = std::string(id3v1Genre(loadU8(tag, 127)));
    fillTags(tags, v1);
    return end - kSize;
}

}  // namespace recovery::metadata::detail
