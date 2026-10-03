#include "filesystem/ntfs/ntfs_record.hpp"

#include "filesystem/ntfs/ntfs_boot_sector.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/checked_math.hpp"
#include "recovery/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>

namespace recovery::filesystem::ntfs {

namespace {

constexpr std::size_t kHeaderSize = 0x30;  // FILE record header up to the NTFS 3.1 record number
constexpr std::size_t kResidentHeaderSize = 0x18;
constexpr std::size_t kNonResidentHeaderSize = 0x40;
constexpr std::size_t kStandardInformationMinSize = 0x30;  // the NTFS 1.2 layout; 3.x appends fields
constexpr std::size_t kFileNameHeaderSize = 0x42;

std::uint64_t loadUnsigned(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = width; i-- > 0;) {
        value = (value << 8) | loadU8(bytes, offset + i);
    }
    return value;
}

std::int64_t loadSigned(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) noexcept {
    std::uint64_t value = loadUnsigned(bytes, offset, width);
    if (width < 8 && (value >> (8 * width - 1)) != 0) {
        value |= ~0ULL << (8 * width);  // sign-extend
    }
    return static_cast<std::int64_t>(value);
}

std::string describeAttribute(std::uint32_t type, std::size_t offset) {
    return std::format("attribute 0x{:X} at offset {}", type, offset);
}

}  // namespace

std::string_view toString(FixupStatus status) noexcept {
    switch (status) {
    case FixupStatus::Ok:
        return "Ok";
    case FixupStatus::InvalidLayout:
        return "InvalidLayout";
    case FixupStatus::Mismatch:
        return "Mismatch";
    }
    return "Unknown";
}

std::string_view toString(RunListProblem problem) noexcept {
    switch (problem) {
    case RunListProblem::None:
        return "None";
    case RunListProblem::Truncated:
        return "Truncated";
    case RunListProblem::InvalidHeader:
        return "InvalidHeader";
    case RunListProblem::ZeroLength:
        return "ZeroLength";
    case RunListProblem::InvalidCluster:
        return "InvalidCluster";
    case RunListProblem::Overflow:
        return "Overflow";
    }
    return "Unknown";
}

FixupStatus applyFixups(std::span<std::byte> record) noexcept {
    if (record.size() < kFixupBlockSize || record.size() % kFixupBlockSize != 0) {
        return FixupStatus::InvalidLayout;
    }
    const std::size_t usaOffset = loadLe16(record, 4);
    const std::size_t usaCount = loadLe16(record, 6);
    const std::size_t blocks = record.size() / kFixupBlockSize;
    // One entry for the update sequence number, then one per block. The array
    // lies in the header, before the first protected position.
    if (usaCount != blocks + 1 || usaOffset < 8 || usaOffset % 2 != 0 ||
        usaOffset + usaCount * 2 > kFixupBlockSize - 2) {
        return FixupStatus::InvalidLayout;
    }
    const std::uint16_t sequenceNumber = loadLe16(record, usaOffset);
    for (std::size_t block = 0; block < blocks; ++block) {
        if (loadLe16(record, (block + 1) * kFixupBlockSize - 2) != sequenceNumber) {
            return FixupStatus::Mismatch;
        }
    }
    for (std::size_t block = 0; block < blocks; ++block) {
        storeLe16(record, (block + 1) * kFixupBlockSize - 2, loadLe16(record, usaOffset + 2 * (block + 1)));
    }
    return FixupStatus::Ok;
}

std::uint64_t RunList::vcnCount() const noexcept {
    return runs.empty() ? 0 : runs.back().vcn + runs.back().length - runs.front().vcn;
}

RunList decodeRunList(std::span<const std::byte> bytes, std::uint64_t firstVcn) {
    RunList list;
    std::size_t pos = 0;
    std::uint64_t vcn = firstVcn;
    std::int64_t lcn = 0;
    // Bounded: every pair consumes at least two bytes.
    while (true) {
        if (pos >= bytes.size()) {
            list.problem = RunListProblem::Truncated;
            break;
        }
        const std::uint8_t header = loadU8(bytes, pos);
        if (header == 0) {
            break;
        }
        const std::size_t lengthSize = header & 0x0FU;
        const std::size_t offsetSize = header >> 4U;
        if (lengthSize == 0 || lengthSize > 8 || offsetSize > 8) {
            list.problem = RunListProblem::InvalidHeader;
            break;
        }
        if (bytes.size() - pos - 1 < lengthSize + offsetSize) {
            list.problem = RunListProblem::Truncated;
            break;
        }
        const std::uint64_t length = loadUnsigned(bytes, pos + 1, lengthSize);
        if (length == 0) {
            list.problem = RunListProblem::ZeroLength;
            break;
        }
        if (length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            list.problem = RunListProblem::InvalidCluster;
            break;
        }
        DataRun run{vcn, length, std::nullopt};
        if (offsetSize > 0) {
            // The offset is relative to the previous stored run.
            const std::int64_t delta = loadSigned(bytes, pos + 1 + lengthSize, offsetSize);
            if ((delta > 0 && lcn > std::numeric_limits<std::int64_t>::max() - delta) ||
                (delta < 0 && lcn < std::numeric_limits<std::int64_t>::min() - delta)) {
                list.problem = RunListProblem::Overflow;
                break;
            }
            lcn += delta;
            if (lcn < 0) {
                list.problem = RunListProblem::InvalidCluster;
                break;
            }
            run.lcn = static_cast<std::uint64_t>(lcn);
        }
        const std::optional<std::uint64_t> next = checkedAdd(vcn, length);
        if (!next) {
            list.problem = RunListProblem::Overflow;
            break;
        }
        vcn = *next;
        list.runs.push_back(run);
        pos += 1 + lengthSize + offsetSize;
    }
    return list;
}

