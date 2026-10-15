"""Writes tests/support/zip_samples.cpp: small ZIP archives that Python's
zipfile wrote (DEFLATE with Huffman codes, stored members, empty files and
folders, data descriptors from a stream that cannot seek, LZMA and bzip2
members, an archive comment), and archives reworked here into what other
writers make: ZIP64 end records, an APK signing block before the central
directory, a DOCX's first members. Every archive is valid; the tests break
them byte by byte. Samples are only ever appended: the corpus baselines
hold the hashes of the earlier ones (Windows' Python 3.14 reproduces them).

Usage: python3 make_zip_samples.py <output .cpp>
"""

import io
import random
import struct
import sys
import zipfile

WHEN = (2020, 6, 8, 14, 9, 10)


class Unseekable(io.RawIOBase):
    """A stream zipfile cannot seek: it writes data descriptors."""

    def __init__(self, target):
        self.target = target

    def writable(self):
        return True

    def write(self, data):
        return self.target.write(data)


def entry(name, method):
    info = zipfile.ZipInfo(name, WHEN)
    info.compress_type = method
    info.external_attr = (0o40755 if name.endswith("/") else 0o100644) << 16
    return info


def archive(members, comment=b"", stream=False, zip64=False):
    out = io.BytesIO()
    with zipfile.ZipFile(Unseekable(out) if stream else out, "w") as z:
        z.comment = comment
        for name, data, method in members:
            if zip64:
                with z.open(entry(name, method), "w", force_zip64=True) as f:
                    f.write(data)
            else:
                z.writestr(entry(name, method), data)
    return out.getvalue()


def end_record(data):
    at = data.rindex(b"PK\x05\x06")
    return at, list(struct.unpack("<4sHHHHIIH", data[at:at + 22]))


def with_zip64_end(data):
    """The same archive with a ZIP64 end record and locator, as writers of
    archives beyond 4 GiB or 65535 members make them."""
    at, fields = end_record(data)
    _, disk, cd_disk, on_disk, total, cd_size, cd_offset, comment_length = fields
    record = struct.pack("<4sQHHIIQQQQ", b"PK\x06\x06", 44, 45, 45, disk, cd_disk, on_disk, total, cd_size, cd_offset)
    locator = struct.pack("<4sIQI", b"PK\x06\x07", 0, at, 1)
    end = struct.pack("<4sHHHHIIH", b"PK\x05\x06", 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, comment_length)
    return data[:at] + record + locator + end + data[at + 22:]


def with_signing_block(data):
    """An APK's signing block between the last member and the central directory."""
    at, fields = end_record(data)
    cd_offset = fields[6]
    pairs = struct.pack("<QI", 4 + 32, 0x7109871A) + bytes(range(32))
    size = len(pairs) + 8 + 16
    block = struct.pack("<Q", size) + pairs + struct.pack("<Q", size) + b"APK Sig Block 42"
    fields[6] = cd_offset + len(block)
    end = struct.pack("<4sHHHHIIH", *fields)
    return data[:cd_offset] + block + data[cd_offset:at] + end + data[at + 22:]


def text(n, seed):
    rng = random.Random(seed)
    words = ["recovery", "cluster", "sector", "archive", "member", "deflate", "huffman", "window", "zip"]
    out = []
    while sum(len(w) + 1 for w in out) < n:
        out.append(rng.choice(words))
    return (" ".join(out))[:n].encode()


def noise(n, seed):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(n))


D = zipfile.ZIP_DEFLATED
S = zipfile.ZIP_STORED
MIXED = [
    ("docs/", b"", S),
    ("docs/readme.txt", text(2000, 1), D),
    ("data/noise.bin", noise(3000, 2), D),
    ("data/empty.txt", b"", D),
    ("data/stored.bin", noise(700, 3), S),
    ("logs/long.log", text(70000, 4), D),
]

VECTORS = [
    ("mixed", archive(MIXED)),
    ("stored", archive([("a.bin", noise(500, 5), S), ("b.bin", noise(900, 6), S)])),
    ("streamed", archive(MIXED, stream=True)),
    ("zip64_members", archive(MIXED[1:4], zip64=True)),
    ("zip64_end", with_zip64_end(archive(MIXED[1:3]))),
    ("comment", archive(MIXED[1:2], comment=b"made for the recovery engine's tests")),
    ("lzma", archive([("plain.txt", text(1500, 7), D), ("packed.txt", text(1500, 8), zipfile.ZIP_LZMA)])),
    ("bzip2", archive([("packed.txt", text(1500, 9), zipfile.ZIP_BZIP2)])),
    ("apk", with_signing_block(archive([("AndroidManifest.xml", text(800, 10), D),
                                         ("classes.dex", noise(1200, 11), D),
                                         ("res/icon.png", noise(400, 12), S)]))),
    ("docx", archive([("[Content_Types].xml", text(900, 13), D), ("_rels/.rels", text(400, 14), D),
                      ("word/document.xml", text(5000, 15), D)])),
    # The other formats built on ZIP, told apart by their members' names
    # (and an OpenDocument or EPUB file's first member, "mimetype", stored).
    ("xlsx", archive([("[Content_Types].xml", text(700, 16), D), ("_rels/.rels", text(300, 17), D),
                      ("xl/workbook.xml", text(1200, 18), D), ("xl/worksheets/sheet1.xml", text(3000, 19), D)])),
    ("pptm", archive([("[Content_Types].xml", text(700, 20), D), ("ppt/presentation.xml", text(1500, 21), D),
                      ("ppt/vbaProject.bin", noise(600, 22), D)])),
    ("odt", archive([("mimetype", b"application/vnd.oasis.opendocument.text", S),
                     ("content.xml", text(2500, 23), D), ("META-INF/manifest.xml", text(400, 24), D)])),
    ("epub", archive([("mimetype", b"application/epub+zip", S), ("META-INF/container.xml", text(300, 25), D),
                      ("OEBPS/chapter1.xhtml", text(4000, 26), D)])),
    ("jar", archive([("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\r\nCreated-By: tests\r\n\r\n", D),
                     ("org/example/App.class", noise(900, 27), D)])),
]


def main():
    out = [
        "// Generated by tests/reference/make_zip_samples.py: do not edit.",
        "",
        '#include "support/zip_samples.hpp"',
        "",
        "#include <array>",
        "",
        "namespace recovery::test::zip_samples {",
        "",
        "namespace {",
        "",
    ]
    for index, (name, data) in enumerate(VECTORS):
        zipfile.ZipFile(io.BytesIO(data)).testzip()
        out.append("// %s: %d bytes" % (name, len(data)))
        out.append("constexpr std::array<std::uint8_t, %d> kSample%d = {" % (len(data), index))
        for start in range(0, len(data), 24):
            out.append("    " + ", ".join("0x%02X" % b for b in data[start:start + 24]) + ",")
        out.append("};")
        out.append("")
    out.append("}  // namespace")
    out.append("")
    out.append("const std::vector<Sample>& all() {")
    out.append("    static const std::vector<Sample> samples = {")
    for index, (name, _) in enumerate(VECTORS):
        out.append('        {"%s", kSample%d},' % (name, index))
    out.append("    };")
    out.append("    return samples;")
    out.append("}")
    out.append("")
    out.append("const Sample& named(std::string_view name) {")
    out.append("    for (const Sample& sample : all()) {")
    out.append("        if (sample.name == name) {")
    out.append("            return sample;")
    out.append("        }")
    out.append("    }")
    out.append("    return all().front();")
    out.append("}")
    out.append("")
    out.append("}  // namespace recovery::test::zip_samples")
    with open(sys.argv[1], "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
