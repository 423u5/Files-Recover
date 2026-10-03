#include "support/ntfs_builder.hpp"

#include "recovery/byte_order.hpp"
#include "support/fat32_builder.hpp"  // utf8ToUtf16

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace recovery::test {

namespace {

// Attribute types and flags, written out here independently of the engine.
constexpr std::uint32_t kStandardInformation = 0x10;
constexpr std::uint32_t kFileName = 0x30;
constexpr std::uint32_t kVolumeName = 0x60;
constexpr std::uint32_t kVolumeInformation = 0x70;
constexpr std::uint32_t kData = 0x80;
constexpr std::uint32_t kIndexRoot = 0x90;
constexpr std::uint32_t kIndexAllocation = 0xA0;
constexpr std::uint32_t kBitmap = 0xB0;
constexpr std::uint32_t kEnd = 0xFFFFFFFF;
constexpr std::uint16_t kInUse = 0x0001;
constexpr std::uint16_t kDirectory = 0x0002;
constexpr std::uint16_t kViewIndex = 0x0008;
constexpr auto kInUseDirectory = static_cast<std::uint16_t>(kInUse | kDirectory);
constexpr auto kInUseViewIndex = static_cast<std::uint16_t>(kInUse | kViewIndex);
constexpr std::uint16_t kAttrCompressed = 0x0001;
constexpr std::uint16_t kAttrSparse = 0x8000;
constexpr std::uint32_t kFileArchive = 0x20;
constexpr std::uint32_t kHiddenSystem = 0x06;
constexpr std::uint32_t kFileNameIndexPresent = 0x10000000;  // $FILE_NAME flag of directories
constexpr std::uint8_t kWin32 = 1;
constexpr std::uint8_t kDos = 2;
constexpr std::uint8_t kWin32AndDos = 3;
constexpr std::uint16_t kUpdateSequence = 0x0003;
constexpr std::size_t kBlock = 512;
constexpr std::uint64_t kLogFileBytes = 64 * 1024;
constexpr std::uint64_t kAttrDefBytes = 2560;
constexpr std::uint64_t kUpcaseBytes = 65536 * 2;
constexpr std::uint64_t kBootBytes = 8192;

std::size_t align8(std::size_t value) {
    return (value + 7) & ~std::size_t{7};
}

std::uint64_t clustersFor(std::uint64_t bytes, std::uint32_t clusterSize) {
    return (bytes + clusterSize - 1) / clusterSize;
}

std::uint64_t toFileTime(std::chrono::sys_time<std::chrono::milliseconds> time) {
    return static_cast<std::uint64_t>(time.time_since_epoch().count() + 11'644'473'600'000LL) * 10'000ULL;
}

constexpr std::chrono::sys_seconds kStamp = std::chrono::sys_days{std::chrono::year{2024} / 5 / 17} +
                                            std::chrono::hours{11} + std::chrono::minutes{45} +
                                            std::chrono::seconds{30};

// Bytes needed for `value` as a little-endian two's complement number.
std::size_t signedBytes(std::int64_t value) {
    std::size_t bytes = 1;
    while (bytes < 8) {
        const std::int64_t limit = std::int64_t{1} << (8 * bytes - 1);
        if (value >= -limit && value < limit) {
            break;
        }
        ++bytes;
    }
    return bytes;
}

// Mapping pairs: a header byte (offset size << 4 | length size), the length,
// then the cluster offset relative to the previous stored run; no offset for
// a hole. Both are signed and as short as possible, as Windows writes them.
std::vector<std::byte> encodeRuns(const std::vector<NtfsRunSpec>& runs) {
    std::vector<std::byte> out;
    std::int64_t previous = 0;
    for (const NtfsRunSpec& run : runs) {
        const auto length = static_cast<std::int64_t>(run.length);
        const std::size_t lengthBytes = signedBytes(length);
        std::size_t offsetBytes = 0;
        std::int64_t delta = 0;
        if (run.lcn) {
            delta = static_cast<std::int64_t>(*run.lcn) - previous;
            offsetBytes = signedBytes(delta);
            previous = static_cast<std::int64_t>(*run.lcn);
        }
        out.push_back(static_cast<std::byte>((offsetBytes << 4) | lengthBytes));
        for (std::size_t i = 0; i < lengthBytes; ++i) {
            out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(length) >> (8 * i)) & 0xFF));
        }
        for (std::size_t i = 0; i < offsetBytes; ++i) {
            out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(delta) >> (8 * i)) & 0xFF));
        }
    }
    out.push_back(std::byte{0});
    return out;
}

std::vector<NtfsRunSpec> runsOf(const std::vector<std::uint64_t>& clusters) {
    std::vector<NtfsRunSpec> runs;
    for (const std::uint64_t cluster : clusters) {
        if (!runs.empty() && *runs.back().lcn + runs.back().length == cluster) {
            ++runs.back().length;
        } else {
            runs.push_back(NtfsRunSpec{cluster, 1});
        }
    }
    return runs;
}

