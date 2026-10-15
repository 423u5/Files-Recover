#include "formats/pdf_format.hpp"

#include "recovery/checked_math.hpp"
#include "structure_walk.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::formats {

namespace {

using carving::EndDetection;
using carving::HeaderCheck;
using carving::IContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;
using detail::Depth;
using detail::Walk;
using detail::WalkStatus;

// Nesting of arrays and dictionaries in one object.
constexpr int kMaxDepth = 64;
// A run of regular characters longer than this is no token (binary data).
constexpr std::size_t kMaxRegularRun = 4096;
// Literal and hexadecimal strings longer than this are taken as garbage.
constexpr std::uint64_t kMaxString = 64 * kMiB;
// Characters kept of a name or keyword (longer ones are read whole).
constexpr std::size_t kMaxKeptText = 128;
// Items kept of an array that is kept (/Index, /W, /Filter).
constexpr std::size_t kMaxKeptItems = 4096;
// The decoded size of one cross-reference stream checked.
constexpr std::uint64_t kMaxXrefStream = 64 * kMiB;
// Whitespace allowed between a stream's data and "endstream", and the
// distance from where /Length says the data ends within which "endstream"
// is still taken (a writer that miscounted an end of line).
constexpr std::uint64_t kStreamSlack = 16;
// Bytes that may follow a JPEG image in its DCTDecode stream unchecked.
constexpr std::uint64_t kJpegSlack = 16;
// Whitespace after the last %%EOF taken as the file's padding when it runs
// to the end of the content (validation of a file whose length is known).
constexpr std::uint64_t kMaxPadding = 1 * kMiB;

constexpr std::string_view kEndstream = "endstream";

// ---------------------------------------------------------------------------
// Characters (ISO 32000-1 7.2.2)

bool isWhite(std::uint8_t c) noexcept {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

bool isDelimiter(std::uint8_t c) noexcept {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

bool isRegular(std::uint8_t c) noexcept {
    return !isWhite(c) && !isDelimiter(c);
}

bool isDigit(std::uint8_t c) noexcept {
    return c >= '0' && c <= '9';
}

bool isHexDigit(std::uint8_t c) noexcept {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Whitespace other than NUL: what separates a %%EOF from an update.
bool isLineSpace(std::uint8_t c) noexcept {
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

// A byte text does not hold: a control character other than tab, line
// feed, form feed and carriage return. Every file the engine carves holds
// some, so none lies in data without them.
bool isBinary(std::byte b) noexcept {
    const auto c = static_cast<std::uint8_t>(b);
    return (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0C && c != 0x0D) || c == 0x7F;
}

std::string count(std::uint64_t n, std::string_view one, std::string_view many) {
    return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

// ---------------------------------------------------------------------------
// Reading

// The content byte by byte through a buffer of its own, so that other reads
// of the content (the decoder's, the JPEG format's) do not disturb it. A
// read error is kept and ends the bytes.
class Cursor {
public:
    static constexpr std::size_t kBuffer = 64 * kKiB;

    explicit Cursor(IContentReader& content) noexcept : content_(content) {}

    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return content_.size(); }
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }
    void seek(std::uint64_t at) noexcept { position_ = std::min(at, content_.size()); }

    // The byte at the position; false at the end of the content (or after a read error).
    bool peek(std::uint8_t& value) {
        if (!holds(position_) && !load(position_)) {
            return false;
        }
        value = static_cast<std::uint8_t>(buffer_[static_cast<std::size_t>(position_ - start_)]);
        return true;
    }
    bool next(std::uint8_t& value) {
        if (!peek(value)) {
            return false;
        }
        ++position_;
        return true;
    }
    // Whether the bytes at the position are `text`; the position does not move.
    bool lookingAt(std::string_view text) {
        if (position_ > size() || text.size() > size() - position_) {
            return false;
        }
        if (!holds(position_) || position_ + text.size() > start_ + buffer_.size()) {
            if (!load(position_)) {
                return false;
            }
        }
        const auto at = static_cast<std::size_t>(position_ - start_);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (buffer_[at + i] != static_cast<std::byte>(text[i])) {
                return false;
            }
        }
        return true;
    }

private:
    [[nodiscard]] bool holds(std::uint64_t at) const noexcept {
        return at >= start_ && at - start_ < buffer_.size();
    }
    bool load(std::uint64_t at) {
        if (error_.has_value() || at >= size()) {
            return false;
        }
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(kBuffer, size() - at));
        Result<std::span<const std::byte>> read = content_.read(at, length);
        if (!read.ok()) {
            error_ = read.error();
            return false;
        }
        buffer_.assign(read->begin(), read->end());
        start_ = at;
        return true;
    }

    IContentReader& content_;
    std::vector<std::byte> buffer_;
    std::uint64_t start_ = 0;
    std::uint64_t position_ = 0;
    std::optional<Error> error_;
};

// ---------------------------------------------------------------------------
// Tokens (ISO 32000-1 7.2, 7.3)

enum class TokenKind : std::uint8_t {
    // No more bytes (or a read error, which the cursor keeps).
    End,
    Integer,
    Real,
    Name,
    String,
    Keyword,
    DictBegin,
    DictEnd,
    ArrayBegin,
    ArrayEnd,
    // A "%%EOF" comment.
    EofMarker,
    // Bytes that are no token (see text).
    Bad,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::uint64_t offset = 0;
    // Integer: its value (one that does not fit is Real).
    std::int64_t integer = 0;
    // Name (decoded, without the slash) and Keyword, up to kMaxKeptText
    // characters; Bad: what is wrong.
    std::string text;

    [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
    [[nodiscard]] bool isKeyword(std::string_view word) const noexcept {
        return kind == TokenKind::Keyword && text == word;
    }
};

class Lexer {
public:
    explicit Lexer(Cursor& cursor) noexcept : cursor_(cursor) {}

    // The next token after whitespace and comments.
    Token next();
    // Reads from `offset` again (a token's, to read it once more).
    void rewind(std::uint64_t offset) noexcept { cursor_.seek(offset); }
    // Where the whitespace before the last token begins (after the last
    // comment before it): a reader that starts anywhere from there reads
    // that token.
    [[nodiscard]] std::uint64_t leading() const noexcept { return leading_; }

private:
    void skipWhite();
    // At a '%': whether it starts a "%%EOF" comment.
    bool atEofMarker();
    Token name(std::uint64_t start);
    Token literalString(std::uint64_t start);
    Token hexString(std::uint64_t start);
    Token regular(std::uint64_t start);

    Cursor& cursor_;
    std::uint64_t leading_ = 0;
};

Token bad(std::uint64_t offset, std::string why) {
    Token token;
    token.kind = TokenKind::Bad;
    token.offset = offset;
    token.text = std::move(why);
    return token;
}

bool Lexer::atEofMarker() {
    if (!cursor_.lookingAt("%%EOF")) {
        return false;
    }
    const std::uint64_t at = cursor_.position();
    cursor_.seek(at + 5);
    std::uint8_t c = 0;
    const bool ends = !cursor_.peek(c) || isWhite(c) || c == '%';
    cursor_.seek(at);
    return ends;
}

void Lexer::skipWhite() {
    std::uint8_t c = 0;
    while (cursor_.peek(c)) {
        if (isWhite(c)) {
            cursor_.seek(cursor_.position() + 1);
            continue;
        }
        if (c != '%' || atEofMarker()) {
            return;
        }
        // A comment, to the end of its line. A reader that starts inside it
        // reads its text as tokens: the whitespace before the next token
        // starts at its end of line.
        while (cursor_.next(c) && c != '\r' && c != '\n') {
        }
        leading_ = c == '\r' || c == '\n' ? cursor_.position() - 1 : cursor_.position();
    }
}

Token Lexer::next() {
    leading_ = cursor_.position();
    skipWhite();
    Token token;
    token.offset = cursor_.position();
    std::uint8_t c = 0;
    if (!cursor_.peek(c)) {
        return token;  // End
    }
    switch (c) {
    case '%':
        cursor_.seek(token.offset + 5);
        token.kind = TokenKind::EofMarker;
        return token;
    case '/':
        cursor_.seek(token.offset + 1);
        return name(token.offset);
    case '(':
        cursor_.seek(token.offset + 1);
        return literalString(token.offset);
    case '<':
        if (cursor_.lookingAt("<<")) {
            cursor_.seek(token.offset + 2);
            token.kind = TokenKind::DictBegin;
            return token;
        }
        cursor_.seek(token.offset + 1);
        return hexString(token.offset);
    case '>':
        if (cursor_.lookingAt(">>")) {
            cursor_.seek(token.offset + 2);
            token.kind = TokenKind::DictEnd;
            return token;
        }
        return bad(token.offset, "a '>' that ends nothing");
    case '[':
        cursor_.seek(token.offset + 1);
        token.kind = TokenKind::ArrayBegin;
        return token;
    case ']':
        cursor_.seek(token.offset + 1);
        token.kind = TokenKind::ArrayEnd;
        return token;
    case ')':
        return bad(token.offset, "a ')' that ends no string");
    case '{':
    case '}':
        return bad(token.offset, "a brace outside a PostScript function");
    default:
        return regular(token.offset);
    }
}

Token Lexer::name(std::uint64_t start) {
    Token token;
    token.kind = TokenKind::Name;
    token.offset = start;
    std::uint8_t c = 0;
    std::size_t length = 0;
    while (cursor_.peek(c) && isRegular(c)) {
        cursor_.seek(cursor_.position() + 1);
        if (++length > kMaxRegularRun) {
            return bad(start, "a name longer than " + std::to_string(kMaxRegularRun) + " bytes");
        }
        // #xx: a byte in hexadecimal (ISO 32000-1 7.3.5).
        if (c == '#') {
            std::uint8_t high = 0;
            std::uint8_t low = 0;
            const std::uint64_t at = cursor_.position();
            if (cursor_.next(high) && cursor_.next(low) && isHexDigit(high) && isHexDigit(low)) {
                const auto digit = [](std::uint8_t h) {
                    return h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10;
                };
                c = static_cast<std::uint8_t>(digit(high) * 16 + digit(low));
            } else {
                cursor_.seek(at);
            }
        }
        if (token.text.size() < kMaxKeptText) {
            token.text += static_cast<char>(c);
        }
    }
    return token;
}

Token Lexer::literalString(std::uint64_t start) {
    Token token;
    token.kind = TokenKind::String;
    token.offset = start;
    std::uint64_t depth = 1;
    std::uint8_t c = 0;
    while (cursor_.next(c)) {
        if (cursor_.position() - start > kMaxString) {
            return bad(start, "a string longer than " + std::to_string(kMaxString) + " bytes");
        }
        if (c == '\\') {
            if (!cursor_.next(c)) {
                break;
            }
        } else if (c == '(') {
            ++depth;
        } else if (c == ')' && --depth == 0) {
            return token;
        }
    }
    token.kind = TokenKind::End;
    return token;
}

Token Lexer::hexString(std::uint64_t start) {
    Token token;
    token.kind = TokenKind::String;
    token.offset = start;
    std::uint8_t c = 0;
    while (cursor_.next(c)) {
        if (c == '>') {
            return token;
        }
        if (!isHexDigit(c) && !isWhite(c)) {
            return bad(cursor_.position() - 1, "a hexadecimal string that holds a character no hex digit is");
        }
        if (cursor_.position() - start > kMaxString) {
            return bad(start, "a string longer than " + std::to_string(kMaxString) + " bytes");
        }
    }
    token.kind = TokenKind::End;
    return token;
}

Token Lexer::regular(std::uint64_t start) {
    std::string text;
    std::size_t length = 0;
    std::uint8_t c = 0;
    while (cursor_.peek(c) && isRegular(c)) {
        cursor_.seek(cursor_.position() + 1);
        if (++length > kMaxRegularRun) {
            return bad(start, "a run of more than " + std::to_string(kMaxRegularRun) + " bytes that is no token");
        }
        if (text.size() < kMaxKeptText) {
            text += static_cast<char>(c);
        }
    }
    Token token;
    token.offset = start;
    // A number: [+-] digits [. digits] (ISO 32000-1 7.3.3).
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    std::size_t digits = 0;
    bool point = false;
    bool number = i < text.size() && length == text.size();
    for (std::size_t j = i; number && j < text.size(); ++j) {
        const auto d = static_cast<std::uint8_t>(text[j]);
        if (isDigit(d)) {
            ++digits;
        } else if (d == '.' && !point) {
            point = true;
        } else {
            number = false;
        }
    }
    if (number && digits > 0) {
        token.kind = TokenKind::Real;
        if (!point && digits <= 18) {
            std::int64_t value = 0;
            for (std::size_t j = i; j < text.size(); ++j) {
                value = value * 10 + (text[j] - '0');
            }
            token.kind = TokenKind::Integer;
            token.integer = text[0] == '-' ? -value : value;
        }
        return token;
    }
    token.kind = TokenKind::Keyword;
    token.text = std::move(text);
    return token;
}

// ---------------------------------------------------------------------------
// Values (ISO 32000-1 7.3): only the dictionary entries a walk needs are
// kept, the rest is read and dropped.

struct Value {
    enum class Kind : std::uint8_t {
        Other,
        Integer,
        Name,
        Reference,
        Array,
        Dictionary,
    };

    Kind kind = Kind::Other;
    // Integer; Reference: the object number.
    std::int64_t integer = 0;
    // Reference.
    std::int64_t generation = 0;
    // Name.
    std::string name;
    // Array (at most kMaxKeptItems; `partial` when there were more).
    std::vector<Value> items;
    bool partial = false;
    // Dictionary.
    std::vector<std::pair<std::string, Value>> entries;

    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        for (const auto& [k, v] : entries) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
    [[nodiscard]] std::optional<std::int64_t> integerAt(std::string_view key) const noexcept {
        const Value* value = find(key);
        if (value == nullptr || value->kind != Kind::Integer) {
            return std::nullopt;
        }
        return value->integer;
    }
    [[nodiscard]] bool nameAt(std::string_view key, std::string_view wanted) const noexcept {
        const Value* value = find(key);
        return value != nullptr && value->kind == Kind::Name && value->name == wanted;
    }
};

// The entries of an object's (or trailer's) dictionary that are kept.
bool keptKey(std::string_view key) noexcept {
    static constexpr std::array<std::string_view, 14> kKeys = {
        "Length", "Filter", "DecodeParms", "Type",    "W",    "Index",   "Size",
        "Prev",   "N",      "First",       "XRefStm", "Root", "Encrypt", "Linearized"};
    return std::find(kKeys.begin(), kKeys.end(), key) != kKeys.end();
}

// The filters of a stream dictionary, in order (a name that is no name: "?").
std::vector<std::string> filtersOf(const Value& dictionary) {
    std::vector<std::string> filters;
    const Value* filter = dictionary.find("Filter");
    if (filter == nullptr) {
        return filters;
    }
    if (filter->kind == Value::Kind::Name) {
        filters.push_back(filter->name);
    } else if (filter->kind == Value::Kind::Array) {
        for (const Value& item : filter->items) {
            filters.push_back(item.kind == Value::Kind::Name ? item.name : "?");
        }
    } else {
        filters.emplace_back("?");
    }
    return filters;
}

// The decode parameters of a stream's first filter.
const Value* firstDecodeParms(const Value& dictionary) noexcept {
    const Value* parms = dictionary.find("DecodeParms");
    if (parms != nullptr && parms->kind == Value::Kind::Array) {
        parms = parms->items.empty() ? nullptr : &parms->items.front();
    }
    return parms != nullptr && parms->kind == Value::Kind::Dictionary ? parms : nullptr;
}

bool isFlate(std::string_view filter) noexcept {
    return filter == "FlateDecode" || filter == "Fl";
}

bool isDct(std::string_view filter) noexcept {
    return filter == "DCTDecode" || filter == "DCT";
}

bool isAsciiHex(std::string_view filter) noexcept {
    return filter == "ASCIIHexDecode" || filter == "AHx";
}

bool isAscii85(std::string_view filter) noexcept {
    return filter == "ASCII85Decode" || filter == "A85";
}

// ---------------------------------------------------------------------------
// What a walk records

struct ObjectRecord {
    std::uint64_t offset = 0;
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
    // Where the whitespace before the object begins (Lexer::leading).
    std::uint64_t leading = 0;
};

// An in-use entry of a cross-reference table.
struct EntryRecord {
    std::uint64_t offset = 0;
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
};

enum class StreamType : std::uint8_t {
    Plain,
    CrossReference,
    ObjectStream,
};

struct StreamRecord {
    // Content offset of the stream's data, and its length.
    std::uint64_t data = 0;
    std::uint64_t length = 0;
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
    StreamType type = StreamType::Plain;
    // The first filter, and how many there are.
    std::string filter;
    std::size_t filters = 0;
    // ObjectStream: the objects it holds (/N).
    std::int64_t objects = 0;
};

// A cross-reference section: a table (with its trailer) or a stream.
struct Section {
    // The "xref" keyword, or the start of the cross-reference stream's
    // object, and where the whitespace before it begins.
    std::uint64_t offset = 0;
    std::uint64_t leading = 0;
    bool stream = false;
    std::optional<std::uint64_t> prev;
    // A table's /XRefStm (a hybrid-reference file's stream).
    std::optional<std::uint64_t> xrefStream;
    // The trailer's (or the stream's) /Root.
    std::optional<std::pair<std::int64_t, std::int64_t>> root;
    // A table: its in-use entries, [firstEntry, firstEntry + entries) of the walk's.
    std::size_t firstEntry = 0;
    std::size_t entries = 0;
    // A stream: its record, /W, /Index (empty: [0 /Size]), /Size and its predictor.
    std::size_t streamRecord = 0;
    std::vector<std::int64_t> widths;
    std::vector<std::int64_t> index;
    bool indexPartial = false;
    std::int64_t size = 0;
    std::int64_t predictor = 1;
    std::int64_t columns = 1;
    std::int64_t colors = 1;
    std::int64_t bits = 8;
};

// A stream whose /Length is a reference: what its data measured.
struct LengthCheck {
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
    // The bytes before "endstream", and the end of line among them.
    std::uint64_t raw = 0;
    std::uint64_t eol = 0;
    // Where the stream's data starts (where the walk breaks when they differ).
    std::uint64_t data = 0;

    [[nodiscard]] bool matches(std::int64_t length) const noexcept {
        return length >= 0 && (static_cast<std::uint64_t>(length) == raw ||
                               static_cast<std::uint64_t>(length) == raw - eol);
    }
};

struct IntegerObject {
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
    std::int64_t value = 0;
};

// Where a revision ended, and what had been recorded by then.
struct Mark {
    std::uint64_t end = 0;
    std::optional<detail::WalkProblem> problem;
    std::size_t objects = 0;
    std::size_t entries = 0;
    std::size_t streams = 0;
    std::size_t sections = 0;
    std::size_t lengths = 0;
    std::size_t integers = 0;
    std::uint64_t objectCount = 0;
    std::uint64_t streamCount = 0;
    std::uint64_t revisions = 0;
    std::optional<std::uint64_t> startxref;
    bool encrypted = false;
    bool overflow = false;
};

// ---------------------------------------------------------------------------
// The walk

// Walks a document from its header to the end of its last revision. See
// structure_walk.hpp for what a walk reports. Depth::Full also records what
// check() needs: every object, cross-reference entry and stream.
class PdfWalker {
public:
    PdfWalker(IContentReader& content, Depth depth, const PdfLimits& limits) noexcept
        : content_(content), cursor_(content), lexer_(cursor_), depth_(depth), limits_(limits) {}

    Result<Walk> run();

    // Depth::Full, after a complete walk: the cross-reference sections, the
    // chain, the catalog and the indirect lengths, as problems of the walk;
    // and every stream's data, counted as unchecked or noted as a problem.
    Status check(const IDeflateDecoder& decoder, const JpegFormat& jpeg);

    [[nodiscard]] const Walk& walk() const noexcept { return walk_; }
    // After a complete walk: whitespace from the end of the structure to the
    // end of the content (zeros a writer padded the file with, a blank line)
    // is the file's.
    Status takePadding();
    [[nodiscard]] std::uint64_t unchecked() const noexcept { return unchecked_; }
    [[nodiscard]] const std::string& uncheckedReason() const noexcept { return uncheckedReason_; }

private:
    // The outcome of reading one element: the walk goes on, or it ended
    // (walk_ holds how), or a read failed (cursor_ holds the error).
    enum class Step : std::uint8_t { Next, Ended, Failed };

    Step object(const Token& first);
    Step stream(std::uint64_t objectStart, std::uint32_t number, std::uint16_t generation, const Value& dictionary);
    Step table(const Token& keyword);
    Step startxref();
    Step revisionEnd(const Token& marker);
    // Whether an update (an object, or a cross-reference table) starts at
    // `at`, after line spaces (the cursor is then at it), or whether the
    // data ends before that is known. `after` is where the line spaces end.
    enum class Continuation : std::uint8_t { Yes, No, DataEnds };
    Continuation continues(std::uint64_t at, std::uint64_t& after);
    // Whether the newest startxref's chain reaches every section.
    [[nodiscard]] bool chainReachesAll() const;
    [[nodiscard]] std::optional<std::size_t> sectionAt(std::uint64_t offset) const noexcept;
    void recordCrossReferenceStream(std::uint64_t objectStart, const Value& dictionary);
    void recordTrailer(Section& section, const Value& dictionary);

    // Parses the value `first` starts; `out` (when not null) keeps it.
    bool value(const Token& first, int depth, Value* out);
    bool dictionary(int depth, Value* out);
    bool array(int depth, Value* out);
    // A value could not be read: `failure_` holds why.
    Step failedValue();
    Step ended(WalkStatus status, std::uint64_t at, std::string detail);
    // The data ended (or a read failed).
    Step dataEnded(std::uint64_t at, std::string detail);
    void problem(std::uint64_t at, std::string detail) { walk_.noteProblem(at, std::move(detail)); }
    [[nodiscard]] bool full() const noexcept { return depth_ == Depth::Full; }
    // Whether one more object, entry or stream may be recorded.
    bool room(std::size_t recorded);
    [[nodiscard]] Mark mark(std::uint64_t end) const;
    void restore(const Mark& to);
    [[nodiscard]] std::string summary() const;

    // check()'s parts.
    Status checkStreams(const IDeflateDecoder& decoder, const JpegFormat& jpeg);
    Status checkStream(const StreamRecord& stream, const IDeflateDecoder& decoder, const JpegFormat& jpeg);
    Result<bool> isText(std::uint64_t from, std::uint64_t length);
    Result<bool> asciiSyntax(const StreamRecord& stream, bool hex);
    Status checkJpeg(const StreamRecord& stream, const JpegFormat& jpeg);
    void checkChain();
    void checkEntries();
    Status checkCrossReferenceStreams(const IDeflateDecoder& decoder);
    void checkCatalog();
    void checkLengths();
    [[nodiscard]] const ObjectRecord* objectAt(std::uint64_t offset) const noexcept;
    void skip(std::string reason);

    IContentReader& content_;
    Cursor cursor_;
    Lexer lexer_;
    Depth depth_;
    const PdfLimits& limits_;
    Walk walk_;
    bool finished_ = false;
    struct Failure {
        WalkStatus status = WalkStatus::Broken;
        std::uint64_t offset = 0;
        std::string detail;
    };
    std::optional<Failure> failure_;

    // Both depths: the sections, the newest startxref, the last revision
    // end, a reference /Length that the next object may resolve.
    std::vector<Section> sections_;
    std::optional<std::uint64_t> startxref_;
    std::optional<Mark> accepted_;
    std::optional<LengthCheck> pendingLength_;
    // Where the whitespace before the object being read begins.
    std::uint64_t objectLeading_ = 0;
    std::uint64_t objectCount_ = 0;
    std::uint64_t streamCount_ = 0;
    std::uint64_t revisions_ = 0;
    bool encrypted_ = false;
    bool linearized_ = false;

    // Depth::Full: what check() checks.
    std::vector<ObjectRecord> objects_;
    std::vector<EntryRecord> entries_;
    std::vector<StreamRecord> streams_;
    std::vector<LengthCheck> lengths_;
    std::vector<IntegerObject> integers_;
    // More objects, entries or streams than the limit: the rest is not recorded.
    bool overflow_ = false;

    std::uint64_t unpacked_ = 0;
    std::uint64_t unchecked_ = 0;
    std::string uncheckedReason_;
};

bool PdfWalker::room(std::size_t recorded) {
    if (recorded < limits_.maxObjects) {
        return true;
    }
    overflow_ = true;
    return false;
}

PdfWalker::Step PdfWalker::ended(WalkStatus status, std::uint64_t at, std::string detail) {
    walk_.finish(status, at, std::move(detail));
    finished_ = true;
    return Step::Ended;
}

PdfWalker::Step PdfWalker::dataEnded(std::uint64_t at, std::string detail) {
    if (cursor_.error().has_value()) {
        return Step::Failed;
    }
    return ended(WalkStatus::Truncated, at, std::move(detail));
}

PdfWalker::Step PdfWalker::failedValue() {
    if (cursor_.error().has_value()) {
        return Step::Failed;
    }
    const Failure failure = failure_.value_or(Failure{});
    failure_.reset();
    return ended(failure.status, failure.offset, failure.detail);
}

Result<Walk> PdfWalker::run() {
    // The header: "%PDF-" and a version (ISO 32000-1 7.5.2), a comment line.
    if (!cursor_.lookingAt("%PDF-")) {
        if (cursor_.error().has_value()) {
            return *cursor_.error();
        }
        return walk_.finish(WalkStatus::Broken, 0, "no PDF header");
    }
    cursor_.seek(5);
    std::uint8_t c = 0;
    std::size_t major = 0;
    while (cursor_.peek(c) && isDigit(c) && major < 2) {
        cursor_.seek(cursor_.position() + 1);
        ++major;
    }
    const bool point = cursor_.next(c) && c == '.';
    std::size_t minor = 0;
    while (point && cursor_.peek(c) && isDigit(c) && minor < 2) {
        cursor_.seek(cursor_.position() + 1);
        ++minor;
    }
    if (cursor_.error().has_value()) {
        return *cursor_.error();
    }
    if (major == 0 || !point || minor == 0) {
        return walk_.finish(WalkStatus::Broken, 0, "a PDF header without a version");
    }
    cursor_.seek(0);

    while (!finished_) {
        const Token token = lexer_.next();
        Step step = Step::Next;
        switch (token.kind) {
        case TokenKind::End:
            step = dataEnded(token.offset, "the data ends before the document's %%EOF");
            break;
        case TokenKind::Integer:
            step = object(token);
            break;
        case TokenKind::Keyword:
            if (token.text == "xref") {
                step = table(token);
            } else if (token.text == "startxref") {
                step = startxref();
            } else {
                step = ended(WalkStatus::Broken, token.offset,
                             "a keyword ('" + token.text + "') where an object or a cross-reference table belongs");
            }
            break;
        case TokenKind::EofMarker:
            step = revisionEnd(token);
            break;
        case TokenKind::Bad:
            step = ended(WalkStatus::Broken, token.offset, token.text);
            break;
        default:
            step = ended(WalkStatus::Broken, token.offset,
                         "no object, cross-reference table or %%EOF after " + count(objectCount_, "object", "objects"));
            break;
        }
        if (step == Step::Failed) {
            return *cursor_.error();
        }
    }
    return walk_;
}

PdfWalker::Step PdfWalker::object(const Token& first) {
    const std::uint64_t start = first.offset;
    objectLeading_ = lexer_.leading();
    const Token generation = lexer_.next();
    const Token keyword = lexer_.next();
    if (!generation.is(TokenKind::Integer) || !keyword.isKeyword("obj")) {
        if (generation.is(TokenKind::End) || keyword.is(TokenKind::End)) {
            return dataEnded(start, "the data ends inside an object header");
        }
        return ended(WalkStatus::Broken, start,
                     "a number that starts no object header after " + count(objectCount_, "object", "objects"));
    }
    if (first.integer < 0 || first.integer > 0x7FFFFFFF || generation.integer < 0 || generation.integer > 0xFFFF) {
        return ended(WalkStatus::Broken, start, "an object number or generation out of range");
    }
    const auto number = static_cast<std::uint32_t>(first.integer);
    const auto gen = static_cast<std::uint16_t>(generation.integer);
    const std::string which = "object " + std::to_string(number);
    ++objectCount_;
    if (full() && room(objects_.size())) {
        objects_.push_back(ObjectRecord{start, number, gen, objectLeading_});
    }

    const Token head = lexer_.next();
    Value parsed;
    if (!value(head, 0, &parsed)) {
        return failedValue();
    }
    if (objectCount_ == 1 && parsed.kind == Value::Kind::Dictionary && parsed.find("Linearized") != nullptr) {
        linearized_ = true;
    }
    // The previous stream's reference /Length is usually the next object.
    if (pendingLength_.has_value()) {
        const LengthCheck pending = *pendingLength_;
        pendingLength_.reset();
        if (pending.number == number && pending.generation == gen && parsed.kind == Value::Kind::Integer) {
            if (!pending.matches(parsed.integer)) {
                return ended(WalkStatus::Broken, pending.data,
                             "a stream's data is " + std::to_string(pending.raw - pending.eol) +
                                 " bytes long, but its /Length (" + which + ") says " +
                                 std::to_string(parsed.integer));
            }
        } else if (full() && room(lengths_.size())) {
            lengths_.push_back(pending);
        }
    }
    if (full() && parsed.kind == Value::Kind::Integer && room(integers_.size())) {
        integers_.push_back(IntegerObject{number, gen, parsed.integer});
    }

    Token after = lexer_.next();
    if (after.isKeyword("stream")) {
        if (parsed.kind != Value::Kind::Dictionary) {
            return ended(WalkStatus::Broken, after.offset, "the stream of " + which + " has no dictionary");
        }
        const Step streamed = stream(start, number, gen, parsed);
        if (streamed != Step::Next) {
            return streamed;
        }
        after = lexer_.next();
    } else if (parsed.kind == Value::Kind::Dictionary && parsed.nameAt("Type", "XRef")) {
        problem(start, "the cross-reference stream " + which + " has no stream");
    }
    if (after.isKeyword("endobj")) {
        return Step::Next;
    }
    switch (after.kind) {
    case TokenKind::End:
        return dataEnded(start, "the data ends inside " + which);
    case TokenKind::Integer:
    case TokenKind::EofMarker:
        // What comes next starts: the object lacks only its endobj.
        problem(start, which + " has no endobj");
        lexer_.rewind(after.offset);
        return Step::Next;
    default:
        if (after.isKeyword("xref") || after.isKeyword("startxref")) {
            problem(start, which + " has no endobj");
            lexer_.rewind(after.offset);
            return Step::Next;
        }
        return ended(WalkStatus::Broken, after.offset, "no endobj after " + which);
    }
}

PdfWalker::Step PdfWalker::stream(std::uint64_t objectStart, std::uint32_t number, std::uint16_t generation,
                                  const Value& dictionary) {
    const std::string which = "object " + std::to_string(number);
    // The keyword's end of line (CR LF or LF; a lone CR is taken too).
    std::uint8_t c = 0;
    if (cursor_.peek(c) && c == '\r') {
        cursor_.seek(cursor_.position() + 1);
        if (cursor_.peek(c) && c == '\n') {
            cursor_.seek(cursor_.position() + 1);
        }
    } else if (cursor_.peek(c) && c == '\n') {
        cursor_.seek(cursor_.position() + 1);
    } else if (!cursor_.error().has_value() && cursor_.position() < cursor_.size()) {
        problem(cursor_.position(), "no end of line after the stream keyword of " + which);
    }
    if (cursor_.error().has_value()) {
        return Step::Failed;
    }
    const std::uint64_t data = cursor_.position();
    const Value* lengthValue = dictionary.find("Length");
    std::uint64_t length = 0;

    if (lengthValue != nullptr && lengthValue->kind == Value::Kind::Integer && lengthValue->integer >= 0) {
        const auto recorded = static_cast<std::uint64_t>(lengthValue->integer);
        if (recorded > cursor_.size() - data) {
            // Broken, not Truncated: a damaged /Length would make the
            // document reach the end of the source.
            return ended(WalkStatus::Broken, objectStart,
                         "the /Length of " + which + "'s stream (" + std::to_string(recorded) +
                             " bytes) reaches beyond the data");
        }
        cursor_.seek(data + recorded);
        std::uint64_t spaces = 0;
        while (spaces < kStreamSlack && cursor_.peek(c) && isWhite(c)) {
            cursor_.seek(cursor_.position() + 1);
            ++spaces;
        }
        if (cursor_.lookingAt(kEndstream)) {
            length = recorded;
            cursor_.seek(cursor_.position() + kEndstream.size());
        } else {
            if (cursor_.error().has_value()) {
                return Step::Failed;
            }
            // A writer that miscounted: "endstream" a few bytes off.
            const std::uint64_t from = data + recorded - std::min(recorded, kStreamSlack);
            const std::uint64_t to = std::min(cursor_.size(), data + recorded + kStreamSlack + kEndstream.size());
            const auto pattern = std::as_bytes(std::span(kEndstream.data(), kEndstream.size()));
            Result<std::optional<std::uint64_t>> found = carving::findPattern(content_, pattern, from, to);
            if (!found.ok()) {
                return dataEnded(objectStart, found.error().message);
            }
            if (!found->has_value()) {
                return ended(WalkStatus::Broken, data,
                             "the data of " + which + "'s stream does not end where its /Length (" +
                                 std::to_string(recorded) + ") says");
            }
            length = **found - data;
            problem(data, "the /Length of " + which + "'s stream (" + std::to_string(recorded) +
                              ") is not the length of its data (" + std::to_string(length) + ")");
            cursor_.seek(**found + kEndstream.size());
        }
    } else {
        // A reference (or no /Length): the data ends at "endstream", and the
        // length object, when it comes, must say so.
        const std::uint64_t to = data + std::min(cursor_.size() - data, limits_.maxSearchedStream);
        const auto pattern = std::as_bytes(std::span(kEndstream.data(), kEndstream.size()));
        Result<std::optional<std::uint64_t>> found = carving::findPattern(content_, pattern, data, to);
        if (!found.ok()) {
            return dataEnded(objectStart, found.error().message);
        }
        if (!found->has_value()) {
            return ended(WalkStatus::Broken, objectStart,
                         "no endstream ends the data of " + which + "'s stream within " +
                             std::to_string(to - data) + " bytes");
        }
        const std::uint64_t raw = **found - data;
        std::uint64_t eol = 0;
        if (raw >= 1) {
            cursor_.seek(**found - 1);
            if (cursor_.peek(c) && c == '\n') {
                eol = 1;
                if (raw >= 2) {
                    cursor_.seek(**found - 2);
                    if (cursor_.peek(c) && c == '\r') {
                        eol = 2;
                    }
                }
            } else if (cursor_.peek(c) && c == '\r') {
                eol = 1;
            }
        }
        if (cursor_.error().has_value()) {
            return Step::Failed;
        }
        length = raw - eol;
        if (lengthValue != nullptr && lengthValue->kind == Value::Kind::Reference &&
            lengthValue->integer >= 0 && lengthValue->integer <= 0x7FFFFFFF && lengthValue->generation >= 0 &&
            lengthValue->generation <= 0xFFFF) {
            pendingLength_ = LengthCheck{static_cast<std::uint32_t>(lengthValue->integer),
                                         static_cast<std::uint16_t>(lengthValue->generation), raw, eol, data};
        } else {
            problem(objectStart, which + "'s stream has no /Length");
        }
        cursor_.seek(**found + kEndstream.size());
    }

    ++streamCount_;
    StreamType type = StreamType::Plain;
    if (dictionary.nameAt("Type", "XRef")) {
        type = StreamType::CrossReference;
    } else if (dictionary.nameAt("Type", "ObjStm")) {
        type = StreamType::ObjectStream;
    }
    if (full() && room(streams_.size())) {
        const std::vector<std::string> filters = filtersOf(dictionary);
        StreamRecord record;
        record.data = data;
        record.length = length;
        record.number = number;
        record.generation = generation;
        record.type = type;
        record.filter = filters.empty() ? std::string() : filters.front();
        record.filters = filters.size();
        record.objects = dictionary.integerAt("N").value_or(0);
        streams_.push_back(std::move(record));
    }
    if (type == StreamType::CrossReference) {
        if (sections_.size() >= limits_.maxSections) {
            return ended(WalkStatus::Broken, objectStart,
                         "more than " +
                             count(limits_.maxSections, "cross-reference section", "cross-reference sections"));
        }
        recordCrossReferenceStream(objectStart, dictionary);
    }
    return Step::Next;
}

void PdfWalker::recordTrailer(Section& section, const Value& dictionary) {
    const std::optional<std::int64_t> prev = dictionary.integerAt("Prev");
    if (prev.has_value() && *prev >= 0) {
        section.prev = static_cast<std::uint64_t>(*prev);
    }
    const Value* root = dictionary.find("Root");
    if (root != nullptr && root->kind == Value::Kind::Reference) {
        section.root = std::make_pair(root->integer, root->generation);
    }
    if (dictionary.find("Encrypt") != nullptr) {
        encrypted_ = true;
    }
}

void PdfWalker::recordCrossReferenceStream(std::uint64_t objectStart, const Value& dictionary) {
    Section section;
    section.offset = objectStart;
    section.leading = objectLeading_;
    section.stream = true;
    recordTrailer(section, dictionary);
    section.streamRecord = full() && !streams_.empty() ? streams_.size() - 1 : 0;
    const bool recorded = full() && !streams_.empty() && streams_.back().data > objectStart;
    if (const Value* widths = dictionary.find("W"); widths != nullptr && widths->kind == Value::Kind::Array) {
        for (const Value& item : widths->items) {
            section.widths.push_back(item.kind == Value::Kind::Integer ? item.integer : -1);
        }
    }
    if (const Value* index = dictionary.find("Index"); index != nullptr && index->kind == Value::Kind::Array) {
        for (const Value& item : index->items) {
            section.index.push_back(item.kind == Value::Kind::Integer ? item.integer : -1);
        }
        section.indexPartial = index->partial;
    }
    section.size = dictionary.integerAt("Size").value_or(0);
    if (const Value* parms = firstDecodeParms(dictionary); parms != nullptr) {
        section.predictor = parms->integerAt("Predictor").value_or(1);
        section.columns = parms->integerAt("Columns").value_or(1);
        section.colors = parms->integerAt("Colors").value_or(1);
        section.bits = parms->integerAt("BitsPerComponent").value_or(8);
    }
    if (!recorded) {
        section.streamRecord = static_cast<std::size_t>(-1);
    }
    sections_.push_back(std::move(section));
}

PdfWalker::Step PdfWalker::table(const Token& keyword) {
    if (sections_.size() >= limits_.maxSections) {
        return ended(WalkStatus::Broken, keyword.offset,
                     "more than " +
                         count(limits_.maxSections, "cross-reference section", "cross-reference sections"));
    }
    Section section;
    section.offset = keyword.offset;
    section.leading = lexer_.leading();
    section.firstEntry = entries_.size();
    std::uint8_t c = 0;
    for (;;) {
        const Token first = lexer_.next();
        if (first.isKeyword("trailer")) {
            break;
        }
        if (first.is(TokenKind::End)) {
            return dataEnded(keyword.offset, "the data ends inside a cross-reference table");
        }
        const Token entries = lexer_.next();
        if (!first.is(TokenKind::Integer) || !entries.is(TokenKind::Integer) || first.integer < 0 ||
            entries.integer < 0) {
            if (entries.is(TokenKind::End)) {
                return dataEnded(keyword.offset, "the data ends inside a cross-reference table");
            }
            return ended(WalkStatus::Broken, first.offset, "a cross-reference subsection without its first number "
                                                           "and count");
        }
        // Each entry: 10 digits, a space, 5 digits, a space, 'n' or 'f',
        // and an end of line (ISO 32000-1 7.5.4). The digits' count is not
        // held to.
        for (std::int64_t i = 0; i < entries.integer; ++i) {
            while (cursor_.peek(c) && isWhite(c)) {
                cursor_.seek(cursor_.position() + 1);
            }
            const std::uint64_t at = cursor_.position();
            const auto digits = [&](std::size_t most, std::uint64_t& out) {
                std::size_t n = 0;
                out = 0;
                while (n < most && cursor_.peek(c) && isDigit(c)) {
                    out = out * 10 + (c - '0');
                    cursor_.seek(cursor_.position() + 1);
                    ++n;
                }
                return n > 0;
            };
            const auto spaces = [&]() {
                std::size_t n = 0;
                while (n < 2 && cursor_.peek(c) && c == ' ') {
                    cursor_.seek(cursor_.position() + 1);
                    ++n;
                }
                return n > 0;
            };
            std::uint64_t offset = 0;
            std::uint64_t generation = 0;
            std::uint8_t type = 0;
            if (!digits(10, offset) || !spaces() || !digits(5, generation) || !spaces() || !cursor_.next(type) ||
                (type != 'n' && type != 'f')) {
                if (cursor_.error().has_value()) {
                    return Step::Failed;
                }
                if (cursor_.position() >= cursor_.size()) {
                    return dataEnded(keyword.offset, "the data ends inside a cross-reference table");
                }
                return ended(WalkStatus::Broken, at, "a cross-reference entry that is not one");
            }
            const std::uint64_t objectNumber = static_cast<std::uint64_t>(first.integer) + static_cast<std::uint64_t>(i);
            if (type == 'n' && full() && objectNumber <= 0x7FFFFFFF && generation <= 0xFFFF &&
                room(entries_.size())) {
                entries_.push_back(EntryRecord{offset, static_cast<std::uint32_t>(objectNumber),
                                               static_cast<std::uint16_t>(generation)});
            }
        }
    }
    const Token open = lexer_.next();
    Value trailer;
    if (!open.is(TokenKind::DictBegin)) {
        if (open.is(TokenKind::End)) {
            return dataEnded(keyword.offset, "the data ends before the trailer's dictionary");
        }
        return ended(WalkStatus::Broken, open.offset, "a trailer without a dictionary");
    }
    if (!value(open, 0, &trailer)) {
        return failedValue();
    }
    recordTrailer(section, trailer);
    if (const std::optional<std::int64_t> stream = trailer.integerAt("XRefStm"); stream.has_value() && *stream >= 0) {
        section.xrefStream = static_cast<std::uint64_t>(*stream);
    }
    section.entries = entries_.size() - section.firstEntry;
    sections_.push_back(std::move(section));
    return Step::Next;
}

PdfWalker::Step PdfWalker::startxref() {
    const Token offset = lexer_.next();
    if (offset.is(TokenKind::End)) {
        return dataEnded(offset.offset, "the data ends after startxref");
    }
    if (!offset.is(TokenKind::Integer) || offset.integer < 0) {
        return ended(WalkStatus::Broken, offset.offset, "startxref without an offset");
    }
    startxref_ = static_cast<std::uint64_t>(offset.integer);
    return Step::Next;
}

// As objectAt(): the section a reader finds at `offset`. The sections are in
// the order walked, so their offsets (and the whitespace before each) rise.
std::optional<std::size_t> PdfWalker::sectionAt(std::uint64_t offset) const noexcept {
    const auto found = std::lower_bound(sections_.begin(), sections_.end(), offset,
                                        [](const Section& s, std::uint64_t at) { return s.offset < at; });
    if (found == sections_.end() || found->leading > offset) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - sections_.begin());
}

bool PdfWalker::chainReachesAll() const {
    if (!startxref_.has_value() || sections_.empty()) {
        return false;
    }
    std::vector<bool> reached(sections_.size(), false);
    std::vector<std::uint64_t> pending{*startxref_};
    while (!pending.empty()) {
        const std::uint64_t offset = pending.back();
        pending.pop_back();
        const std::optional<std::size_t> at = sectionAt(offset);
        if (!at.has_value() || reached[*at]) {
            continue;
        }
        reached[*at] = true;
        const Section& section = sections_[*at];
        if (section.prev.has_value()) {
            pending.push_back(*section.prev);
        }
        if (section.xrefStream.has_value()) {
            pending.push_back(*section.xrefStream);
        }
    }
    return std::all_of(reached.begin(), reached.end(), [](bool r) { return r; });
}

Mark PdfWalker::mark(std::uint64_t end) const {
    Mark m;
    m.end = end;
    m.problem = walk_.problem;
    m.objects = objects_.size();
    m.entries = entries_.size();
    m.streams = streams_.size();
    m.sections = sections_.size();
    m.lengths = lengths_.size();
    m.integers = integers_.size();
    m.objectCount = objectCount_;
    m.streamCount = streamCount_;
    m.revisions = revisions_;
    m.startxref = startxref_;
    m.encrypted = encrypted_;
    m.overflow = overflow_;
    return m;
}

void PdfWalker::restore(const Mark& to) {
    walk_.problem = to.problem;
    objects_.resize(to.objects);
    entries_.resize(to.entries);
    streams_.resize(to.streams);
    sections_.resize(to.sections);
    lengths_.resize(to.lengths);
    integers_.resize(to.integers);
    objectCount_ = to.objectCount;
    streamCount_ = to.streamCount;
    revisions_ = to.revisions;
    startxref_ = to.startxref;
    encrypted_ = to.encrypted;
    overflow_ = to.overflow;
    pendingLength_.reset();
}

PdfWalker::Step PdfWalker::revisionEnd(const Token& marker) {
    // "%%EOF" and its end of line.
    cursor_.seek(marker.offset + 5);
    std::uint8_t c = 0;
    if (cursor_.peek(c) && c == '\r') {
        cursor_.seek(cursor_.position() + 1);
        if (cursor_.peek(c) && c == '\n') {
            cursor_.seek(cursor_.position() + 1);
        }
    } else if (cursor_.peek(c) && c == '\n') {
        cursor_.seek(cursor_.position() + 1);
    }
    if (cursor_.error().has_value()) {
        return Step::Failed;
    }
    const std::uint64_t end = cursor_.position();
    if (pendingLength_.has_value()) {
        if (full() && room(lengths_.size())) {
            lengths_.push_back(*pendingLength_);
        }
        pendingLength_.reset();
    }
    if (accepted_.has_value() && !chainReachesAll()) {
        // What followed the last revision is not an update of it (an older
        // file's objects, left after this one): the document ended there.
        const Mark last = *accepted_;
        restore(last);
        return ended(WalkStatus::Complete, last.end, summary());
    }
    if (!startxref_.has_value()) {
        problem(marker.offset, "no startxref before %%EOF");
    }
    if (revisions_ >= limits_.maxSections) {
        return ended(WalkStatus::Broken, marker.offset,
                     "more than " + count(limits_.maxSections, "revision", "revisions"));
    }
    ++revisions_;
    accepted_ = mark(end);
    std::uint64_t after = end;
    const Continuation next = continues(end, after);
    if (cursor_.error().has_value()) {
        return Step::Failed;
    }
    if (next == Continuation::Yes) {
        return Step::Next;
    }
    if (next == Continuation::DataEnds) {
        return ended(WalkStatus::Truncated, after, "the data ends inside what may be an update of the document");
    }
    // Line spaces after the last %%EOF line (a second end of line) are the file's.
    return ended(WalkStatus::Complete, after, summary());
}

PdfWalker::Continuation PdfWalker::continues(std::uint64_t at, std::uint64_t& after) {
    cursor_.seek(at);
    std::uint8_t c = 0;
    std::size_t spaces = 0;
    while (spaces < 64 && cursor_.peek(c) && isLineSpace(c)) {
        cursor_.seek(cursor_.position() + 1);
        ++spaces;
    }
    const std::uint64_t start = cursor_.position();
    after = start;
    // An update's first bytes: "xref" and a delimiter, or digits, spaces,
    // digits, spaces and "obj". The data may end inside them.
    const auto endsInside = [&](std::string_view word) {
        const std::uint64_t left = cursor_.size() - cursor_.position();
        return left < word.size() && (left == 0 || cursor_.lookingAt(word.substr(0, static_cast<std::size_t>(left))));
    };
    const auto run = [&](auto test, std::size_t most) {
        std::size_t n = 0;
        while (n < most && cursor_.peek(c) && test(c)) {
            cursor_.seek(cursor_.position() + 1);
            ++n;
        }
        return n;
    };
    Continuation verdict = Continuation::No;
    if (cursor_.lookingAt("xref")) {
        cursor_.seek(start + 4);
        if (!cursor_.peek(c)) {
            verdict = Continuation::DataEnds;
        } else if (isWhite(c) || isDelimiter(c)) {
            verdict = Continuation::Yes;
        }
    } else if (start < cursor_.size() && endsInside("xref")) {
        verdict = Continuation::DataEnds;
    } else if (run(isDigit, 10) > 0) {
        // The number, its spaces, the generation, its spaces, "obj".
        bool header = true;
        for (int part = 0; header && part < 3; ++part) {
            if (cursor_.position() >= cursor_.size()) {
                verdict = Continuation::DataEnds;
                header = false;
            } else {
                header = (part == 1 ? run(isDigit, 5) : run(isWhite, 4)) > 0;
            }
        }
        if (header) {
            if (cursor_.lookingAt("obj")) {
                verdict = Continuation::Yes;
            } else if (endsInside("obj")) {
                verdict = Continuation::DataEnds;
            }
        } else if (cursor_.position() >= cursor_.size()) {
            verdict = Continuation::DataEnds;
        }
    }
    cursor_.seek(start);
    return verdict;
}

std::string PdfWalker::summary() const {
    std::string text = count(objectCount_, "object", "objects") + ", " + count(streamCount_, "stream", "streams") +
                       ", " + count(revisions_, "revision", "revisions");
    if (linearized_) {
        text += ", linearized";
    }
    if (encrypted_) {
        text += ", encrypted";
    }
    return text;
}

bool PdfWalker::value(const Token& first, int depth, Value* out) {
    const auto fail = [&](WalkStatus status, std::uint64_t at, std::string detail) {
        if (!failure_.has_value()) {
            failure_ = Failure{status, at, std::move(detail)};
        }
        return false;
    };
    if (depth > kMaxDepth) {
        return fail(WalkStatus::Broken, first.offset,
                    "arrays and dictionaries nested deeper than " + std::to_string(kMaxDepth));
    }
    switch (first.kind) {
    case TokenKind::Integer: {
        // "n g R": a reference.
        const std::uint64_t after = cursor_.position();
        const Token generation = lexer_.next();
        if (generation.is(TokenKind::Integer)) {
            const Token r = lexer_.next();
            if (r.isKeyword("R")) {
                if (out != nullptr) {
                    out->kind = Value::Kind::Reference;
                    out->integer = first.integer;
                    out->generation = generation.integer;
                }
                return true;
            }
        }
        lexer_.rewind(after);
        if (out != nullptr) {
            out->kind = Value::Kind::Integer;
            out->integer = first.integer;
        }
        return true;
    }
    case TokenKind::Real:
    case TokenKind::String:
        return true;
    case TokenKind::Name:
        if (out != nullptr) {
            out->kind = Value::Kind::Name;
            out->name = first.text;
        }
        return true;
    case TokenKind::Keyword:
        if (first.text == "true" || first.text == "false" || first.text == "null") {
            return true;
        }
        return fail(WalkStatus::Broken, first.offset, "a keyword ('" + first.text + "') where a value belongs");
    case TokenKind::DictBegin:
        return dictionary(depth, out);
    case TokenKind::ArrayBegin:
        return array(depth, out);
    case TokenKind::End:
        return fail(WalkStatus::Truncated, first.offset, "the data ends inside an object");
    case TokenKind::Bad:
        return fail(WalkStatus::Broken, first.offset, first.text);
    case TokenKind::DictEnd:
    case TokenKind::ArrayEnd:
    case TokenKind::EofMarker:
        break;
    }
    return fail(WalkStatus::Broken, first.offset, "no value where one belongs");
}

bool PdfWalker::dictionary(int depth, Value* out) {
    if (out != nullptr) {
        out->kind = Value::Kind::Dictionary;
    }
    for (;;) {
        const Token key = lexer_.next();
        if (key.is(TokenKind::DictEnd)) {
            return true;
        }
        if (!key.is(TokenKind::Name)) {
            if (key.is(TokenKind::End)) {
                return value(key, depth + 1, nullptr);
            }
            if (!failure_.has_value()) {
                failure_ = Failure{WalkStatus::Broken, key.offset,
                                   key.is(TokenKind::Bad) ? key.text : "a dictionary key that is not a name"};
            }
            return false;
        }
        const Token item = lexer_.next();
        // Only the top level's entries a walk needs are kept (and what they hold).
        Value* kept = nullptr;
        if (out != nullptr && (depth > 0 || keptKey(key.text))) {
            out->entries.emplace_back(key.text, Value{});
            kept = &out->entries.back().second;
        }
        if (!value(item, depth + 1, kept)) {
            return false;
        }
    }
}

bool PdfWalker::array(int depth, Value* out) {
    if (out != nullptr) {
        out->kind = Value::Kind::Array;
    }
    for (;;) {
        const Token item = lexer_.next();
        if (item.is(TokenKind::ArrayEnd)) {
            return true;
        }
        Value* kept = nullptr;
        if (out != nullptr) {
            if (out->items.size() < kMaxKeptItems) {
                out->items.emplace_back();
                kept = &out->items.back();
            } else {
                out->partial = true;
            }
        }
        if (!value(item, depth + 1, kept)) {
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Checks after the walk (Depth::Full)

// The content [offset, offset + size) of another reader, as content of its own.
class WindowReader final : public IContentReader {
public:
    WindowReader(IContentReader& base, std::uint64_t offset, std::uint64_t size) noexcept
        : base_(base), offset_(offset), size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override {
        if (!rangeWithin<std::uint64_t>(offset, length, size_)) {
            return makeError(ErrorCode::InvalidInput, "a read outside the stream's data");
        }
        return base_.read(offset_ + offset, length);
    }

private:
    IContentReader& base_;
    std::uint64_t offset_;
    std::uint64_t size_;
};

Status PdfWalker::takePadding() {
    const std::uint64_t end = walk_.end;
    const std::uint64_t size = content_.size();
    if (end >= size || size - end > kMaxPadding) {
        return success();
    }
    for (std::uint64_t done = end; done < size;) {
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(IContentReader::kMaxReadLength, size - done));
        Result<std::span<const std::byte>> piece = content_.read(done, take);
        if (!piece.ok()) {
            return piece.error();
        }
        if (!std::all_of(piece->begin(), piece->end(),
                         [](std::byte b) { return isWhite(static_cast<std::uint8_t>(b)); })) {
            return success();
        }
        done += take;
    }
    walk_.end = size;
    walk_.detail += ", " + std::to_string(size - end) + " bytes of padding after %%EOF";
    return success();
}

void PdfWalker::skip(std::string reason) {
    if (unchecked_++ == 0) {
        uncheckedReason_ = std::move(reason);
    }
}

// The object a reader finds at `offset`: the one that starts there, or after
// the whitespace that runs from there to it. Readers skip it, and a common
// writer (Windows' Microsoft Print to PDF) records every offset at the end of
// line before its object.
const ObjectRecord* PdfWalker::objectAt(std::uint64_t offset) const noexcept {
    const auto found = std::lower_bound(objects_.begin(), objects_.end(), offset,
                                        [](const ObjectRecord& o, std::uint64_t at) { return o.offset < at; });
    return found != objects_.end() && found->leading <= offset ? &*found : nullptr;
}

Status PdfWalker::check(const IDeflateDecoder& decoder, const JpegFormat& jpeg) {
    if (overflow_) {
        skip("more than " + std::to_string(limits_.maxObjects) + " objects, entries or streams to check");
    }
    checkChain();
    checkEntries();
    if (Status checked = checkCrossReferenceStreams(decoder); !checked.ok()) {
        return checked;
    }
    checkCatalog();
    checkLengths();
    return checkStreams(decoder, jpeg);
}

void PdfWalker::checkChain() {
    if (!startxref_.has_value()) {
        return;  // noted where %%EOF came without it
    }
    if (!sectionAt(*startxref_).has_value()) {
        problem(walk_.end, "startxref (" + std::to_string(*startxref_) + ") points at no cross-reference section");
        return;
    }
    for (const Section& section : sections_) {
        if (section.prev.has_value() && !sectionAt(*section.prev).has_value()) {
            problem(section.offset, "the /Prev of the cross-reference section at " + std::to_string(section.offset) +
                                        " points at no section");
            return;
        }
        if (section.xrefStream.has_value()) {
            const std::optional<std::size_t> stream = sectionAt(*section.xrefStream);
            if (!stream.has_value() || !sections_[*stream].stream) {
                problem(section.offset, "the /XRefStm of the trailer at " + std::to_string(section.offset) +
                                            " points at no cross-reference stream");
                return;
            }
        }
    }
}

void PdfWalker::checkEntries() {
    if (overflow_) {
        return;
    }
    for (const Section& section : sections_) {
        for (std::size_t i = section.firstEntry; !section.stream && i < section.firstEntry + section.entries; ++i) {
            const EntryRecord& entry = entries_[i];
            const ObjectRecord* object = objectAt(entry.offset);
            if (object == nullptr || object->number != entry.number || object->generation != entry.generation) {
                problem(section.offset, "the cross-reference entry of object " + std::to_string(entry.number) +
                                            " points at offset " + std::to_string(entry.offset) +
                                            (object == nullptr ? ", where no object starts"
                                                               : ", where object " + std::to_string(object->number) +
                                                                     " starts"));
                return;
            }
        }
    }
}

// Undoes a PNG (10 to 15) or TIFF (2) predictor in place (ISO 32000-1
// 7.4.4.4); false when the parameters or the data do not fit one.
bool unpredict(std::vector<std::byte>& data, const Section& section, std::size_t& rowLength) {
    if (section.colors < 1 || section.colors > 32 || section.columns < 1 || section.columns > (1 << 20) ||
        (section.bits != 1 && section.bits != 2 && section.bits != 4 && section.bits != 8 && section.bits != 16)) {
        return false;
    }
    const auto bitsPerPixel = static_cast<std::size_t>(section.colors * section.bits);
    const std::size_t bytesPerPixel = std::max<std::size_t>(1, (bitsPerPixel + 7) / 8);
    rowLength = (static_cast<std::size_t>(section.columns) * bitsPerPixel + 7) / 8;
    if (section.predictor == 1) {
        return true;
    }
    if (section.predictor == 2) {
        if (section.bits != 8 || data.size() % rowLength != 0) {
            return false;
        }
        for (std::size_t row = 0; row < data.size(); row += rowLength) {
            for (std::size_t i = bytesPerPixel; i < rowLength; ++i) {
                data[row + i] = static_cast<std::byte>(static_cast<std::uint8_t>(data[row + i]) +
                                                       static_cast<std::uint8_t>(data[row + i - bytesPerPixel]));
            }
        }
        return true;
    }
    if (section.predictor < 10 || section.predictor > 15) {
        return false;
    }
    const std::size_t stride = rowLength + 1;
    if (data.size() % stride != 0) {
        return false;
    }
    std::vector<std::byte> out;
    out.reserve(data.size() / stride * rowLength);
    std::vector<std::uint8_t> previous(rowLength, 0);
    std::vector<std::uint8_t> row(rowLength, 0);
    for (std::size_t at = 0; at < data.size(); at += stride) {
        const auto type = static_cast<std::uint8_t>(data[at]);
        for (std::size_t i = 0; i < rowLength; ++i) {
            const auto x = static_cast<std::uint8_t>(data[at + 1 + i]);
            const std::uint8_t a = i >= bytesPerPixel ? row[i - bytesPerPixel] : 0;
            const std::uint8_t b = previous[i];
            const std::uint8_t c = i >= bytesPerPixel ? previous[i - bytesPerPixel] : 0;
            std::uint8_t value = 0;
            switch (type) {
            case 0:
                value = x;
                break;
            case 1:
                value = static_cast<std::uint8_t>(x + a);
                break;
            case 2:
                value = static_cast<std::uint8_t>(x + b);
                break;
            case 3:
                value = static_cast<std::uint8_t>(x + (a + b) / 2);
                break;
            case 4: {
                const int p = a + b - c;
                const int pa = p > a ? p - a : a - p;
                const int pb = p > b ? p - b : b - p;
                const int pc = p > c ? p - c : c - p;
                const std::uint8_t predicted = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                value = static_cast<std::uint8_t>(x + predicted);
                break;
            }
            default:
                return false;
            }
            row[i] = value;
        }
        for (const std::uint8_t value : row) {
            out.push_back(static_cast<std::byte>(value));
        }
        previous = row;
    }
    data = std::move(out);
    return true;
}

Status PdfWalker::checkCrossReferenceStreams(const IDeflateDecoder& decoder) {
    if (overflow_) {
        return success();
    }
    for (const Section& section : sections_) {
        if (!section.stream) {
            continue;
        }
        const std::string where = "the cross-reference stream at " + std::to_string(section.offset);
        if (section.streamRecord >= streams_.size()) {
            skip(where + " was not recorded");
            continue;
        }
        const StreamRecord& stream = streams_[section.streamRecord];
        if (section.widths.size() != 3 ||
            std::any_of(section.widths.begin(), section.widths.end(), [](std::int64_t w) { return w < 0 || w > 8; }) ||
            section.indexPartial || section.index.size() % 2 != 0) {
            problem(section.offset, where + " has a /W or /Index no writer makes");
            continue;
        }
        if (stream.filters > 1 || (stream.filters == 1 && !isFlate(stream.filter))) {
            skip(where + " uses " + (stream.filters > 1 ? "several filters" : stream.filter));
            continue;
        }
        std::vector<std::byte> data;
        if (stream.filters == 0) {
            if (stream.length > kMaxXrefStream) {
                skip(where + " is too long to check");
                continue;
            }
            for (std::uint64_t done = 0; done < stream.length;) {
                const auto take = static_cast<std::size_t>(
                    std::min<std::uint64_t>(IContentReader::kMaxReadLength, stream.length - done));
                Result<std::span<const std::byte>> piece = content_.read(stream.data + done, take);
                if (!piece.ok()) {
                    return piece.error();
                }
                data.insert(data.end(), piece->begin(), piece->end());
                done += take;
            }
        } else {
            // Counted against the document's budget, as every stream's data is.
            const std::uint64_t budget = limits_.maxUnpacked > unpacked_ ? limits_.maxUnpacked - unpacked_ : 0;
            const std::uint64_t most = std::min(kMaxXrefStream, budget);
            Result<IDeflateDecoder::Outcome> decoded =
                decoder.decodeZlib(content_, stream.data, stream.data + stream.length, most, &data);
            if (!decoded.ok()) {
                return decoded.error();
            }
            unpacked_ += decoded->output;
            if (decoded->kind == IDeflateDecoder::Outcome::Kind::TooLarge) {
                skip(most < kMaxXrefStream
                         ? where + " unpacks past the " + std::to_string(limits_.maxUnpacked) +
                               " bytes the document's streams may unpack to"
                         : where + " unpacks to more than " + std::to_string(kMaxXrefStream) + " bytes");
                continue;
            }
            if (decoded->kind != IDeflateDecoder::Outcome::Kind::Done) {
                // checkStreams() says what is wrong with the data.
                continue;
            }
        }
        std::size_t rowLength = 0;
        const std::size_t entryLength =
            static_cast<std::size_t>(section.widths[0] + section.widths[1] + section.widths[2]);
        Section parms = section;
        if (section.predictor == 1) {
            parms.columns = static_cast<std::int64_t>(entryLength);
        }
        if (entryLength == 0 || !unpredict(data, parms, rowLength) ||
            (section.predictor != 1 && rowLength != entryLength)) {
            problem(section.offset, where + " has a predictor its entries do not fit");
            continue;
        }
        std::vector<std::int64_t> index = section.index;
        if (index.empty()) {
            index = {0, section.size};
        }
        // The entries /Index lists, counted so that no sum of counts wraps
        // around: the loop below reads them all from the data.
        const std::uint64_t available = data.size() / entryLength;
        std::uint64_t needed = 0;
        bool tooMany = false;
        for (std::size_t i = 1; i < index.size() && !tooMany; i += 2) {
            const auto entries = static_cast<std::uint64_t>(std::max<std::int64_t>(0, index[i]));
            tooMany = entries > available - needed;
            needed += tooMany ? 0 : entries;
        }
        if (tooMany) {
            problem(section.offset, where + " holds fewer entries than its /Index lists");
            continue;
        }
        const auto field = [&](std::size_t at, std::int64_t width) {
            std::uint64_t value = 0;
            for (std::int64_t i = 0; i < width; ++i) {
                value = (value << 8) | static_cast<std::uint8_t>(data[at + static_cast<std::size_t>(i)]);
            }
            return value;
        };
        std::size_t at = 0;
        bool wrong = false;
        for (std::size_t i = 0; i + 1 < index.size() && !wrong; i += 2) {
            if (index[i] < 0 || index[i + 1] < 0) {
                problem(section.offset, where + " has a negative /Index");
                break;
            }
            for (std::int64_t k = 0; k < index[i + 1] && !wrong; ++k, at += entryLength) {
                const std::uint64_t type = section.widths[0] == 0 ? 1 : field(at, section.widths[0]);
                const std::uint64_t second = field(at + static_cast<std::size_t>(section.widths[0]), section.widths[1]);
                const std::uint64_t third =
                    field(at + static_cast<std::size_t>(section.widths[0] + section.widths[1]), section.widths[2]);
                const std::uint64_t number = static_cast<std::uint64_t>(index[i]) + static_cast<std::uint64_t>(k);
                if (type == 1) {
                    const ObjectRecord* object = objectAt(second);
                    if (object == nullptr || object->number != number || object->generation != third) {
                        problem(section.offset, where + "'s entry of object " + std::to_string(number) +
                                                    " points at offset " + std::to_string(second) +
                                                    (object == nullptr ? ", where no object starts"
                                                                       : ", where object " +
                                                                             std::to_string(object->number) +
                                                                             " starts"));
                        wrong = true;
                    }
                } else if (type == 2) {
                    const auto holder = std::find_if(streams_.begin(), streams_.end(), [&](const StreamRecord& s) {
                        return s.type == StreamType::ObjectStream && s.number == second && s.generation == 0;
                    });
                    if (holder == streams_.end() || third >= static_cast<std::uint64_t>(std::max<std::int64_t>(
                                                                 0, holder->objects))) {
                        problem(section.offset, where + " places object " + std::to_string(number) +
                                                    " in object stream " + std::to_string(second) +
                                                    ", which does not hold it");
                        wrong = true;
                    }
                }
            }
        }
    }
    return success();
}

void PdfWalker::checkCatalog() {
    if (!startxref_.has_value() || overflow_) {
        return;
    }
    const std::optional<std::size_t> newest = sectionAt(*startxref_);
    if (!newest.has_value()) {
        return;  // checkChain() noted it
    }
    // The newest trailer's /Root, or the first one down the chain that has one.
    std::optional<std::pair<std::int64_t, std::int64_t>> root;
    std::optional<std::size_t> at = newest;
    for (std::size_t steps = 0; at.has_value() && steps < sections_.size() && !root.has_value(); ++steps) {
        root = sections_[*at].root;
        at = sections_[*at].prev.has_value() ? sectionAt(*sections_[*at].prev) : std::nullopt;
    }
    if (!root.has_value()) {
        problem(walk_.end, "no trailer names the document catalog (/Root)");
        return;
    }
    const bool top = std::any_of(objects_.begin(), objects_.end(), [&](const ObjectRecord& o) {
        return std::int64_t{o.number} == root->first && std::int64_t{o.generation} == root->second;
    });
    // In an object stream: listed by a cross-reference stream (checked there).
    const bool packed = std::any_of(streams_.begin(), streams_.end(),
                                    [](const StreamRecord& s) { return s.type == StreamType::ObjectStream; });
    if (!top && !packed) {
        problem(walk_.end, "the document catalog (object " + std::to_string(root->first) + ") is missing");
    }
}

void PdfWalker::checkLengths() {
    for (const LengthCheck& pending : lengths_) {
        const auto found = std::find_if(integers_.begin(), integers_.end(), [&](const IntegerObject& o) {
            return o.number == pending.number && o.generation == pending.generation;
        });
        if (found != integers_.end() && !pending.matches(found->value)) {
            problem(pending.data, "a stream's data is " + std::to_string(pending.raw - pending.eol) +
                                      " bytes long, but its /Length (object " + std::to_string(pending.number) +
                                      ") says " + std::to_string(found->value));
            return;
        }
    }
}

Result<bool> PdfWalker::isText(std::uint64_t from, std::uint64_t length) {
    for (std::uint64_t done = 0; done < length;) {
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(IContentReader::kMaxReadLength, length - done));
        Result<std::span<const std::byte>> piece = content_.read(from + done, take);
        if (!piece.ok()) {
            return piece.error();
        }
        if (std::any_of(piece->begin(), piece->end(), isBinary)) {
            return false;
        }
        done += take;
    }
    return true;
}

Result<bool> PdfWalker::asciiSyntax(const StreamRecord& stream, bool hex) {
    // ASCIIHexDecode: hex digits and whitespace to '>'; ASCII85Decode: '!'
    // to 'u', 'z' and whitespace to "~>" (ISO 32000-1 7.4.2, 7.4.3).
    bool ended = false;
    for (std::uint64_t done = 0; done < stream.length && !ended;) {
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(IContentReader::kMaxReadLength, stream.length - done));
        Result<std::span<const std::byte>> piece = content_.read(stream.data + done, take);
        if (!piece.ok()) {
            return piece.error();
        }
        for (std::size_t i = 0; i < piece->size() && !ended; ++i) {
            const auto c = static_cast<std::uint8_t>((*piece)[i]);
            if (isWhite(c)) {
                continue;
            }
            if (hex ? c == '>' : c == '~') {
                ended = true;
            } else if (hex ? !isHexDigit(c) : !((c >= '!' && c <= 'u') || c == 'z')) {
                return false;
            }
        }
        done += take;
    }
    return ended;
}

Status PdfWalker::checkJpeg(const StreamRecord& stream, const JpegFormat& jpeg) {
    const std::string which = "the JPEG image of object " + std::to_string(stream.number);
    if (stream.length < JpegFormat::kMinimumSize) {
        problem(stream.data, which + " is too short to be one");
        return success();
    }
    if (stream.length > JpegFormat::kMaximumSize) {
        skip(which + " is larger than the JPEG format checks");
        return success();
    }
    WindowReader window(content_, stream.data, stream.length);
    Result<EndDetection> end = jpeg.findEnd(window);
    if (!end.ok()) {
        return end.error();
    }
    switch (end->status) {
    case carving::EndStatus::Found:
        break;
    case carving::EndStatus::Truncated:
        problem(stream.data, which + " is cut short: " + end->detail);
        return success();
    case carving::EndStatus::Broken:
        problem(stream.data + end->length, which + " is damaged: " + end->detail);
        return success();
    case carving::EndStatus::Unknown:
        skip(which + " could not be walked");
        return success();
    }
    if (end->length < stream.length) {
        // A writer's padding after EOI is taken; anything more is data no check covers.
        Result<std::span<const std::byte>> rest = content_.read(
            stream.data + end->length,
            static_cast<std::size_t>(std::min<std::uint64_t>(stream.length - end->length, kJpegSlack + 1)));
        if (!rest.ok()) {
            return rest.error();
        }
        const bool padding = stream.length - end->length <= kJpegSlack &&
                             std::all_of(rest->begin(), rest->end(),
                                         [](std::byte b) { return isWhite(static_cast<std::uint8_t>(b)); });
        if (!padding) {
            skip(std::to_string(stream.length - end->length) + " bytes follow " + which + " in its stream");
            return success();
        }
    }
    WindowReader image(content_, stream.data, end->length);
    Result<ValidationResult> verdict = jpeg.validate(image);
    if (!verdict.ok()) {
        return verdict.error();
    }
    switch (verdict->status) {
    case ValidationStatus::Valid:
        break;
    case ValidationStatus::NotValidated:
        skip(which + " could not be checked: " + verdict->detail);
        break;
    case ValidationStatus::Truncated:
    case ValidationStatus::Invalid:
        problem(stream.data + std::min(verdict->validBytes, end->length), which + " is damaged: " + verdict->detail);
        break;
    }
    return success();
}

Status PdfWalker::checkStream(const StreamRecord& stream, const IDeflateDecoder& decoder, const JpegFormat& jpeg) {
    const std::string which = "object " + std::to_string(stream.number) + "'s stream";
    // An encrypted document's streams are ciphertext, but for its
    // cross-reference streams (ISO 32000-1 7.6.1).
    if (encrypted_ && stream.type != StreamType::CrossReference) {
        skip("the document is encrypted");
        return success();
    }
    if (stream.filters == 0) {
        Result<bool> text = isText(stream.data, stream.length);
        if (!text.ok()) {
            return text.error();
        }
        if (!*text) {
            skip(which + " holds binary data without a filter");
        }
        return success();
    }
    if (isFlate(stream.filter)) {
        const std::uint64_t budget = limits_.maxUnpacked > unpacked_ ? limits_.maxUnpacked - unpacked_ : 0;
        Result<IDeflateDecoder::Outcome> decoded =
            decoder.decodeZlib(content_, stream.data, stream.data + stream.length, budget, nullptr);
        if (!decoded.ok()) {
            return decoded.error();
        }
        unpacked_ += decoded->output;
        switch (decoded->kind) {
        case IDeflateDecoder::Outcome::Kind::Done:
            if (decoded->length < stream.length) {
                Result<bool> rest = isText(stream.data + decoded->length, stream.length - decoded->length);
                if (!rest.ok()) {
                    return rest.error();
                }
                if (stream.length - decoded->length > kStreamSlack || !*rest) {
                    skip(std::to_string(stream.length - decoded->length) + " bytes follow the Flate data of " +
                         which);
                }
            }
            return success();
        case IDeflateDecoder::Outcome::Kind::Truncated:
            problem(decoded->offset, "the Flate data of " + which + " does not end within the stream");
            return success();
        case IDeflateDecoder::Outcome::Kind::Invalid:
            problem(decoded->offset, "the Flate data of " + which + " is invalid: " + decoded->detail);
            return success();
        case IDeflateDecoder::Outcome::Kind::TooLarge:
            skip("the streams unpack to more than " + std::to_string(limits_.maxUnpacked) + " bytes");
            return success();
        }
        return success();
    }
    if (isDct(stream.filter)) {
        return checkJpeg(stream, jpeg);
    }
    if (isAsciiHex(stream.filter) || isAscii85(stream.filter)) {
        Result<bool> syntax = asciiSyntax(stream, isAsciiHex(stream.filter));
        if (!syntax.ok()) {
            return syntax.error();
        }
        if (!*syntax) {
            problem(stream.data, "the " + stream.filter + " data of " + which + " is malformed");
        }
        return success();
    }
    skip(which + " uses " + (stream.filter == "?" ? std::string("a filter that is not a name") : stream.filter));
    return success();
}

Status PdfWalker::checkStreams(const IDeflateDecoder& decoder, const JpegFormat& jpeg) {
    for (const StreamRecord& stream : streams_) {
        if (Status checked = checkStream(stream, decoder, jpeg); !checked.ok()) {
            return checked;
        }
    }
    return success();
}

// Whether `header` holds, after comments and whitespace, an object header
// ("n g obj") or a cross-reference table ("xref").
bool startsBody(std::span<const std::byte> header) noexcept {
    std::size_t at = 0;
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(header[i]); };
    for (;;) {
        while (at < header.size() && isWhite(byte(at))) {
            ++at;
        }
        if (at < header.size() && byte(at) == '%') {
            while (at < header.size() && byte(at) != '\r' && byte(at) != '\n') {
                ++at;
            }
            continue;
        }
        break;
    }
    const auto run = [&](auto test, std::size_t most) {
        std::size_t n = 0;
        while (n < most && at < header.size() && test(byte(at))) {
            ++at;
            ++n;
        }
        return n;
    };
    const std::size_t start = at;
    if (header.size() - at >= 4 && byte(at) == 'x' && byte(at + 1) == 'r' && byte(at + 2) == 'e' &&
        byte(at + 3) == 'f') {
        return true;
    }
    at = start;
    return run(isDigit, 10) > 0 && run(isWhite, 4) > 0 && run(isDigit, 5) > 0 && run(isWhite, 4) > 0 &&
           header.size() - at >= 3 && byte(at) == 'o' && byte(at + 1) == 'b' && byte(at + 2) == 'j';
}

}  // namespace

