#include "support/pdf_builders.hpp"

#include <algorithm>
#include <cstdio>

namespace recovery::test {

namespace {

std::uint32_t adler32(std::span<const std::byte> data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::byte byte : data) {
        a = (a + static_cast<std::uint8_t>(byte)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

std::string tenDigits(std::uint64_t value) {
    std::string text = std::to_string(value);
    return std::string(text.size() < 10 ? 10 - text.size() : 0, '0') + text;
}

void putBe(std::vector<std::byte>& out, std::uint64_t value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
}

}  // namespace

std::vector<std::byte> storedZlib(std::span<const std::byte> data) {
    std::vector<std::byte> out = {std::byte{0x78}, std::byte{0x01}};
    std::size_t position = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, data.size() - position);
        const bool last = position + length == data.size();
        out.push_back(std::byte{static_cast<std::uint8_t>(last ? 1 : 0)});
        out.push_back(static_cast<std::byte>(length & 0xFF));
        out.push_back(static_cast<std::byte>(length >> 8));
        out.push_back(static_cast<std::byte>(~length & 0xFF));
        out.push_back(static_cast<std::byte>((~length >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(position),
                   data.begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
    } while (position < data.size());
    putBe(out, adler32(data), 4);
    return out;
}

std::vector<std::byte> textBytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

PdfWriter::PdfWriter(std::string_view version, std::string_view eol) : eol_(eol) {
    line("%PDF-" + std::string(version));
    // A comment of bytes above 127, so that transfers take the file as binary.
    raw("%");
    raw(std::vector<std::byte>{std::byte{0xE2}, std::byte{0xE3}, std::byte{0xCF}, std::byte{0xD3}});
    raw(eol_);
}

void PdfWriter::raw(std::string_view text) {
    const std::vector<std::byte> bytes = textBytes(text);
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void PdfWriter::raw(std::span<const std::byte> bytes) {
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void PdfWriter::line(std::string_view text) {
    raw(text);
    raw(eol_);
}

std::uint32_t PdfWriter::object(std::string_view value) {
    const std::uint32_t number = next_++;
    since_.push_back(Placed{number, out_.size()});
    line(std::to_string(number) + " 0 obj");
    line(value);
    line("endobj");
    return number;
}

std::uint32_t PdfWriter::stream(std::string_view entries, std::span<const std::byte> data, bool lengthObject) {
    const std::uint32_t number = next_++;
    since_.push_back(Placed{number, out_.size()});
    line(std::to_string(number) + " 0 obj");
    const std::uint32_t length = lengthObject ? next_++ : 0;
    line("<<" + std::string(entries) + " /Length " +
         (lengthObject ? std::to_string(length) + " 0 R" : std::to_string(data.size())) + ">>");
    line("stream");
    raw(data);
    raw(eol_);
    line("endstream");
    line("endobj");
    if (lengthObject) {
        since_.push_back(Placed{length, out_.size()});
        line(std::to_string(length) + " 0 obj");
        line(std::to_string(data.size()));
        line("endobj");
    }
    return number;
}

std::vector<std::uint32_t> PdfWriter::objectStream(const std::vector<std::string>& values) {
    const std::uint32_t holder = next_++;
    std::vector<std::uint32_t> numbers;
    std::string head;
    std::string body;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint32_t number = next_++;
        numbers.push_back(number);
        head += std::to_string(number) + " " + std::to_string(body.size()) + " ";
        body += values[i] + "\n";
    }
    const std::vector<std::byte> data = storedZlib(textBytes(head + body));
    since_.push_back(Placed{holder, out_.size()});
    line(std::to_string(holder) + " 0 obj");
    line("<</Type /ObjStm /N " + std::to_string(values.size()) + " /First " + std::to_string(head.size()) +
         " /Filter /FlateDecode /Length " + std::to_string(data.size()) + ">>");
    line("stream");
    raw(data);
    raw(eol_);
    line("endstream");
    line("endobj");
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        since_.push_back(Placed{numbers[i], 0, holder, static_cast<std::uint32_t>(i)});
    }
    return numbers;
}

void PdfWriter::startxrefAndEof(std::uint64_t at) {
    line("startxref");
    line(std::to_string(at));
    line("%%EOF");
}

void PdfWriter::table(std::vector<Placed> placed) {
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.number < b.number; });
    line("xref");
    // A subsection per run of consecutive numbers; entries end in " \n" or "\r\n" (20 bytes).
    const std::string entryEnd = eol_ == "\r\n" ? "\r\n" : " \n";
    if (first_) {
        line("0 1");
        raw("0000000000 65535 f" + entryEnd);
    }
    for (std::size_t i = 0; i < placed.size();) {
        std::size_t j = i + 1;
        while (j < placed.size() && placed[j].number == placed[j - 1].number + 1) {
            ++j;
        }
        line(std::to_string(placed[i].number) + " " + std::to_string(j - i));
        for (std::size_t k = i; k < j; ++k) {
            raw(tenDigits(placed[k].offset) + " 00000 n" + entryEnd);
        }
        i = j;
    }
}

void PdfWriter::crossReferenceStream(std::uint32_t self, std::vector<Placed> placed, bool withFree, bool predictor,
                                     std::string_view entries) {
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.number < b.number; });
    // /W [1 4 2]: type, offset (or holder), generation (or index).
    std::vector<std::byte> rows;
    std::vector<std::byte> previous(7, std::byte{0});
    std::string index;
    const auto addRow = [&](std::uint8_t type, std::uint64_t second, std::uint64_t third) {
        std::vector<std::byte> row;
        putBe(row, type, 1);
        putBe(row, second, 4);
        putBe(row, third, 2);
        if (predictor) {
            rows.push_back(std::byte{2});  // Up
            for (std::size_t i = 0; i < row.size(); ++i) {
                rows.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(row[i]) -
                                                      static_cast<std::uint8_t>(previous[i])));
            }
        } else {
            rows.insert(rows.end(), row.begin(), row.end());
        }
        previous = row;
    };
    if (withFree) {
        index += "0 1 ";
        addRow(0, 0, 65535);
    }
    for (std::size_t i = 0; i < placed.size();) {
        std::size_t j = i + 1;
        while (j < placed.size() && placed[j].number == placed[j - 1].number + 1) {
            ++j;
        }
        index += std::to_string(placed[i].number) + " " + std::to_string(j - i) + " ";
        for (std::size_t k = i; k < j; ++k) {
            if (placed[k].holder != 0) {
                addRow(2, placed[k].holder, placed[k].index);
            } else {
                addRow(1, placed[k].offset, 0);
            }
        }
        i = j;
    }
    const std::vector<std::byte> data = storedZlib(rows);
    line(std::to_string(self) + " 0 obj");
    line("<</Type /XRef /Size " + std::to_string(next_) + " /W [1 4 2] /Index [" + index + "]" +
         (predictor ? " /DecodeParms <</Columns 7 /Predictor 12>>" : "") + " /Filter /FlateDecode /Length " +
         std::to_string(data.size()) + std::string(entries) + ">>");
    line("stream");
    raw(data);
    raw(eol_);
    line("endstream");
    line("endobj");
}

