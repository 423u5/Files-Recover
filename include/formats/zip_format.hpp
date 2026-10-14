#pragma once

// ZIP archives (PKWARE APPNOTE 6.3.10) carving and structural validation:
// .zip files and the formats built on ZIP (.docx, .xlsx, .pptx, .odt, .jar,
// .apk, .epub), which are carved as ZIP archives.
//
// An archive is walked from its first local header: every member's header
// and data (and data descriptor), an APK signing block, the central
// directory, the ZIP64 end of central directory record and locator when
// there are any, and the end of central directory record, whose comment ends
// the file. End detection follows the recorded sizes, and decodes the
// DEFLATE data of members whose size only a data descriptor records.
//
// Validation also checks that the central directory describes exactly the
// members walked, and decodes every member's data for its CRC-32 and size.
// An archive is Valid only when every member's data is: the carver then
// trusts it and skips the hits inside it (the images stored in an archive
// are part of it). An archive with a member the engine cannot decode (an
// encrypted one, or a compression method other than stored and DEFLATE), or
// whose members unpack to more than the limit, is NotValidated: kept, never
// trusted. See docs/formats/archives.md.
//
// Every member's local header starts with the format's signature, so the
// format is self-synchronizing (FormatDescriptor::selfSynchronizing): the
// carver skips the member headers of an archive it has carved already. And an
// archive whose end was found lies in one piece
// (FormatDescriptor::foundEndMeansContiguous): a damaged one is not taken for
// a fragmented one.

#include "carving/content_reader.hpp"
#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace recovery::formats {

// Decodes raw DEFLATE data (RFC 1951) for the ZIP format. The engine's
// decoder is the validation library's (validation::deflateDecoder()); that
// library depends on this one, so the format is handed a decoder when it is
// made.
//
// Thread safety: decode() is const and must be safe to call concurrently.
class IDeflateDecoder {
public:
    struct Outcome {
        enum class Kind : std::uint8_t {
            // The stream ended (its last block) within the bytes given.
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
        // that holds the end of its last block.
        std::uint64_t length = 0;
        // Bytes the data unpacked to (as far as it went), and their CRC-32.
        std::uint64_t output = 0;
        std::uint32_t crc32 = 0;
        // Truncated and Invalid: the content offset of the problem.
        std::uint64_t offset = 0;
        // Invalid: what is wrong (never file content).
        std::string detail;
    };

    virtual ~IDeflateDecoder() = default;

    // Decodes the stream that starts at content offset `begin`, reading no
    // byte at or after `end`, and stops (TooLarge) once it unpacks to more
    // than `maxOutput` bytes. Problems of the data are outcomes; fails only
    // with the reader's errors (Cancelled, source failures).
    [[nodiscard]] virtual Result<Outcome> decode(carving::IContentReader& content, std::uint64_t begin,
                                                 std::uint64_t end, std::uint64_t maxOutput) const = 0;

protected:
    IDeflateDecoder() = default;
    IDeflateDecoder(const IDeflateDecoder&) = default;
    IDeflateDecoder& operator=(const IDeflateDecoder&) = default;
    IDeflateDecoder(IDeflateDecoder&&) = default;
    IDeflateDecoder& operator=(IDeflateDecoder&&) = default;
};

// Bounds on what one archive may cost (ZipFormat::Limits). At namespace
// scope: a default argument inside ZipFormat could not use the member
// initializers of a class nested in it.
struct ZipLimits {
    // Members walked at most: their places are kept until the central
    // directory is checked. An archive with more is carved up to there
    // (Broken).
    std::uint64_t maxMembers = std::uint64_t{1} << 20;
    // Bytes the members of one archive may unpack to in all. Members beyond
    // are not decoded, and the archive is NotValidated.
    std::uint64_t maxUnpacked = 4 * kGiB;
};

class ZipFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // A local header with a one-byte name and no data, its central directory
    // entry, and the end of central directory record.
    static constexpr std::uint64_t kMinimumSize = 30 + 1 + 46 + 1 + 22;
    static constexpr std::uint64_t kMaximumSize = 64 * kGiB;
    // The local header and the start of the first member's name.
    static constexpr std::uint32_t kHeaderSize = 30 + 256;

    using Limits = ZipLimits;

    // InvalidInput when `decoder` is null or a limit is 0.
    [[nodiscard]] static Result<std::shared_ptr<const ZipFormat>> make(std::shared_ptr<const IDeflateDecoder> decoder,
                                                                       Limits limits = Limits{});

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // The signature, then a version, compression method, flags and name
    // that a ZIP writer could have written.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the end of central directory record's comment.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    ZipFormat(std::shared_ptr<const IDeflateDecoder> decoder, Limits limits);

    carving::FormatDescriptor descriptor_;
    std::shared_ptr<const IDeflateDecoder> decoder_;
    Limits limits_;
};

}  // namespace recovery::formats