std::vector<std::byte> residentAttribute(std::uint32_t type, std::u16string_view name, std::span<const std::byte> value,
                                         std::uint8_t indexed = 0) {
    const std::size_t nameOffset = 0x18;
    const std::size_t valueOffset = align8(nameOffset + name.size() * 2);
    const std::size_t length = align8(valueOffset + value.size());
    std::vector<std::byte> a(length);
    storeLe32(a, 0, type);
    storeLe32(a, 4, static_cast<std::uint32_t>(length));
    a[9] = static_cast<std::byte>(name.size());
    storeLe16(a, 10, static_cast<std::uint16_t>(nameOffset));
    storeLe32(a, 16, static_cast<std::uint32_t>(value.size()));
    storeLe16(a, 20, static_cast<std::uint16_t>(valueOffset));
    a[22] = static_cast<std::byte>(indexed);
    for (std::size_t i = 0; i < name.size(); ++i) {
        storeLe16(a, nameOffset + 2 * i, name[i]);
    }
    std::copy(value.begin(), value.end(), a.begin() + static_cast<std::ptrdiff_t>(valueOffset));
    return a;
}

std::vector<std::byte> nonResidentAttribute(std::uint32_t type, std::u16string_view name,
                                            const std::vector<NtfsRunSpec>& runs, std::uint64_t realSize,
                                            std::uint64_t initializedSize, std::uint16_t flags,
                                            std::uint32_t clusterSize) {
    std::uint64_t vcns = 0;
    std::uint64_t stored = 0;
    for (const NtfsRunSpec& run : runs) {
        vcns += run.length;
        stored += run.lcn ? run.length : 0;
    }
    // Compressed and sparse attributes carry an extra "compressed size" field.
    const std::size_t header = (flags & (kAttrCompressed | kAttrSparse)) != 0 ? 0x48 : 0x40;
    const std::size_t runsOffset = align8(header + name.size() * 2);
    const std::vector<std::byte> encoded = encodeRuns(runs);
    const std::size_t length = align8(runsOffset + encoded.size());
    std::vector<std::byte> a(length);
    storeLe32(a, 0, type);
    storeLe32(a, 4, static_cast<std::uint32_t>(length));
    a[8] = std::byte{1};
    a[9] = static_cast<std::byte>(name.size());
    storeLe16(a, 10, static_cast<std::uint16_t>(header));
    storeLe16(a, 12, flags);
    storeLe64(a, 16, 0);
    storeLe64(a, 24, vcns == 0 ? ~0ULL : vcns - 1);
    storeLe16(a, 32, static_cast<std::uint16_t>(runsOffset));
    storeLe16(a, 34, (flags & kAttrCompressed) != 0 ? 4 : 0);
    storeLe64(a, 40, vcns * clusterSize);
    storeLe64(a, 48, realSize);
    storeLe64(a, 56, initializedSize);
    if (header == 0x48) {
        storeLe64(a, 64, stored * clusterSize);
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        storeLe16(a, header + 2 * i, name[i]);
    }
    std::copy(encoded.begin(), encoded.end(), a.begin() + static_cast<std::ptrdiff_t>(runsOffset));
    return a;
}

std::vector<std::byte> standardInformation(std::uint32_t attributes) {
    std::vector<std::byte> v(72);
    storeLe64(v, 0, NtfsImageBuilder::createdTime());
    storeLe64(v, 8, NtfsImageBuilder::modifiedTime());
    storeLe64(v, 16, NtfsImageBuilder::modifiedTime());
    storeLe64(v, 24, NtfsImageBuilder::modifiedTime());
    storeLe32(v, 32, attributes);
    return v;
}

std::vector<std::byte> fileName(std::uint64_t parentReference, std::u16string_view name, std::uint8_t nameSpace,
                                std::uint64_t allocatedSize, std::uint64_t realSize, std::uint32_t flags) {
    std::vector<std::byte> v(0x42 + name.size() * 2);
    storeLe64(v, 0, parentReference);
    storeLe64(v, 8, NtfsImageBuilder::createdTime());
    storeLe64(v, 16, NtfsImageBuilder::modifiedTime());
    storeLe64(v, 24, NtfsImageBuilder::modifiedTime());
    storeLe64(v, 32, NtfsImageBuilder::modifiedTime());
    storeLe64(v, 40, allocatedSize);
    storeLe64(v, 48, realSize);
    storeLe32(v, 56, flags);
    v[64] = static_cast<std::byte>(name.size());
    v[65] = static_cast<std::byte>(nameSpace);
    for (std::size_t i = 0; i < name.size(); ++i) {
        storeLe16(v, 0x42 + 2 * i, name[i]);
    }
    return v;
}

// A $I30 index kept in the record: the given entries (file reference and
// $FILE_NAME value, already in collation order), then the end entry.
std::vector<std::byte> indexRoot(std::uint32_t indexRecordSize, std::uint32_t clusterSize,
                                 const std::vector<std::pair<std::uint64_t, std::vector<std::byte>>>& entries = {}) {
    std::vector<std::byte> v(32);
    storeLe32(v, 0, kFileName);  // indexed attribute
    storeLe32(v, 4, 1);          // collation: file names
    storeLe32(v, 8, indexRecordSize);
    v[12] = static_cast<std::byte>(indexRecordSize >= clusterSize
                                       ? indexRecordSize / clusterSize
                                       : 256U - static_cast<unsigned>(std::countr_zero(indexRecordSize)));
    for (const auto& [reference, key] : entries) {
        std::vector<std::byte> entry(align8(16 + key.size()));
        storeLe64(entry, 0, reference);
        storeLe16(entry, 8, static_cast<std::uint16_t>(entry.size()));
        storeLe16(entry, 10, static_cast<std::uint16_t>(key.size()));
        std::copy(key.begin(), key.end(), entry.begin() + 16);
        v.insert(v.end(), entry.begin(), entry.end());
    }
    std::vector<std::byte> end(16);
    storeLe16(end, 8, 16);  // length
    storeLe16(end, 12, 2);  // last-entry flag
    v.insert(v.end(), end.begin(), end.end());
    // The node header (at 16) counts from itself: entries start right after it.
    const auto length = static_cast<std::uint32_t>(v.size() - 16);
    storeLe32(v, 16, 16);
    storeLe32(v, 20, length);
    storeLe32(v, 24, length);
    return v;
}