void PdfWriter::endRevision(std::uint64_t at) {
    startxrefAndEof(at);
    prev_ = at;
    first_ = false;
    since_.clear();
}

void PdfWriter::endWithTable(std::uint32_t root, std::string_view extra) {
    const std::uint64_t at = out_.size();
    table(since_);
    line("trailer");
    line("<</Size " + std::to_string(next_) + " /Root " + std::to_string(root) + " 0 R" +
         (first_ ? std::string() : " /Prev " + std::to_string(prev_)) + std::string(extra) + ">>");
    endRevision(at);
}

void PdfWriter::endWithStream(std::uint32_t root, bool predictor, std::string_view extra) {
    const std::uint32_t self = next_++;
    const std::uint64_t at = out_.size();
    std::vector<Placed> placed = since_;
    placed.push_back(Placed{self, at});
    crossReferenceStream(self, placed, first_, predictor,
                         " /Root " + std::to_string(root) + " 0 R" +
                             (first_ ? std::string() : " /Prev " + std::to_string(prev_)) + std::string(extra));
    endRevision(at);
}

void PdfWriter::endHybrid(std::uint32_t root) {
    // The stream lists the objects in object streams, the table the others
    // (and the stream's own object).
    const std::uint32_t self = next_++;
    const std::uint64_t streamAt = out_.size();
    std::vector<Placed> packed;
    std::vector<Placed> direct;
    for (const Placed& placed : since_) {
        (placed.holder != 0 ? packed : direct).push_back(placed);
    }
    crossReferenceStream(self, packed, false, true, {});
    direct.push_back(Placed{self, streamAt});
    const std::uint64_t at = out_.size();
    table(direct);
    line("trailer");
    line("<</Size " + std::to_string(next_) + " /Root " + std::to_string(root) + " 0 R" +
         (first_ ? std::string() : " /Prev " + std::to_string(prev_)) + " /XRefStm " + std::to_string(streamAt) +
         ">>");
    endRevision(at);
}

