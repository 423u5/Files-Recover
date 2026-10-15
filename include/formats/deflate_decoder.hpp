#pragma once

// DEFLATE decoding for the format modules: the data of ZIP members (raw
// DEFLATE, RFC 1951) and of PDF streams (zlib, RFC 1950). The engine's
// decoder is the validation library's (validation::deflateDecoder()); that
// library depends on this one, so a format is handed a decoder when it is
// made.

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recovery::formats {

// Thread safety: the decode functions are const and must be safe to call
// concurrently.
class IDeflateDecoder {
public:
    struct Outcome {
        enum class Kind : std::uint8_t {
            // The stream ended (its last block; for zlib, a right Adler-32)
            // within the bytes given.
            Done,
            // The bytes given end inside the stream.
            Truncated,
            // The stream is malformed (see detail and offset).
            Invalid,
            // The stream unpacks to more than the bytes allowed.
            TooLarge,
        };

        Kind kind = Kind::Invalid;
        // Done: the bytes of the stream, from its first byte to the byte
        // that holds the end of its last block (raw), or to the end of its
        // Adler-32 (zlib).
        std::uint64_t length = 0;
        // Bytes the data unpacked to (as far as it went), and (decode()
        // only: zlib checks its own Adler-32) their CRC-32.
        std::uint64_t output = 0;
        std::uint32_t crc32 = 0;
        // Truncated and Invalid: the content offset of the problem.
        std::uint64_t offset = 0;
        // Invalid: what is wrong (never file content).
        std::string detail;
    };

    virtual ~IDeflateDecoder() = default;

    // Decodes the raw DEFLATE stream that starts at content offset `begin`,
    // reading no byte at or after `end`, and stops (TooLarge) once it
    // unpacks to more than `maxOutput` bytes. Problems of the data are
    // outcomes; fails only with the reader's errors (Cancelled, source
    // failures).
    [[nodiscard]] virtual Result<Outcome> decode(carving::IContentReader& content, std::uint64_t begin,
                                                 std::uint64_t end, std::uint64_t maxOutput) const = 0;

    // As decode(), for a zlib stream: its header, the DEFLATE data, and an
    // Adler-32 of the output that must be right for Done (a wrong one is
    // Invalid). When `kept` is not null, the output is appended to it
    // (at most maxOutput bytes).
    [[nodiscard]] virtual Result<Outcome> decodeZlib(carving::IContentReader& content, std::uint64_t begin,
                                                     std::uint64_t end, std::uint64_t maxOutput,
                                                     std::vector<std::byte>* kept) const = 0;

protected:
    IDeflateDecoder() = default;
    IDeflateDecoder(const IDeflateDecoder&) = default;
    IDeflateDecoder& operator=(const IDeflateDecoder&) = default;
    IDeflateDecoder(IDeflateDecoder&&) = default;
    IDeflateDecoder& operator=(IDeflateDecoder&&) = default;
};

}  // namespace recovery::formats