Result<std::shared_ptr<const PdfFormat>> PdfFormat::make(std::shared_ptr<const IDeflateDecoder> decoder,
                                                         Limits limits) {
    if (decoder == nullptr) {
        return makeError(ErrorCode::InvalidInput, "the PDF format needs a DEFLATE decoder");
    }
    if (limits.maxObjects == 0 || limits.maxUnpacked == 0 || limits.maxSearchedStream == 0 ||
        limits.maxSections == 0) {
        return makeError(ErrorCode::InvalidInput, "the PDF format's limits must not be 0");
    }
    return std::shared_ptr<const PdfFormat>(new PdfFormat(std::move(decoder), limits));
}

PdfFormat::PdfFormat(std::shared_ptr<const IDeflateDecoder> decoder, Limits limits)
    : decoder_(std::move(decoder)), limits_(limits) {
    descriptor_.id = "pdf";
    descriptor_.name = "PDF document";
    descriptor_.extension = "pdf";
    descriptor_.signatures = {carving::textSignature("PDF header", "%PDF-")};
    descriptor_.minimumSize = kMinimumSize;
    descriptor_.maximumSize = kMaximumSize;
    descriptor_.headerSize = kHeaderSize;
    descriptor_.endDetection = carving::EndDetectionMethod::StructureWalk;
    // Each element is found where the one before it ends: a document whose
    // %%EOF was reached is in one piece, and a bad stream in it is damage.
    descriptor_.foundEndMeansContiguous = true;
}

