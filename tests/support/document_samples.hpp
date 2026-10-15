#pragma once

// Small documents that real writers made: Microsoft Edge, Microsoft Print to
// PDF, the Microsoft XPS Document Writer, Windows' structured storage and
// Windows Installer (tests/reference/make_document_samples.ps1), and qpdf's
// variants of them (make_document_samples.sh), embedded by
// tests/reference/embed_document_samples.py. They guard against the document
// builders and the format modules sharing a misreading of a specification,
// and against writers' habits that a specification does not mention
// (Microsoft Print to PDF records every offset one byte early).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::test::document_samples {

// The bytes of a sample at `offset`; the bytes between pieces are zeros.
struct Piece {
    std::uint32_t offset = 0;
    std::span<const std::uint8_t> bytes;
};

struct Sample {
    std::string_view name;
    // The carving format id: "pdf", "ole2" or "zip".
    std::string_view format;
    std::string_view producer;
    std::size_t size = 0;
    std::span<const Piece> pieces;

    [[nodiscard]] std::vector<std::byte> data() const {
        std::vector<std::byte> out(size, std::byte{0});
        for (const Piece& piece : pieces) {
            for (std::size_t i = 0; i < piece.bytes.size(); ++i) {
                out[piece.offset + i] = static_cast<std::byte>(piece.bytes[i]);
            }
        }
        return out;
    }
};

[[nodiscard]] const std::vector<Sample>& all();

// The sample with this file name; aborts the test program when there is none.
[[nodiscard]] const Sample& named(std::string_view name);

}  // namespace recovery::test::document_samples