std::vector<std::byte> emptyIndexRoot(std::uint32_t indexRecordSize, std::uint32_t clusterSize) {
    return indexRoot(indexRecordSize, clusterSize);
}

// Valid 8.3 names in upper case need no separate DOS name.
bool isShortName(std::u16string_view name) {
    const std::size_t dot = name.find(u'.');
    const std::u16string_view base = name.substr(0, dot);
    const std::u16string_view extension = dot == std::u16string_view::npos ? std::u16string_view{} : name.substr(dot + 1);
    const auto valid = [](std::u16string_view part) {
        return std::all_of(part.begin(), part.end(), [](char16_t c) {
            return (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_' || c == u'-' || c == u'~';
        });
    };
    return !base.empty() && base.size() <= 8 && extension.size() <= 3 &&
           extension.find(u'.') == std::u16string_view::npos && valid(base) && valid(extension);
}

}  // namespace

std::uint64_t NtfsImageBuilder::createdTime() {
    return toFileTime(kStamp + std::chrono::milliseconds{500});
}

std::uint64_t NtfsImageBuilder::modifiedTime() {
    return toFileTime(kStamp);
}

NtfsImageBuilder::NtfsImageBuilder(NtfsBuilderOptions options)
    : options_(std::move(options)), clusterSize_(options_.bytesPerSector * options_.sectorsPerCluster) {
    if (options_.recordSize % kBlock != 0 || !std::has_single_bit(options_.recordSize) ||
        options_.mftRecords < kFirstUserRecord || !std::has_single_bit(options_.sectorsPerCluster)) {
        throw std::runtime_error("unsupported NTFS builder options");
    }
    const std::uint64_t totalSectors = options_.clusterCount * options_.sectorsPerCluster;
    image_.assign(static_cast<std::size_t>((totalSectors + 1) * options_.bytesPerSector), std::byte{0});
    allocated_.assign(static_cast<std::size_t>(options_.clusterCount), false);
    records_.resize(options_.mftRecords);

    // Layout: $Boot, the MFT, $LogFile, $AttrDef, $Bitmap, the MFT bitmap and
    // $UpCase from cluster 0; $MFTMirr in the middle of the volume.
    const std::vector<std::uint64_t> boot = allocateRun(std::max<std::uint64_t>(1, kBootBytes / clusterSize_));
    const std::uint64_t mftBytes = static_cast<std::uint64_t>(options_.mftRecords) * options_.recordSize;
    const std::uint64_t mftClusters = clustersFor(mftBytes, clusterSize_);
    const std::uint64_t firstPart = options_.fragmentedMft ? std::max<std::uint64_t>(1, mftClusters / 2) : mftClusters;
    const std::vector<std::uint64_t> mftFirst = allocateRun(firstPart);
    mftRuns_.push_back(Run{mftFirst.front(), firstPart});
    const std::vector<std::uint64_t> log = allocateRun(clustersFor(kLogFileBytes, clusterSize_));
    const std::vector<std::uint64_t> attrDef = allocateRun(clustersFor(kAttrDefBytes, clusterSize_));
    bitmapBytes_ = align8(static_cast<std::size_t>((options_.clusterCount + 7) / 8));
    const std::vector<std::uint64_t> bitmap = allocateRun(clustersFor(bitmapBytes_, clusterSize_));
    bitmapCluster_ = bitmap.front();
    const std::uint64_t mftBitmapBytes = align8((options_.mftRecords + 7) / 8);
    const std::vector<std::uint64_t> mftBitmap = allocateRun(clustersFor(mftBitmapBytes, clusterSize_));
    mftBitmapCluster_ = mftBitmap.front();
    const std::vector<std::uint64_t> upcase = allocateRun(clustersFor(kUpcaseBytes, clusterSize_));
    if (options_.fragmentedMft) {
        skipClusters(3);
        const std::vector<std::uint64_t> mftSecond = allocateRun(mftClusters - firstPart);
        mftRuns_.push_back(Run{mftSecond.front(), mftClusters - firstPart});
    }
    const std::uint64_t mirrorClusters = clustersFor(4ULL * options_.recordSize, clusterSize_);
    mirrorCluster_ = options_.clusterCount / 2;
    std::vector<std::uint64_t> mirror;
    for (std::uint64_t i = 0; i < mirrorClusters; ++i) {
        mirror.push_back(mirrorCluster_ + i);
    }
    claim(mirror);

    std::fill_n(image_.begin() + static_cast<std::ptrdiff_t>(clusterOffset(log.front())), kLogFileBytes,
                std::byte{0xFF});  // an empty (clean) log
    std::vector<std::byte> table(kUpcaseBytes);
    for (std::uint32_t c = 0; c < 65536; ++c) {
        std::uint32_t upper = c;
        if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) {
            upper = c - 0x20;
        } else if (c == 0xFF) {
            upper = 0x178;
        }
        storeLe16(table, 2 * c, static_cast<std::uint16_t>(upper));
    }
    writeData(upcase, table);

    const auto data = [&](const std::vector<std::uint64_t>& clusters, std::uint64_t bytes, std::u16string_view name = {}) {
        return Attr{kData, std::u16string(name),
                    nonResidentAttribute(kData, name, runsOf(clusters), bytes, bytes, 0, clusterSize_)};
    };
    const auto emptyData = [](std::u16string_view name = {}) {
        return Attr{kData, std::u16string(name), residentAttribute(kData, name, {})};
    };
    std::vector<std::uint64_t> mftClustersList;
    for (const Run& run : mftRuns_) {
        for (std::uint64_t i = 0; i < run.count; ++i) {
            mftClustersList.push_back(run.first + i);
        }
    }

    for (std::uint64_t n = 0; n < records_.size(); ++n) {
        records_[n].sequence = 1;
    }
    setSystemRecord(0, "$MFT", kInUse,
                    {data(mftClustersList, mftBytes),
                     Attr{kBitmap, {}, nonResidentAttribute(kBitmap, {}, runsOf(mftBitmap), mftBitmapBytes,
                                                            mftBitmapBytes, 0, clusterSize_)}});
    setSystemRecord(1, "$MFTMirr", kInUse, {data(mirror, 4ULL * options_.recordSize)});
    setSystemRecord(2, "$LogFile", kInUse, {data(log, kLogFileBytes)});
    {
        const std::u16string label = utf8ToUtf16(options_.label);
        std::vector<std::byte> labelBytes(label.size() * 2);
        for (std::size_t i = 0; i < label.size(); ++i) {
            storeLe16(labelBytes, 2 * i, label[i]);
        }
        std::vector<std::byte> volumeInformation(12);
        volumeInformation[8] = std::byte{3};
        volumeInformation[9] = std::byte{1};
        setSystemRecord(3, "$Volume", kInUse,
                        {Attr{kVolumeName, {}, residentAttribute(kVolumeName, {}, labelBytes)},
                         Attr{kVolumeInformation, {}, residentAttribute(kVolumeInformation, {}, volumeInformation)},
                         emptyData()});
    }
    setSystemRecord(4, "$AttrDef", kInUse, {data(attrDef, kAttrDefBytes)});
    {
        // The root index lists $Secure only: ntfs-3g looks it up by name when
        // it mounts a volume, and the engine does not read indexes yet.
        const std::uint64_t rootReference = root() | (static_cast<std::uint64_t>(root()) << 48);
        const std::uint64_t secureReference = 9 | (9ULL << 48);
        const std::vector<std::byte> secureName =
            fileName(rootReference, u"$Secure", kWin32AndDos, 0, 0, kHiddenSystem);
        setSystemRecord(5, ".", kInUseDirectory,
                        {Attr{kIndexRoot, u"$I30",
                              residentAttribute(kIndexRoot, u"$I30",
                                                indexRoot(options_.indexRecordSize, clusterSize_,
                                                          {{secureReference, secureName}}))}});
    }
    setSystemRecord(6, "$Bitmap", kInUse, {data(bitmap, bitmapBytes_)});
    setSystemRecord(7, "$Boot", kInUse, {data(boot, boot.size() * clusterSize_)});
    setSystemRecord(8, "$BadClus", kInUse, {emptyData()});
    rebuildBadClusters();
    setSystemRecord(9, "$Secure", kInUseViewIndex, {emptyData(u"$SDS")});
    setSystemRecord(10, "$UpCase", kInUse, {data(upcase, kUpcaseBytes)});
    setSystemRecord(11, "$Extend", kInUseDirectory,
                    {Attr{kIndexRoot, u"$I30",
                          residentAttribute(kIndexRoot, u"$I30",
                                            emptyIndexRoot(options_.indexRecordSize, clusterSize_))}});
    // Reserved records: in use, unnamed.
    for (std::uint64_t n = 12; n < 16; ++n) {
        Record& r = records_[n];
        r.sequence = static_cast<std::uint16_t>(n);
        r.flags = kInUse;
        r.attributes = {Attr{kStandardInformation, {}, residentAttribute(kStandardInformation, {},
                                                                         standardInformation(kHiddenSystem))},
                        emptyData()};
        serialize(n);
    }
    // Every other record is formatted but free.
    for (std::uint64_t n = 16; n < records_.size(); ++n) {
        serialize(n);
    }
}