HeaderCheck PdfFormat::checkHeader(std::span<const std::byte> header) const {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(header[i]); };
    if (header.size() < 8 || byte(0) != '%' || byte(1) != 'P' || byte(2) != 'D' || byte(3) != 'F' || byte(4) != '-') {
        return HeaderCheck::reject("no PDF header");
    }
    std::size_t at = 5;
    std::size_t major = 0;
    while (at < header.size() && isDigit(byte(at)) && major < 2) {
        ++at;
        ++major;
    }
    if (major == 0 || at >= header.size() || byte(at) != '.') {
        return HeaderCheck::reject("a PDF header without a version");
    }
    ++at;
    std::size_t minor = 0;
    while (at < header.size() && isDigit(byte(at)) && minor < 2) {
        ++at;
        ++minor;
    }
    if (minor == 0) {
        return HeaderCheck::reject("a PDF header without a version");
    }
    // The rest of the header line, then the body.
    while (at < header.size() && byte(at) != '\r' && byte(at) != '\n') {
        ++at;
    }
    if (!startsBody(header.subspan(at))) {
        return HeaderCheck::reject("no object after the PDF header");
    }
    return HeaderCheck::accept();
}

Result<EndDetection> PdfFormat::findEnd(IContentReader& content) const {
    PdfWalker walker(content, Depth::Layout, limits_);
    Result<Walk> walk = walker.run();
    if (!walk.ok()) {
        return walk.error();
    }
    return detail::endOf(*walk, content.size());
}

Result<ValidationResult> PdfFormat::validate(IContentReader& content) const {
    PdfWalker walker(content, Depth::Full, limits_);
    Result<Walk> run = walker.run();
    if (!run.ok()) {
        return run.error();
    }
    if (run->status == WalkStatus::Complete) {
        if (Status padded = walker.takePadding(); !padded.ok()) {
            return padded.error();
        }
        if (Status checked = walker.check(*decoder_, jpeg_); !checked.ok()) {
            return checked.error();
        }
    }
    // The walker's walk: check() notes its problems there.
    ValidationResult verdict = detail::verdictOf(walker.walk(), content.size());
    if (verdict.status == ValidationStatus::Valid) {
        if (walker.unchecked() > 0) {
            // Consistent, but not every stream could be checked: never trusted.
            verdict.status = ValidationStatus::NotValidated;
            verdict.detail += "; " + count(walker.unchecked(), "stream was", "streams were") + " not checked (" +
                              walker.uncheckedReason() + ")";
        } else {
            verdict.detail += ", every stream checked";
        }
    }
    return verdict;
}

}  // namespace recovery::formats
