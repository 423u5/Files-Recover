#include "metadata_test_helpers.hpp"

#include "carving/content_reader.hpp"
#include "recovery/byte_order.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

namespace recovery::metadata::test {

namespace {

void put8(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

void putBe16(Bytes& out, std::uint32_t value) {
    put8(out, value >> 8);
    put8(out, value);
}

void putBe32(Bytes& out, std::uint32_t value) {
    putBe16(out, value >> 16);
    putBe16(out, value & 0xFFFF);
}

void append(Bytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

Bytes syncsafe(std::uint32_t value) {
    Bytes out;
    for (int shift = 21; shift >= 0; shift -= 7) {
        put8(out, (value >> shift) & 0x7F);
    }
    return out;
}

}  // namespace

MediaMetadata extract(std::span<const std::byte> bytes, std::string_view formatId, const MetadataOptions& options) {
    carving::MemoryContentReader reader(bytes);
    Result<MediaMetadata> metadata = extractMetadata(reader, formatId, options);
    EXPECT_TRUE(metadata.ok()) << (metadata.ok() ? std::string() : describe(metadata.error()));
    return metadata.ok() ? std::move(metadata).value() : MediaMetadata{};
}

std::string issuesText(const MediaMetadata& metadata) {
    std::string out;
    for (const MetadataIssue& issue : metadata.issues) {
        out += (issue.offset.has_value() ? std::to_string(*issue.offset) : std::string("-")) + ": " + issue.detail +
               "\n";
    }
    return out;
}

Bytes text(std::string_view value) {
    Bytes out;
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

Bytes utf16(std::u16string_view value, bool bigEndian) {
    Bytes out;
    if (!bigEndian) {
        put8(out, 0xFF);
        put8(out, 0xFE);
    }
    for (const char16_t unit : value) {
        if (bigEndian) {
            putBe16(out, unit);
        } else {
            put8(out, unit & 0xFF);
            put8(out, unit >> 8);
        }
    }
    return out;
}

Bytes id3Tag(std::uint8_t version, const std::vector<Id3Frame>& frames, std::size_t padding, bool unsync) {
    Bytes body;
    for (const Id3Frame& frame : frames) {
        append(body, text(frame.id));
        const auto size = static_cast<std::uint32_t>(frame.body.size());
        if (version == 2) {
            put8(body, size >> 16);
            putBe16(body, size & 0xFFFF);
        } else {
            append(body, version == 4 ? syncsafe(size) : Bytes{});
            if (version == 3) {
                putBe32(body, size);
            }
            putBe16(body, 0);
        }
        append(body, frame.body);
    }
    body.resize(body.size() + padding, std::byte{0});
    if (unsync) {
        Bytes encoded;
        for (std::size_t i = 0; i < body.size(); ++i) {
            encoded.push_back(body[i]);
            const bool next = i + 1 < body.size();
            if (body[i] == std::byte{0xFF} &&
                (!next || body[i + 1] == std::byte{0} || (static_cast<std::uint8_t>(body[i + 1]) & 0xE0) == 0xE0)) {
                encoded.push_back(std::byte{0});
            }
        }
        body = std::move(encoded);
    }
    Bytes out = text("ID3");
    put8(out, version);
    put8(out, 0);
    put8(out, unsync ? 0x80 : 0);
    append(out, syncsafe(static_cast<std::uint32_t>(body.size())));
    append(out, body);
    return out;
}

Bytes id3Text(std::uint8_t encoding, std::span<const std::byte> value) {
    Bytes out;
    put8(out, encoding);
    append(out, value);
    return out;
}

Bytes box(std::string_view type, std::span<const std::byte> payload) {
    Bytes out;
    putBe32(out, static_cast<std::uint32_t>(payload.size() + 8));
    append(out, text(type));
    append(out, payload);
    return out;
}

Bytes fullBox(std::string_view type, std::uint8_t version, std::uint32_t flags, std::span<const std::byte> payload) {
    Bytes body;
    putBe32(body, (std::uint32_t{version} << 24) | (flags & 0xFFFFFF));
    append(body, payload);
    return box(type, body);
}

Bytes ilstItem(std::string_view type, std::uint32_t dataType, std::span<const std::byte> value) {
    Bytes data;
    putBe32(data, dataType);
    putBe32(data, 0);
    append(data, value);
    return box(type, box("data", data));
}

Bytes userData(const std::vector<Bytes>& items) {
    Bytes ilst;
    for (const Bytes& item : items) {
        append(ilst, item);
    }
    Bytes hdlr;
    putBe32(hdlr, 0);
    append(hdlr, text("mdirappl"));
    hdlr.resize(hdlr.size() + 9, std::byte{0});
    Bytes meta = fullBox("hdlr", 0, 0, hdlr);
    append(meta, box("ilst", ilst));
    return box("udta", fullBox("meta", 0, 0, meta));
}

Bytes withMoovChild(Bytes file, std::span<const std::byte> child) {
    std::size_t pos = 0;
    while (pos + 8 <= file.size()) {
        const std::uint32_t size = loadBe32(file, pos);
        const bool moov = std::equal(file.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                                     file.begin() + static_cast<std::ptrdiff_t>(pos + 8), text("moov").begin());
        if (size < 8) {
            break;
        }
        if (moov) {
            if (pos + size != file.size()) {
                throw std::invalid_argument("moov is not the last box");
            }
            storeBe32(file, pos, static_cast<std::uint32_t>(size + child.size()));
            append(file, child);
            return file;
        }
        pos += size;
    }
    throw std::invalid_argument("no moov box");
}

std::size_t find(std::span<const std::byte> haystack, std::span<const std::byte> needle) {
    const auto found = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
    return found == haystack.end() ? std::string::npos : static_cast<std::size_t>(found - haystack.begin());
}

}  // namespace recovery::metadata::test