void NtfsImageBuilder::setSystemRecord(std::uint64_t number, std::string_view name, std::uint16_t flags,
                                       std::vector<Attr> extra) {
    Record& r = records_[number];
    r.sequence = static_cast<std::uint16_t>(std::max<std::uint64_t>(1, number));
    r.flags = flags;
    r.links = 1;
    const std::u16string name16 = utf8ToUtf16(name);
    const std::uint64_t rootReference = root() | (static_cast<std::uint64_t>(root()) << 48);  // root sequence is 5
    r.attributes = {Attr{kStandardInformation, {},
                         residentAttribute(kStandardInformation, {}, standardInformation(kHiddenSystem))},
                    Attr{kFileName, {},
                         residentAttribute(kFileName, {},
                                           fileName(rootReference, name16, kWin32AndDos, 0, 0,
                                                    (flags & kDirectory) != 0 ? kFileNameIndexPresent
                                                                              : kHiddenSystem),
                                           1)}};
    for (Attr& attribute : extra) {
        r.attributes.push_back(std::move(attribute));
    }
    serialize(number);
}

void NtfsImageBuilder::rebuildBadClusters() {
    std::sort(badRuns_.begin(), badRuns_.end(), [](const Run& a, const Run& b) { return a.first < b.first; });
    std::vector<NtfsRunSpec> runs;
    std::uint64_t next = 0;
    for (const Run& run : badRuns_) {
        if (run.first > next) {
            runs.push_back(NtfsRunSpec{std::nullopt, run.first - next});
        }
        runs.push_back(NtfsRunSpec{run.first, run.count});
        next = run.first + run.count;
    }
    if (next < options_.clusterCount) {
        runs.push_back(NtfsRunSpec{std::nullopt, options_.clusterCount - next});
    }
    Record& r = records_[8];
    std::erase_if(r.attributes, [](const Attr& a) { return a.type == kData && a.name == u"$Bad"; });
    r.attributes.push_back(Attr{kData, u"$Bad",
                                nonResidentAttribute(kData, u"$Bad", runs, options_.clusterCount * clusterSize_, 0, 0,
                                                     clusterSize_)});
    serialize(8);
}