std::vector<std::byte> simplePdf(std::string_view text, std::span<const std::byte> jpeg) {
    PdfWriter pdf("1.7");
    const std::uint32_t catalog = pdf.object("<</Type /Catalog /Pages 2 0 R /Metadata 3 0 R>>");
    pdf.object("<</Type /Pages /Kids [4 0 R] /Count 1>>");
    const std::string xmp = "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                            "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></x:xmpmeta>\n"
                            "<?xpacket end=\"w\"?>";
    pdf.stream("/Type /Metadata /Subtype /XML", textBytes(xmp));
    const bool image = !jpeg.empty();
    pdf.object(std::string("<</Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 5 0 R /Resources "
                           "<</Font <</F1 6 0 R>>") +
               (image ? " /XObject <</Im1 7 0 R>>" : "") + ">>>>");
    std::string content = "BT /F1 24 Tf 72 700 Td (" + std::string(text) + ") Tj ET";
    if (image) {
        content += "\nq 200 0 0 150 72 400 cm /Im1 Do Q";
    }
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes(content)));
    pdf.object("<</Type /Font /Subtype /Type1 /BaseFont /Helvetica>>");
    if (image) {
        pdf.stream("/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8 "
                   "/Filter /DCTDecode",
                   jpeg);
    }
    pdf.endWithTable(catalog, " /Info <<>>");
    return pdf.bytes();
}

std::vector<std::byte> updatedPdf() {
    PdfWriter pdf("1.4");
    pdf.object("<</Type /Catalog /Pages 2 0 R>>");
    pdf.object("<</Type /Pages /Kids [3 0 R] /Count 1>>");
    pdf.object("<</Type /Page /Parent 2 0 R /MediaBox [0 0 200 200]>>");
    pdf.endWithTable(1);
    pdf.object("<</Producer (an editor)>>");
    pdf.endWithTable(1, " /Info 4 0 R");
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes("BT ET")));
    pdf.endWithTable(1);
    return pdf.bytes();
}

std::vector<std::byte> compressedPdf(bool predictor) {
    PdfWriter pdf("1.5");
    // Object 1 holds the catalog (2) and the page tree (3).
    (void)pdf.objectStream({"<</Type /Catalog /Pages 3 0 R>>", "<</Type /Pages /Kids [4 0 R] /Count 1>>"});
    pdf.object("<</Type /Page /Parent 3 0 R /MediaBox [0 0 200 200] /Contents 5 0 R>>");
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes("BT /F1 12 Tf (packed) Tj ET")));
    pdf.endWithStream(2, predictor);
    return pdf.bytes();
}

std::vector<std::byte> hybridPdf() {
    PdfWriter pdf("1.5");
    (void)pdf.objectStream({"<</Type /Catalog /Pages 3 0 R>>", "<</Type /Pages /Kids [4 0 R] /Count 1>>"});
    pdf.object("<</Type /Page /Parent 3 0 R /MediaBox [0 0 200 200] /Contents 5 0 R>>");
    pdf.stream("/Filter /FlateDecode", storedZlib(textBytes("BT /F1 12 Tf (hybrid) Tj ET")));
    pdf.endHybrid(2);
    return pdf.bytes();
}

std::vector<std::byte> linearizedPdf() {
    // Written twice: once to learn the offsets, then with them.
    std::uint64_t mainXref = 0;
    std::uint64_t firstXref = 0;
    std::vector<std::uint64_t> offsets(5, 0);
    std::string text;
    for (int pass = 0; pass < 2; ++pass) {
        text = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
        offsets[1] = text.size();
        text += "1 0 obj\n<</Linearized 1 /L " + tenDigits(0) + " /O 3 /E 0 /N 1 /T " + tenDigits(0) +
                " /H [0 0]>>\nendobj\n";
        firstXref = text.size();
        text += "xref\n1 3\n" + tenDigits(offsets[1]) + " 00000 n \n" + tenDigits(offsets[2]) + " 00000 n \n" +
                tenDigits(offsets[3]) + " 00000 n \ntrailer\n<</Size 5 /Root 2 0 R /Prev " + tenDigits(mainXref) +
                ">>\nstartxref\n0\n%%EOF\n";
        offsets[2] = text.size();
        text += "2 0 obj\n<</Type /Catalog /Pages 4 0 R>>\nendobj\n";
        offsets[3] = text.size();
        text += "3 0 obj\n<</Type /Page /Parent 4 0 R /MediaBox [0 0 200 200]>>\nendobj\n";
        offsets[4] = text.size();
        text += "4 0 obj\n<</Type /Pages /Kids [3 0 R] /Count 1>>\nendobj\n";
        mainXref = text.size();
        text += "xref\n0 1\n0000000000 65535 f \n4 1\n" + tenDigits(offsets[4]) + " 00000 n \ntrailer\n<</Size 5>>\n";
        text += "startxref\n" + std::to_string(firstXref) + "\n%%EOF\n";
    }
    return textBytes(text);
}

}  // namespace recovery::test
