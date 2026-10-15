# Document formats (after P20): PDF and OLE2 compound files

Added after P20 at the user's request (2026-10-14). Asked whether the engine recovers `.apk`, `.docx`, `.doc` and
`.pdf` files, the user chose all three proposed changes ("yes solve them"): carving PDF documents, carving OLE2
compound files (the Office 97-2003 documents and their relatives), and naming carved archives by what they hold
(L214). Before, PDFs and Office files came back only while a filesystem entry still named them, unchecked (L213).

Library `recovery_formats`, like the other formats; headers `include/formats/pdf_format.hpp` and
`include/formats/ole2_format.hpp`, registered together by `include/formats/document_formats.hpp`:

```cpp
carving::FormatRegistry registry;
Status added = formats::registerDocumentFormats(registry, validation::deflateDecoder());
```

| Header | Format | Signature | Minimum | Maximum | End detection |
| --- | --- | --- | --- | --- | --- |
| `pdf_format.hpp` | `PdfFormat` | `%PDF-` | 64 | 16 GiB | StructureWalk: the end of the last revision's `%%EOF` line |
| `ole2_format.hpp` | `Ole2Format` | `D0 CF 11 E0 A1 B1 1A E1` | 1,536 | 16 GiB | StructureWalk: the end of the last sector the FAT has in use |