void NtfsImageBuilder::serialize(std::uint64_t number) {
    Record& r = records_[number];
    const std::uint32_t size = options_.recordSize;
    const std::size_t usaCount = size / kBlock + 1;
    const std::size_t firstAttribute = align8(0x30 + 2 * usaCount);
    std::vector<std::byte> p(size);
    std::memcpy(p.data(), "FILE", 4);
    storeLe16(p, 4, 0x30);
    storeLe16(p, 6, static_cast<std::uint16_t>(usaCount));
    storeLe16(p, 0x10, r.sequence);
    storeLe16(p, 0x12, r.links);
    storeLe16(p, 0x14, static_cast<std::uint16_t>(firstAttribute));
    storeLe16(p, 0x16, r.flags);
    // Attributes in type order, then by name.
    std::vector<const Attr*> sorted;
    for (const Attr& attribute : r.attributes) {
        sorted.push_back(&attribute);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const Attr* a, const Attr* b) {
        return a->type != b->type ? a->type < b->type : a->name < b->name;
    });
    std::size_t offset = firstAttribute;
    std::uint16_t id = 0;
    for (const Attr* attribute : sorted) {
        if (offset + attribute->bytes.size() + 8 > size) {
            throw std::runtime_error("attributes do not fit in the MFT record");
        }
        std::copy(attribute->bytes.begin(), attribute->bytes.end(), p.begin() + static_cast<std::ptrdiff_t>(offset));
        storeLe16(p, offset + 14, id++);
        offset += attribute->bytes.size();
    }
    storeLe32(p, offset, kEnd);
    storeLe32(p, 0x18, static_cast<std::uint32_t>(offset + 8));
    storeLe32(p, 0x1C, size);
    storeLe16(p, 0x28, id);
    storeLe32(p, 0x2C, static_cast<std::uint32_t>(number));
    storeLe16(p, 0x30, kUpdateSequence);
    r.plain = std::move(p);
}

std::uint64_t NtfsImageBuilder::allocateRecord() {
    for (std::uint64_t n = kFirstUserRecord; n < records_.size(); ++n) {
        if ((records_[n].flags & kInUse) == 0) {
            Record& r = records_[n];
            r.attributes.clear();
            r.links = 0;
            if (r.sequence == 0) {
                r.sequence = 1;
            }
            return n;
        }
    }
    throw std::runtime_error("MFT is full");
}

std::vector<std::uint64_t> NtfsImageBuilder::allocateRun(std::uint64_t count) {
    std::uint64_t start = cursor_;
    while (true) {
        if (start + count > options_.clusterCount) {
            throw std::runtime_error("volume is full");
        }
        std::uint64_t free = 0;
        while (free < count && !allocated_[static_cast<std::size_t>(start + free)]) {
            ++free;
        }
        if (free == count) {
            break;
        }
        start += free + 1;
    }
    std::vector<std::uint64_t> clusters;
    for (std::uint64_t i = 0; i < count; ++i) {
        clusters.push_back(start + i);
    }
    claim(clusters);
    cursor_ = start + count;
    return clusters;
}

void NtfsImageBuilder::claim(const std::vector<std::uint64_t>& clusters) {
    for (const std::uint64_t cluster : clusters) {
        if (cluster >= options_.clusterCount || allocated_[static_cast<std::size_t>(cluster)]) {
            throw std::runtime_error("cluster " + std::to_string(cluster) + " is out of range or in use");
        }
        allocated_[static_cast<std::size_t>(cluster)] = true;
    }
}

void NtfsImageBuilder::writeData(const std::vector<std::uint64_t>& clusters, std::span<const std::byte> data) {
    std::size_t done = 0;
    for (const std::uint64_t cluster : clusters) {
        if (done >= data.size()) {
            break;
        }
        const std::size_t length = std::min<std::size_t>(clusterSize_, data.size() - done);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(done), length,
                    image_.begin() + static_cast<std::ptrdiff_t>(clusterOffset(cluster)));
        done += length;
    }
}

