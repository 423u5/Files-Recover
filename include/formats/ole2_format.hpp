#pragma once

// OLE2 compound files ([MS-CFB], "structured storage") carving and
// structural validation: the Office 97-2003 documents (.doc, .xls, .ppt),
// Outlook messages (.msg), Windows Installer packages (.msi, .msp, .mst),
// Visio and Publisher files, thumbnail caches (Thumbs.db).
//
// A compound file is a small file system: a header, a FAT that chains its
// sectors (the DIFAT lists the FAT's own sectors), a directory of storages
// and streams, and a mini stream that holds the small streams in 64-byte
// mini sectors (the mini FAT chains those). Its end is its last sector in
// use, which the FAT says; its kind is what its root's streams are called
// ("WordDocument": doc) or, for Windows Installer files, its root's class.
//
// Validation checks the header, every FAT entry, the directory (every entry
// and its tree) and the chain of every stream: the chain's length is the
// stream's, no sector belongs to two chains, the mini stream holds the small
// streams. Nothing in a compound file checks the data of its streams, so a
// document whose data another file overwrote still validates: the format
// does not trust its files (FormatDescriptor::trustValidFiles), and the hits
// inside a compound file are carved as if it were not there (the pictures in
// a document, the thumbnails in Thumbs.db). See docs/formats/documents.md.

#include "carving/content_reader.hpp"
#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace recovery::formats {

// Bounds on what one compound file may cost (Ole2Format::Limits).
struct Ole2Limits {
    // FAT entries (sectors) read: 4 bytes each. A file whose FAT has more
    // is carved up to its header (Broken).
    std::uint64_t maxSectors = std::uint64_t{1} << 24;
    // Directory entries read. A directory with more is not checked beyond,
    // and the file is NotValidated.
    std::uint64_t maxEntries = std::uint64_t{1} << 20;
};

class Ole2Format final : public carving::IFileFormat, public carving::FormatValidator {
public:
    // A header sector, a FAT sector and a directory sector of 512 bytes.
    static constexpr std::uint64_t kMinimumSize = std::uint64_t{3} * 512;
    static constexpr std::uint64_t kMaximumSize = 16 * kGiB;
    static constexpr std::uint32_t kHeaderSize = 512;

    using Limits = Ole2Limits;

    // InvalidInput when a limit is 0.
    [[nodiscard]] static Result<std::shared_ptr<const Ole2Format>> make(Limits limits = Limits{});

    [[nodiscard]] const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    // The signature, then a byte order, versions, sector sizes and a mini
    // stream cutoff that a compound file has.
    [[nodiscard]] carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    // StructureWalk: the end of the last sector the FAT has in use. The
    // extension is what the file's root holds: doc ("WordDocument"), xls
    // ("Workbook", "Book"), ppt ("PowerPoint Document"), msg ("__substg1.0_"
    // streams), vsd ("VisioDocument"), pub ("Quill"), db (Thumbs.db's
    // "Catalog"), msi, msp, mst (the root's class); empty for another file.
    [[nodiscard]] Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    [[nodiscard]] const carving::FormatValidator& validator() const noexcept override { return *this; }
    [[nodiscard]] Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    explicit Ole2Format(Limits limits);

    carving::FormatDescriptor descriptor_;
    Limits limits_;
};

}  // namespace recovery::formats