The applications register them after the archive formats: the API, the CLI's scan, the benchmark, the fuzz targets
and the test registries. PDF inflates its Flate streams with the decoder ZIP uses, which moved to
`include/formats/deflate_decoder.hpp` (`formats::IDeflateDecoder`) and gained `decodeZlib`: a zlib stream (RFC 1950),
its header, the DEFLATE data and an Adler-32 that must be right. `validation::deflateDecoder()` is the engine's for
both. The documents built on ZIP (DOCX, XLSX, PPTX, ODT, EPUB, XPS) are carved by the ZIP format
([archives.md](archives.md)) and named by their members ([below](#naming-carved-files-by-their-content-l214)).

## PDF

### The walk

One walk (`src/formats/pdf_format.cpp`) serves end detection and validation, as for every format. It reads a
document front to back as ISO 32000 lays it out: the header (`%PDF-` and a version), then **indirect objects**
(`n g obj` ... `endobj`), cross-reference **tables** with their trailers, `startxref`, and the `%%EOF` that ends a
**revision**. Comments and whitespace are skipped; every value is parsed (arrays and dictionaries nested up to 64
deep), and only the dictionary entries the walk needs are kept (`/Length`, `/Filter`, `/Type`, `/W`, `/Index`,
`/Prev`, `/XRefStm`, `/Root`, `/Encrypt`, `/Linearized`, ...).

- A **stream**'s data is delimited by its `/Length` when that is a number: `endstream` must follow after at most 16
  whitespace bytes. When it does not, `endstream` is looked for within 16 bytes either side (a writer that
  miscounted an end of line): a problem of the document (`Invalid`), not of the walk. A `/Length` that reaches beyond
  the data breaks the walk (`Broken`, not `Truncated`: a damaged length would otherwise make the document reach the
  end of the source). When `/Length` is a reference to another object (written after the stream, as many writers
  do) or missing, the data ends at the first `endstream` within 1 GiB, and the object holding the length, when it
  comes, must agree (a missing `/Length` is a problem).
- An object without `endobj` before the next object, table or `%%EOF` is a problem; anything else where an object,
  a table or `%%EOF` belongs breaks the walk.
- **Revisions**: what follows a `%%EOF` line (after at most 64 more line spaces) continues the document when it
  starts as an update does (`xref`, or `n g obj`). The update is taken at its own `%%EOF` only when the newest
  `startxref`'s chain of sections (`/Prev`, `/XRefStm`) reaches every cross-reference section walked; otherwise the
  document ended at the `%%EOF` before it, and what followed is another file's (an older document's objects left
  behind it). Data that ends inside what may be an update is `Truncated`.
- Linearized documents (the first page's section first, with a `/Prev` pointing forward) and hybrid-reference files
  (a table whose trailer's `/XRefStm` names a cross-reference stream, as Microsoft Office writes them) walk the same.

**Layout** (end detection): `Found` at the end of the last revision's `%%EOF` line, with the line spaces after it;
`Broken` where the walk cannot go on; `Truncated` where the data ends. The detail says what was walked:
`6 objects, 2 streams, 1 revision` (`, linearized`, `, encrypted`).

**Header check** (4 KiB): `%PDF-` and a version (`1.7`, `2.0`), then, after the header line and any comments, an
object (`1 0 obj`) or a table (`xref`). Text that only mentions `%PDF-` is not a document.

### What validation checks

After a complete walk, the whole document:

1. **Cross-references**: `startxref` points at a section, every `/Prev` at a section, every `/XRefStm` at a
   cross-reference stream. Every in-use entry of every table, and every entry of every cross-reference stream
   (inflated, its PNG or TIFF predictor undone, at most 64 MiB decoded each), names an object that starts at that
   offset with that number and generation, or an object stream that holds it (`/N`).
2. **The catalog**: the newest trailer's `/Root` (or the first one down the chain) is an object of the document, or
   may be in one of its object streams.
3. **Lengths** that are references match the data they measure.
4. **Every stream's data**: `FlateDecode` data is inflated and its Adler-32 checked (at most 16 bytes of text may
   follow it); `DCTDecode` data must be a JPEG image that the JPEG format walks to its end and validates
   (`JpegFormat`, [images.md](images.md); at most 16 whitespace bytes may follow its EOI); `ASCIIHexDecode` and
   `ASCII85Decode` data must keep to their alphabets up to their end markers; data without a filter must be text
   (no control character but tab, line feed, form feed and carriage return), in which no file the engine carves
   could lie. A stream's first filter decides.

A document is **Valid** only when every stream was checked, and the detail says so:
`7 objects, 3 streams, 1 revision, every stream checked`.

### What cannot be checked

A stream the engine cannot check leaves the document **unchecked**: another filter (`LZWDecode`, `RunLengthDecode`,
`CCITTFaxDecode`, `JBIG2Decode`, `JPXDecode`), binary data without a filter, a JPEG image followed by more than
padding in its stream, and every stream of an **encrypted** document (`/Encrypt`: ciphertext; its cross-reference
streams are not encrypted and are still checked). So is a document beyond the limits (`PdfFormat::Limits`): more
than 1,048,576 objects, table entries or streams to record, or streams that unpack to more than 4 GiB in all
(cross-reference streams included). Such a document, consistent otherwise, is `NotValidated` with how many streams
were not checked and why the first was not (`N streams were not checked (the document is encrypted)`): its
structural level is `Unsupported` and its condition `UNVERIFIED`. The carver keeps it but never trusts it. More than
4,096 revisions or cross-reference sections break the walk instead (each revision's end follows the chain of
sections, so their number bounds the walk's time).

### Writers' habits it takes

- **Microsoft Print to PDF** (Windows 10 22H2) records every object's offset one byte early, at the end of line
  before the object; readers (qpdf among them) open such files. An offset that points into the whitespace before an
  object, back to the end of a comment before it, names that object (`Lexer::leading`).
- Lines that end in `\r\n` or a lone `\r`; a stream keyword followed by `\r\n` or `\n`.
- **Padding**: when the file's length is known (a named file), whitespace and zeros from the last `%%EOF` to the
  end of the file, at most 1 MiB, are the file's: the detail says `N bytes of padding after %%EOF`. A carve ends
  with the `%%EOF` line.

### Trust

A `Valid` document is trusted: the hits strictly inside it are its own content. A PDF stores its JPEG images as
they are (`DCTDecode`), each a complete JPEG file, and validation checked each of them: they are not carved as files
of their own, as the user decided for the images stored in verified archives (L221). A damaged or unchecked document
is never trusted, and the images in it are carved as before. `foundEndMeansContiguous` is set: each element is found
where the one before it ends, so a document whose last `%%EOF` was reached lies in one piece, and a bad stream in it
is damage, not fragmentation. Fragment reconstruction (P13) then takes no seed from it: it stays one `CORRUPTED`
candidate with every byte. A document whose walk breaks is a seed as for any format.

## OLE2 compound files

[MS-CFB] "structured storage": the Office 97-2003 documents (`.doc`, `.xls`, `.ppt`), Outlook messages (`.msg`),
Windows Installer packages, patches and transforms (`.msi`, `.msp`, `.mst`), Visio and Publisher files, thumbnail
caches (`Thumbs.db`), and whatever else a program writes with Windows' structured storage.

### The walk and the end

A compound file is a small file system in sectors of 512 bytes (version 3) or 4,096 (version 4): a header; a FAT
that chains the sectors, whose own sectors the DIFAT lists (109 in the header, the rest in DIFAT sectors); a
directory of storages and streams; and a mini stream that holds the streams under 4,096 bytes in 64-byte mini
sectors, which the mini FAT chains. The walk (`src/formats/ole2_format.cpp`) reads the header, the DIFAT and every
FAT sector; the file ends with the **last sector the FAT has in use** (`Found`). A FAT entry that is neither a
sector the FAT covers nor a marker breaks the walk; data that ends before the last sector in use is `Truncated`.
The directory follows from its chain, and its first entry must be the root. The detail says what was walked:
`N sectors of 512 bytes, M directory entries`.

**Header check** (512 bytes): the signature, little-endian byte order, version 3 with 512-byte sectors or 4 with
4,096-byte ones, 64-byte mini sectors, the mini stream cutoff of 4,096, no directory sector count in version 3, and
at least one FAT sector.

### What validation checks

The FAT's and the DIFAT's sectors are marked as such; every directory entry has a known type, a name length of 2 to
64 bytes and links inside the directory, and there is one root; the trees from the root reach no entry twice and no
unallocated one; the mini stream's and the mini FAT's chains have their recorded lengths; the chain of every stream
the trees reach ends after exactly the sectors (or mini sectors) its size needs, and a chain longer than its table
has sectors runs in a loop (a version 4 size has 64 bits: it never bounds the walk); no sector belongs to two chains
(FAT, DIFAT, directory, mini stream, mini FAT, streams). The detail ends `5 streams, every chain consistent`. Limits
(`Ole2Format::Limits`): a FAT of more than 16,777,216 entries is not read (`Broken`); a directory of more than
1,048,576 entries is not checked beyond (`NotValidated`).

**Free sectors**: a file's FAT may cover sectors after its last one in use (writers keep the sectors a stream gave
up, and grow a file ahead of its streams). When the file's length is known (a named file), those sectors are the
file's, the last one possibly cut short; bytes beyond what the FAT covers are not (`Invalid`). A carve ends at the
last sector in use, without them: readers do not need them, and Windows' own implementation drops them itself.

### Never trusted

Nothing in a compound file checks the data of its streams: no checksum, and no length that damaged data would
break. A document whose sectors another file has taken since still validates. So the format does not trust its
files (`FormatDescriptor::trustValidFiles = false`, added for it):

- the carver carves the hits inside a compound file as if it were not there: the pictures in a Word document's
  `Data` stream, the JPEG thumbnails in `Thumbs.db`;
- fragment reconstruction (P13) does not reconstruct compound files, and a valid one claims its clusters only as an
  unchecked file does: any layout that keeps the header, the FAT and the directory in place validates, whatever data
  it places, so a reconstruction would only choose among guesses. A deleted document stays what the filesystem gives
  (on FAT, its clusters from the first one on; `CORRUPTED` when they do not validate), an unnamed one what carving
  gives. In the corpus scenario `documents`, a Word document deleted in two pieces came back as four `AMBIGUOUS`
  alternatives while it was reconstructed, none of them the original; it is now one `CORRUPTED` candidate, the bytes
  the engine gave before documents were carved.

### Named by its streams

A carved compound file is named by what its root holds: `doc` (`WordDocument`), `xls` (`Workbook`, `Book`), `ppt`
(`PowerPoint Document`), `msg` (`__substg1.0_` streams, `__properties_version1.0`), `vsd` (`VisioDocument`), `pub`
(`Quill`), `db` (`Catalog`: a thumbnail cache), and `msi`, `msp`, `mst` by the root's class. Any other is `ole`, an
encrypted Office 2007 or later document among them (`EncryptionInfo` and `EncryptedPackage` name no application).

## Naming carved files by their content (L214)

`EndDetection::extension` (new) lets a format say what a file is beyond its own extension, and the carver and
fragment reconstruction name the file by it. Besides the compound files above, the ZIP format gives it from the
members it walked (also before a break: a DOCX names itself in its first members):

| Members | Extension |
| --- | --- |
| a first member `mimetype`, stored, holding `application/vnd.oasis.opendocument.text` (`.spreadsheet`, `.presentation`, `.graphics`) or `application/epub+zip` | `odt` (`ods`, `odp`, `odg`), `epub` |
| `[Content_Types].xml` and members under `word/`, `xl/` or `ppt/`; with a `vbaProject.bin` there (macros) | `docx`, `xlsx`, `pptx`; `docm`, `xlsm`, `pptm` |
| `[Content_Types].xml` and members under `visio/`, or a `FixedDocumentSequence.fdseq` (`FixedDocSeq.fdseq`) | `vsdx`, `xps` |
| `AndroidManifest.xml` with `classes.dex` or `resources.arsc`, or an APK signing block | `apk` |
| `META-INF/MANIFEST.MF`, or a first member in `META-INF/` or a class file (the JARs of IntelliJ and Android Studio often have no manifest) | `jar` |

Anything else is `zip`: an archive of a project's folder starts with the folder, whatever classes it holds.
Templates and slide shows are named as the documents they are made like (`.dotx` as `docx`, `.ppsx` as `pptx`), and
OpenXPS as XPS: their members are named the same.

**Kinds**: a file's kind follows from its format and its extension (`metadata::kindOfFile`, `mediaTypeOfFile`), the
extension being its name's or, for a carved file, what its content gave. A new kind, `document`
(`metadata::MediaKind::Document`, `api::MediaKind::Document`, the CLI's `recover --kind document`), covers PDF, the
Office, OpenDocument, EPUB and XPS files on ZIP, and the Office files and Outlook messages in compound files (also a
compound file that names no application). `archive` stays for ZIP, JAR and APK files and the installer packages
(MSI, MSP, MST); a thumbnail cache is `other`. Media types: `application/pdf`, `application/msword`,
`application/vnd.openxmlformats-officedocument.wordprocessingml.document` and so on. Documents have no media
metadata and no preview, like archives.

There are no extension aliases: a file named `.doc` that holds RTF, or `.xls` that holds CSV (as programs save them
under those names), is no compound file. No header check takes it and no format has its extension, so it stays what
it was before: kind `other`, `UNVERIFIED`, never `CORRUPTED`.

## Validation levels

The structural level does everything that can be checked, as for archives. The media level is `NotApplicable` for
both formats (`src/validation/document_media.cpp`): a document codes no media of its own (the structural level has
inflated every Flate stream and checked every JPEG image of a PDF), and a compound file holds no checksums of its
streams. No Windows decoder takes documents, so the playability level is `Unsupported` when it is asked for.

## Tests

- `tests/formats/pdf_format_test.cpp`: documents of every layout intact (tables, cross-reference and object
  streams, hybrid files, linearized ones, updates); the detail; `\r\n` lines; lengths in objects of their own; no
  prefix complete; an earlier revision is a document of its own; what follows the last revision is not the
  document's; padding; damaged streams, tables and trailers; offsets one byte early; hybrid files checked through
  both sections; object streams; `/Index` counts that add up past 64 bits; an encrypted document's cross-reference
  stream; streams the engine cannot check; header checks; the limits (revisions and sections too); carving with the
  image formats (the JPEG of a verified document is part of it, of an unchecked or damaged one carved); documents one
  after another; fuzzing.
- `tests/formats/ole2_format_test.cpp`: files of every kind intact and named by their streams; a FAT that needs
  DIFAT sectors; the end at the last sector in use; free sectors; no prefix complete; header checks; damaged FATs,
  directories and chains, a chain in a loop; the limits; carving and naming; the files inside a compound file carved;
  fuzzing.
- `tests/formats/document_reference_test.cpp`: `DocumentSamplesTest` (always) recognises, validates, names and carves
  back the documents that real writers made (below); `DocumentReferenceTest` does the same for folders of documents
  made elsewhere (`RECOVERY_DOCUMENT_REFERENCE_DIR`, [../testing/testing.md](../testing/testing.md));
  `DocumentBuilderExport` writes the builders' files for other readers (`RECOVERY_DOCUMENT_EXPORT_DIR`).
- `tests/support/document_samples.cpp`: 11 documents that Microsoft Edge (Skia/PDF), Microsoft Print to PDF, the
  XPS Document Writer, Windows' structured storage (`StgCreateStorageEx`) and Windows Installer wrote
  (`tests/reference/make_document_samples.ps1`), and qpdf 11.9.0 variants of them (`make_document_samples.sh`: AES-256,
  linearized, object streams), embedded by `embed_document_samples.py`. `tests/support/pdf_builders.cpp` and
  `ole2_builders.cpp` build documents for the tests; `tests/reference/check_document_builder_files.sh` has qpdf
  (`--check`) and olefile read the builders' files.
- `tests/recovery/fragment_recovery_test.cpp`: a reconstructed archive is named by its members; a compound file that
  validates keeps no other file from its clusters; compound files in pieces are left as they were found.
- `tests/metadata/media_metadata_test.cpp`, `tests/cli/cli_commands_test.cpp`, `tests/api/api_scan_test.cpp`: the
  kinds and media types, `--kind document`, documents and archives told apart.
- The corpus scenario `documents`: named, deleted and unnamed PDFs, compound files and documents on ZIP on a FAT32
  card, files real writers made, text named `.doc` and `.xls`, a JPEG that is part of a verified PDF
  (`inside-document`: no candidate of its own, and inside a recovered `Complete` PDF byte for byte), and images in
  a compound file and in a damaged PDF, which are carved.

## Checked on real files

- Every embedded sample: the PDFs of Edge, Microsoft Print to PDF and qpdf `Valid` (the AES-256 one `NotValidated`),
  the compound files of structured storage and Windows Installer `Valid` and named, the XPS file `Valid` and named.
- `DocumentReferenceTest` on the development computer (2026-10-15; program folders only: `C:\Windows\Installer`,
  `D:\VS\Packages`, Android Studio, IntelliJ IDEA, the JDKs, Git, VLC): 1,828 files, every one carved to its full
  length and none called damaged. 152 Windows Installer packages `Valid` and named `msi`; 1,676 files built on ZIP
  `Valid` (every member's CRC-32 right): 1,279 JARs named `jar`, 381 JARs without a manifest whose first member is
  neither in `META-INF/` nor a class file named `zip` (L239), an APK whose signing block `apksigner` aligned named
  `apk` (it was cut short before L240's fix), 15 ZIPs. The run found no PDF or Office document outside personal
  folders, which it did not read; the samples above stand for them.
- The builders' files pass `qpdf --check` and olefile 0.46.