NtfsImageBuilder::Entry NtfsImageBuilder::addNamed(std::uint64_t parent, std::string_view name, bool directory,
                                                   std::vector<Attr> extra) {
    const std::uint64_t number = allocateRecord();
    Record& r = records_[number];
    r.flags = static_cast<std::uint16_t>(kInUse | (directory ? kDirectory : 0));
    r.links = 1;
    const std::u16string name16 = utf8ToUtf16(name);
    std::uint64_t allocatedSize = 0;
    std::uint64_t realSize = 0;
    for (const Attr& attribute : extra) {
        if (attribute.type == kData && attribute.name.empty()) {
            if (attribute.bytes[8] == std::byte{1}) {
                allocatedSize = loadLe64(attribute.bytes, 40);
                realSize = loadLe64(attribute.bytes, 48);
            } else {
                realSize = loadLe32(attribute.bytes, 16);
                allocatedSize = align8(realSize);
            }
        }
    }
    const std::uint64_t parentReference = parent | (static_cast<std::uint64_t>(records_[parent].sequence) << 48);
    r.attributes = {Attr{kStandardInformation, {},
                         residentAttribute(kStandardInformation, {}, standardInformation(directory ? 0 : kFileArchive))},
                    Attr{kFileName, {},
                         residentAttribute(kFileName, {},
                                           fileName(parentReference, name16, isShortName(name16) ? kWin32AndDos : kWin32,
                                                    allocatedSize, realSize,
                                                    directory ? kFileNameIndexPresent : kFileArchive),
                                           1)}};
    for (Attr& attribute : extra) {
        r.attributes.push_back(std::move(attribute));
    }
    serialize(number);
    return Entry{number, parent, {}, directory};
}

NtfsImageBuilder::Entry NtfsImageBuilder::addDirectory(std::uint64_t parent, std::string_view name,
                                                       std::size_t indexClusters) {
    std::vector<Attr> extra{Attr{kIndexRoot, u"$I30",
                                 residentAttribute(kIndexRoot, u"$I30",
                                                   emptyIndexRoot(options_.indexRecordSize, clusterSize_))}};
    std::vector<std::uint64_t> clusters;
    if (indexClusters > 0) {
        clusters = allocateRun(indexClusters);
        const std::uint64_t bytes = indexClusters * clusterSize_;
        extra.push_back(Attr{kIndexAllocation, u"$I30",
                             nonResidentAttribute(kIndexAllocation, u"$I30", runsOf(clusters), bytes, bytes, 0,
                                                  clusterSize_)});
        std::vector<std::byte> bits(8);
        bits[0] = std::byte{1};
        extra.push_back(Attr{kBitmap, u"$I30", residentAttribute(kBitmap, u"$I30", bits)});
    }
    Entry entry = addNamed(parent, name, true, std::move(extra));
    entry.clusters = std::move(clusters);
    return entry;
}

NtfsImageBuilder::Entry NtfsImageBuilder::addFile(std::uint64_t parent, std::string_view name,
                                                  std::span<const std::byte> data) {
    // Resident when the record can hold it: header, $STANDARD_INFORMATION,
    // $FILE_NAME, $DATA and the end marker.
    const std::size_t usaCount = options_.recordSize / kBlock + 1;
    const std::size_t used = align8(0x30 + 2 * usaCount) + align8(0x18 + 72) +
                             align8(0x18 + 0x42 + 2 * utf8ToUtf16(name).size()) + align8(0x18 + data.size()) + 8;
    if (used <= options_.recordSize) {
        return addNamed(parent, name, false, {Attr{kData, {}, residentAttribute(kData, {}, data)}});
    }
    const std::vector<std::uint64_t> clusters = allocateRun(clustersFor(data.size(), clusterSize_));
    writeData(clusters, data);
    Entry entry = addNamed(parent, name, false,
                           {Attr{kData, {},
                                 nonResidentAttribute(kData, {}, runsOf(clusters), data.size(), data.size(), 0,
                                                      clusterSize_)}});
    entry.clusters = clusters;
    return entry;
}

NtfsImageBuilder::Entry NtfsImageBuilder::addFileInClusters(std::uint64_t parent, std::string_view name,
                                                            std::span<const std::byte> data,
                                                            const std::vector<std::uint64_t>& clusters) {
    if (clusters.size() * clusterSize_ < data.size()) {
        throw std::runtime_error("not enough clusters for the data");
    }
    claim(clusters);
    writeData(clusters, data);
    Entry entry = addNamed(parent, name, false,
                           {Attr{kData, {},
                                 nonResidentAttribute(kData, {}, runsOf(clusters), data.size(), data.size(), 0,
                                                      clusterSize_)}});
    entry.clusters = clusters;
    return entry;
}

NtfsImageBuilder::Entry NtfsImageBuilder::addFileWithRuns(std::uint64_t parent, std::string_view name,
                                                          std::span<const std::byte> data,
                                                          const std::vector<NtfsRunSpec>& runs,
                                                          std::uint16_t attributeFlags) {
    std::vector<std::uint64_t> stored;
    std::uint64_t vcn = 0;
    for (const NtfsRunSpec& run : runs) {
        if (run.lcn) {
            std::vector<std::uint64_t> clusters;
            for (std::uint64_t i = 0; i < run.length; ++i) {
                clusters.push_back(*run.lcn + i);
            }
            claim(clusters);
            const std::uint64_t fileOffset = vcn * clusterSize_;
            if (fileOffset < data.size()) {
                writeData(clusters, data.subspan(static_cast<std::size_t>(fileOffset)));
            }
            stored.insert(stored.end(), clusters.begin(), clusters.end());
        }
        vcn += run.length;
    }
    Entry entry = addNamed(parent, name, false,
                           {Attr{kData, {},
                                 nonResidentAttribute(kData, {}, runs, data.size(), data.size(), attributeFlags,
                                                      clusterSize_)}});
    entry.clusters = std::move(stored);
    return entry;
}

