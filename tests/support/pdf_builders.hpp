#pragma once

// PDF documents built here for the tests, written the way writers write
// them: a header and a binary comment, indirect objects, and for each
// revision a cross-reference table (or stream) with its trailer, startxref
// and %%EOF. Flate data is made of stored deflate blocks, so the bytes of a
// stream show as they are (an image's signature too). Documents real
// writers made are the samples of document_samples.hpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

// A zlib stream (RFC 1950) of stored deflate blocks holding `data`.
[[nodiscard]] std::vector<std::byte> storedZlib(std::span<const std::byte> data);

// The bytes of `text`.
[[nodiscard]] std::vector<std::byte> textBytes(std::string_view text);

class PdfWriter {
public:
    // "%PDF-<version>", the binary comment line, and `eol` after every line.
    explicit PdfWriter(std::string_view version = "1.7", std::string_view eol = "\n");

    // Appends bytes as they are (a comment, damage, garbage).
    void raw(std::string_view text);
    void raw(std::span<const std::byte> bytes);

    // The number the next object gets.
    [[nodiscard]] std::uint32_t nextNumber() const noexcept { return next_; }
    // "n 0 obj" `value` "endobj"; the object's number.
    std::uint32_t object(std::string_view value);
    // A stream object: the dictionary `entries` (without << >>) and /Length,
    // then the data. With `lengthObject`, /Length is a reference to an
    // object that follows the stream and holds it.
    std::uint32_t stream(std::string_view entries, std::span<const std::byte> data, bool lengthObject = false);
    // An object stream (/Type /ObjStm, Flate) holding `values` as objects of
    // their own; their numbers. Only a cross-reference stream can list them.
    std::vector<std::uint32_t> objectStream(const std::vector<std::string>& values);
    // Ends a revision: a cross-reference table of the objects written since
    // the last revision (the first one's also lists object 0, free), a
    // trailer with /Size, /Root, /Prev and `extra`, startxref and %%EOF.
    void endWithTable(std::uint32_t root, std::string_view extra = {});
    // The same with a cross-reference stream (Flate; with `predictor`, the
    // PNG Up predictor as most writers use).
    void endWithStream(std::uint32_t root, bool predictor = true, std::string_view extra = {});
    // A hybrid-reference revision (ISO 32000-1 7.5.8.4), as Microsoft Office
    // writes them: a cross-reference stream of the objects in object streams,
    // then a table of the others whose trailer's /XRefStm points at the stream.
    void endHybrid(std::uint32_t root);

    [[nodiscard]] std::uint64_t size() const noexcept { return out_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return out_; }

private:
    struct Placed {
        std::uint32_t number = 0;
        std::uint64_t offset = 0;
        // In an object stream: its number and the index there.
        std::uint32_t holder = 0;
        std::uint32_t index = 0;
    };

    void line(std::string_view text);
    void startxrefAndEof(std::uint64_t at);
    // "xref" and the subsections of `placed` (and of object 0 in the first revision).
    void table(std::vector<Placed> placed);
    // Object `self`: a cross-reference stream of `placed` (and of object 0
    // with `withFree`), its dictionary ending in `entries`.
    void crossReferenceStream(std::uint32_t self, std::vector<Placed> placed, bool withFree, bool predictor,
                              std::string_view entries);
    // startxref `at`, %%EOF, and a new revision begins.
    void endRevision(std::uint64_t at);

    std::string eol_;
    std::vector<std::byte> out_;
    std::vector<Placed> since_;
    std::uint32_t next_ = 1;
    std::uint64_t prev_ = 0;
    bool first_ = true;
};

// A one-page document as most writers make it: catalog, page tree, page, a
// Flate content stream showing `text`, a font, an uncompressed XMP metadata
// stream. With a JPEG, the page also draws it as an image XObject
// (DCTDecode).
[[nodiscard]] std::vector<std::byte> simplePdf(std::string_view text = "Recovered",
                                               std::span<const std::byte> jpeg = {});

// A document of three revisions: the first, an update that changes a page,
// an update that adds an object.
[[nodiscard]] std::vector<std::byte> updatedPdf();

// PDF 1.5: the objects in an object stream, a cross-reference stream (with
// `predictor`, the PNG Up predictor).
[[nodiscard]] std::vector<std::byte> compressedPdf(bool predictor = true);

// The same document as a hybrid-reference file (PdfWriter::endHybrid): a
// reader of PDF 1.4 finds every object but the ones in the object stream.
[[nodiscard]] std::vector<std::byte> hybridPdf();

// A linearized layout: the first page's cross-reference section first, with
// startxref 0 and a /Prev that points forward at the main section (no hint
// stream: the layout, not a file a viewer could load page by page).
[[nodiscard]] std::vector<std::byte> linearizedPdf();

}  // namespace recovery::test