bool MftRecord::has(std::uint32_t type) const noexcept {
    return std::any_of(attributes.begin(), attributes.end(), [type](const Attribute& a) { return a.type == type; });
}

const Attribute* MftRecord::find(std::uint32_t type, std::u16string_view name) const noexcept {
    for (const Attribute& attribute : attributes) {
        if (attribute.type == type && attribute.name == name && (!attribute.nonResident || attribute.firstVcn == 0)) {
            return &attribute;
        }
    }
    return nullptr;
}

bool isBlankRecord(std::span<const std::byte> record) noexcept {
    return record.size() >= 4 && loadLe32(record, 0) == 0;
}

Result<MftRecord> parseMftRecord(std::span<std::byte> r, std::uint64_t number) {
    if (r.size() < kMinRecordSize || r.size() > kMaxRecordSize || r.size() % kFixupBlockSize != 0) {
        return makeError(ErrorCode::InvalidInput, "MFT record buffer has an invalid size");
    }
    if (std::memcmp(r.data(), "BAAD", 4) == 0) {
        return makeError(ErrorCode::InvalidFormat, "record is marked bad (BAAD)");
    }
    if (std::memcmp(r.data(), "FILE", 4) != 0) {
        return makeError(ErrorCode::InvalidFormat, "record has no FILE signature");
    }
    if (const FixupStatus fixups = applyFixups(r); fixups != FixupStatus::Ok) {
        return makeError(ErrorCode::InvalidFormat, "update sequence " + std::string(toString(fixups)));
    }

    MftRecord record;
    record.number = number;
    record.sequence = loadLe16(r, 0x10);
    record.linkCount = loadLe16(r, 0x12);
    const std::size_t firstAttribute = loadLe16(r, 0x14);
    record.flags = loadLe16(r, 0x16);
    const std::size_t usedSize = loadLe32(r, 0x18);
    const std::size_t allocatedSize = loadLe32(r, 0x1C);
    record.baseRecordRaw = loadLe64(r, 0x20);
    const std::size_t usaEnd = loadLe16(r, 4) + 2ULL * loadLe16(r, 6);

    if (allocatedSize != r.size()) {
        return makeError(ErrorCode::InvalidFormat,
                         std::format("allocated size {} does not match the record size {}", allocatedSize, r.size()));
    }
    if (usedSize > allocatedSize) {
        return makeError(ErrorCode::InvalidFormat, std::format("used size {} exceeds the record", usedSize));
    }
    if (firstAttribute < usaEnd || firstAttribute > usedSize) {
        return makeError(ErrorCode::InvalidFormat, std::format("first attribute offset {} is invalid", firstAttribute));
    }
    // NTFS 3.1 stores the record's own number after the header (the update
    // sequence array then starts at 0x30): a record found elsewhere is
    // misplaced. mkntfs formats its free reserved records (16-23) without a
    // number, so 0 on a record not in use means "never numbered".
    if (loadLe16(r, 4) >= kHeaderSize) {
        const std::uint32_t stored = loadLe32(r, 0x2C);
        const bool unnumberedFree = stored == 0 && (record.flags & kRecordInUse) == 0;
        if (stored != static_cast<std::uint32_t>(number) && !unnumberedFree) {
            return makeError(ErrorCode::InvalidFormat, std::format("record claims to be number {}", stored));
        }
    }

    const std::span<const std::byte> bytes(r.data(), usedSize);
    std::size_t offset = firstAttribute;
    // Bounded: every attribute advances by at least 16 bytes.
    while (true) {
        if (bytes.size() - offset < 4) {
            record.problems.push_back(std::format("attribute chain is not terminated at offset {}", offset));
            break;
        }
        const std::uint32_t type = loadLe32(bytes, offset);
        if (type == kAttrEnd) {
            break;
        }
        if (bytes.size() - offset < 16) {
            record.problems.push_back(describeAttribute(type, offset) + " is cut short");
            break;
        }
        const std::size_t length = loadLe32(bytes, offset + 4);
        if (length < 16 || length > bytes.size() - offset) {
            record.problems.push_back(describeAttribute(type, offset) + std::format(" has invalid length {}", length));
            break;
        }
        const std::span<const std::byte> a = bytes.subspan(offset, length);
        const std::size_t at = offset;
        offset += length;

        Attribute attribute;
        attribute.type = type;
        attribute.recordOffset = static_cast<std::uint32_t>(at);
        const std::uint8_t nonResident = loadU8(a, 8);
        const std::size_t nameLength = loadU8(a, 9);
        const std::size_t nameOffset = loadLe16(a, 10);
        attribute.flags = loadLe16(a, 12);
        attribute.id = loadLe16(a, 14);
        if (nonResident > 1) {
            record.problems.push_back(describeAttribute(type, at) + " has an invalid form code");
            continue;
        }
        attribute.nonResident = nonResident == 1;
        if (nameLength > 0) {
            if (nameOffset > length || (length - nameOffset) / 2 < nameLength) {
                record.problems.push_back(describeAttribute(type, at) + " has its name outside the attribute");
                continue;
            }
            attribute.name = loadUtf16Le(a.subspan(nameOffset), nameLength);
        }

        if (!attribute.nonResident) {
            if (length < kResidentHeaderSize) {
                record.problems.push_back(describeAttribute(type, at) + " is too short for a resident attribute");
                continue;
            }
            const std::size_t valueLength = loadLe32(a, 16);
            const std::size_t valueOffset = loadLe16(a, 20);
            if (!rangeWithin<std::size_t>(valueOffset, valueLength, length)) {
                record.problems.push_back(describeAttribute(type, at) + " has its value outside the attribute");
                continue;
            }
            const std::span<const std::byte> value = a.subspan(valueOffset, valueLength);
            attribute.value.assign(value.begin(), value.end());
        } else {
            if (length < kNonResidentHeaderSize) {
                record.problems.push_back(describeAttribute(type, at) + " is too short for a non-resident attribute");
                continue;
            }
            attribute.firstVcn = loadLe64(a, 16);
            attribute.lastVcn = loadLe64(a, 24);
            const std::size_t runsOffset = loadLe16(a, 32);
            attribute.compressionUnit = loadLe16(a, 34);
            attribute.allocatedSize = loadLe64(a, 40);
            attribute.realSize = loadLe64(a, 48);
            attribute.initializedSize = loadLe64(a, 56);
            if (runsOffset < kNonResidentHeaderSize || runsOffset > length) {
                record.problems.push_back(describeAttribute(type, at) + " has its run list outside the attribute");
                continue;
            }
            attribute.runs = decodeRunList(a.subspan(runsOffset), attribute.firstVcn);
        }

        if (type == kAttrStandardInformation && !record.standardInformation) {
            if (attribute.nonResident || attribute.value.size() < kStandardInformationMinSize) {
                record.problems.push_back(describeAttribute(type, at) + " is malformed");
            } else {
                const std::span<const std::byte> v = attribute.value;
                record.standardInformation = StandardInformation{loadLe64(v, 0x00), loadLe64(v, 0x08),
                                                                 loadLe64(v, 0x10), loadLe64(v, 0x18),
                                                                 loadLe32(v, 0x20)};
            }
        } else if (type == kAttrFileName) {
            const std::span<const std::byte> v = attribute.value;
            const std::size_t length16 = v.size() >= kFileNameHeaderSize ? loadU8(v, 0x40) : 0;
            const std::uint8_t nameSpace = v.size() >= kFileNameHeaderSize ? loadU8(v, 0x41) : 0xFF;
            if (attribute.nonResident || v.size() < kFileNameHeaderSize || nameSpace > 3 ||
                (v.size() - kFileNameHeaderSize) / 2 < length16) {
                record.problems.push_back(describeAttribute(type, at) + " is malformed");
            } else {
                FileNameAttribute name;
                name.parent = FileReference::fromRaw(loadLe64(v, 0x00));
                name.realSize = loadLe64(v, 0x30);
                name.flags = loadLe32(v, 0x38);
                name.nameSpace = static_cast<NameSpace>(nameSpace);
                name.name = loadUtf16Le(v.subspan(kFileNameHeaderSize), length16);
                record.names.push_back(std::move(name));
            }
        }
        record.attributes.push_back(std::move(attribute));
    }
    return record;
}

std::optional<std::chrono::sys_time<std::chrono::milliseconds>> fromFileTime(std::uint64_t fileTime) noexcept {
    // FILETIME counts 100 ns units from 1601-01-01, 11,644,473,600 s before the Unix epoch.
    constexpr std::int64_t kEpochDifferenceMs = 11'644'473'600'000LL;
    if (fileTime == 0 || fileTime > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    const std::int64_t ms = static_cast<std::int64_t>(fileTime / 10'000) - kEpochDifferenceMs;
    return std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(ms));
}

}  // namespace recovery::filesystem::ntfs