void NtfsImageBuilder::addHardLink(const Entry& entry, std::uint64_t parent, std::string_view name) {
    Record& r = records_[entry.record];
    const std::u16string name16 = utf8ToUtf16(name);
    const std::uint64_t parentReference = parent | (static_cast<std::uint64_t>(records_[parent].sequence) << 48);
    r.attributes.push_back(Attr{kFileName, {},
                                residentAttribute(kFileName, {},
                                                  fileName(parentReference, name16,
                                                           isShortName(name16) ? kWin32AndDos : kWin32, 0, 0,
                                                           kFileArchive),
                                                  1)});
    ++r.links;
    serialize(entry.record);
}

void NtfsImageBuilder::setShortName(const Entry& entry, std::string_view dosName) {
    Record& r = records_[entry.record];
    const auto first = std::find_if(r.attributes.begin(), r.attributes.end(),
                                    [](const Attr& a) { return a.type == kFileName; });
    if (first == r.attributes.end()) {
        throw std::runtime_error("record has no name");
    }
    const std::size_t valueOffset = loadLe16(first->bytes, 20);
    first->bytes[valueOffset + 0x41] = static_cast<std::byte>(kWin32);
    const std::uint64_t parentReference = loadLe64(first->bytes, valueOffset);
    r.attributes.push_back(Attr{kFileName, {},
                                residentAttribute(kFileName, {},
                                                  fileName(parentReference, utf8ToUtf16(dosName), kDos, 0, 0,
                                                           kFileArchive),
                                                  1)});
    serialize(entry.record);
}

void NtfsImageBuilder::deleteEntry(const Entry& entry, bool freeClusters) {
    Record& r = records_[entry.record];
    r.flags = static_cast<std::uint16_t>(r.flags & ~kInUse);
    if (r.sequence != 0) {
        r.sequence = r.sequence == 0xFFFF ? std::uint16_t{1} : static_cast<std::uint16_t>(r.sequence + 1);
    }
    if (freeClusters) {
        for (const Attr& attribute : r.attributes) {
            if (attribute.bytes[8] != std::byte{1}) {
                continue;
            }
            // Walk the builder's own mapping pairs.
            std::size_t pos = loadLe16(attribute.bytes, 32);
            std::int64_t lcn = 0;
            while (pos < attribute.bytes.size() && attribute.bytes[pos] != std::byte{0}) {
                const auto header = static_cast<std::uint8_t>(attribute.bytes[pos]);
                const std::size_t lengthBytes = header & 0x0FU;
                const std::size_t offsetBytes = header >> 4U;
                std::uint64_t length = 0;
                for (std::size_t i = lengthBytes; i-- > 0;) {
                    length = (length << 8) | static_cast<std::uint8_t>(attribute.bytes[pos + 1 + i]);
                }
                if (offsetBytes > 0) {
                    std::uint64_t delta = 0;
                    for (std::size_t i = offsetBytes; i-- > 0;) {
                        delta = (delta << 8) | static_cast<std::uint8_t>(attribute.bytes[pos + 1 + lengthBytes + i]);
                    }
                    if (offsetBytes < 8 && (delta >> (8 * offsetBytes - 1)) != 0) {
                        delta |= ~0ULL << (8 * offsetBytes);
                    }
                    lcn += static_cast<std::int64_t>(delta);
                    for (std::uint64_t i = 0; i < length; ++i) {
                        allocated_[static_cast<std::size_t>(lcn) + i] = false;
                    }
                }
                pos += 1 + lengthBytes + offsetBytes;
            }
        }
    }
    serialize(entry.record);
}

void NtfsImageBuilder::markBadClusters(std::uint64_t first, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        allocated_[static_cast<std::size_t>(first + i)] = true;
    }
    badRuns_.push_back(Run{first, count});
    rebuildBadClusters();
}

void NtfsImageBuilder::setAllocated(std::uint64_t cluster, bool allocated) {
    allocated_.at(static_cast<std::size_t>(cluster)) = allocated;
}

bool NtfsImageBuilder::isAllocated(std::uint64_t cluster) const {
    return allocated_.at(static_cast<std::size_t>(cluster));
}

std::uint64_t NtfsImageBuilder::allocatedClusters() const {
    return static_cast<std::uint64_t>(std::count(allocated_.begin(), allocated_.end(), true));
}

void NtfsImageBuilder::skipClusters(std::uint64_t count) {
    cursor_ += count;
}

std::span<std::byte> NtfsImageBuilder::record(std::uint64_t number) {
    return records_.at(static_cast<std::size_t>(number)).plain;
}

std::size_t NtfsImageBuilder::attributeOffset(std::uint64_t number, std::uint32_t type,
                                              std::u16string_view name) const {
    const std::vector<std::byte>& p = records_.at(static_cast<std::size_t>(number)).plain;
    for (std::size_t offset = loadLe16(p, 0x14); offset + 8 <= p.size() && loadLe32(p, offset) != kEnd;
         offset += loadLe32(p, offset + 4)) {
        const std::size_t nameLength = static_cast<std::uint8_t>(p[offset + 9]);
        const std::size_t nameOffset = loadLe16(p, offset + 10);
        std::u16string stored;
        for (std::size_t i = 0; i < nameLength; ++i) {
            stored.push_back(static_cast<char16_t>(loadLe16(p, offset + nameOffset + 2 * i)));
        }
        if (loadLe32(p, offset) == type && stored == name) {
            return offset;
        }
    }
    throw std::runtime_error("attribute not found");
}

std::uint16_t NtfsImageBuilder::sequence(std::uint64_t number) const {
    return records_.at(static_cast<std::size_t>(number)).sequence;
}

void NtfsImageBuilder::setSequence(std::uint64_t number, std::uint16_t sequence) {
    records_.at(static_cast<std::size_t>(number)).sequence = sequence;
    serialize(number);
}

std::uint64_t NtfsImageBuilder::recordOffset(std::uint64_t number) const {
    std::uint64_t byte = number * options_.recordSize;
    for (const Run& run : mftRuns_) {
        const std::uint64_t runBytes = run.count * clusterSize_;
        if (byte < runBytes) {
            return clusterOffset(run.first) + byte;
        }
        byte -= runBytes;
    }
    throw std::runtime_error("record is beyond the MFT");
}

std::uint64_t NtfsImageBuilder::clusterOffset(std::uint64_t cluster) const {
    return cluster * clusterSize_;
}

void NtfsImageBuilder::writeBootSector(std::uint64_t offset) {
    const std::span<std::byte> s(image_.data() + offset, 512);
    s[0] = std::byte{0xEB};
    s[1] = std::byte{0x52};
    s[2] = std::byte{0x90};
    std::memcpy(s.data() + 3, "NTFS    ", 8);
    storeLe16(s, 11, options_.bytesPerSector);
    const std::uint32_t spc = options_.sectorsPerCluster;
    s[13] = static_cast<std::byte>(spc <= 128 ? spc : 256U - static_cast<unsigned>(std::countr_zero(spc)));
    s[21] = std::byte{0xF8};
    storeLe16(s, 24, 63);
    storeLe16(s, 26, 255);
    storeLe32(s, 36, 0x00800080);
    storeLe64(s, 40, options_.clusterCount * spc);
    storeLe64(s, 48, mftCluster());
    storeLe64(s, 56, mirrorCluster_);
    const auto sizeCode = [this](std::uint32_t bytes) {
        return static_cast<std::byte>(bytes >= clusterSize_ ? bytes / clusterSize_
                                                            : 256U - static_cast<unsigned>(std::countr_zero(bytes)));
    };
    s[64] = sizeCode(options_.recordSize);
    s[68] = sizeCode(options_.indexRecordSize);
    storeLe64(s, 72, options_.volumeSerial);
    s[510] = std::byte{0x55};
    s[511] = std::byte{0xAA};
}

std::vector<std::byte> NtfsImageBuilder::build() {
    const std::uint32_t size = options_.recordSize;
    const auto protect = [size](std::vector<std::byte> record) {
        // Save the last two bytes of each block in the update sequence array
        // and replace them with the update sequence number.
        const std::size_t usa = loadLe16(record, 4);
        const std::uint16_t number = loadLe16(record, usa);
        for (std::size_t block = 0; block < size / kBlock; ++block) {
            const std::size_t end = (block + 1) * kBlock - 2;
            storeLe16(record, usa + 2 * (block + 1), loadLe16(record, end));
            storeLe16(record, end, number);
        }
        return record;
    };
    for (std::uint64_t n = 0; n < records_.size(); ++n) {
        const std::vector<std::byte> disk = protect(records_[n].plain);
        std::copy(disk.begin(), disk.end(), image_.begin() + static_cast<std::ptrdiff_t>(recordOffset(n)));
        if (n < 4) {
            std::copy(disk.begin(), disk.end(),
                      image_.begin() + static_cast<std::ptrdiff_t>(clusterOffset(mirrorCluster_) + n * size));
        }
    }
    std::vector<std::byte> bitmap(bitmapBytes_);
    for (std::size_t c = 0; c < allocated_.size(); ++c) {
        if (allocated_[c]) {
            bitmap[c / 8] |= static_cast<std::byte>(1U << (c % 8));
        }
    }
    std::copy(bitmap.begin(), bitmap.end(), image_.begin() + static_cast<std::ptrdiff_t>(clusterOffset(bitmapCluster_)));
    std::vector<std::byte> mftBitmap((records_.size() + 7) / 8);
    for (std::size_t n = 0; n < records_.size(); ++n) {
        if ((records_[n].flags & kInUse) != 0) {
            mftBitmap[n / 8] |= static_cast<std::byte>(1U << (n % 8));
        }
    }
    std::copy(mftBitmap.begin(), mftBitmap.end(),
              image_.begin() + static_cast<std::ptrdiff_t>(clusterOffset(mftBitmapCluster_)));
    writeBootSector(0);
    writeBootSector(options_.clusterCount * clusterSize_);
    return image_;
}

}  // namespace recovery::test
