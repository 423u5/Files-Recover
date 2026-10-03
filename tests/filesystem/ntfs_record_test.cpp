// NTFS records below the filesystem level: update sequence fixups, run list
// decoding, FILE record and attribute parsing, FILETIME conversion.

#include "filesystem/ntfs/ntfs_record.hpp"

#include "recovery/byte_order.hpp"
#include "support/ntfs_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace recovery::filesystem::ntfs {
namespace {

using test::NtfsImageBuilder;

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Record `number` as build() writes it to disk (with fixups).
std::vector<std::byte> diskRecord(NtfsImageBuilder& builder, std::uint64_t number) {
    const std::vector<std::byte> image = builder.build();
    const auto begin = image.begin() + static_cast<std::ptrdiff_t>(builder.recordOffset(number));
    return {begin, begin + builder.recordSize()};
}

// ---------------------------------------------------------------------------
// Update sequence fixups.
// ---------------------------------------------------------------------------

TEST(NtfsFixupTest, RestoresTheSavedBytes) {
    NtfsImageBuilder builder;
    // Resident data long enough to cross the end of the first 512-byte block.
    const auto data = test::makePattern(600, 1);
    const auto file = builder.addFile(NtfsImageBuilder::root(), "file.bin", data);
    const std::vector<std::byte> plain(builder.record(file.record).begin(), builder.record(file.record).end());
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    EXPECT_NE(disk, plain);  // the block ends hold the update sequence number on disk
    EXPECT_EQ(loadLe16(disk, 510), 3u);
    EXPECT_EQ(loadLe16(disk, 1022), 3u);

    EXPECT_EQ(applyFixups(disk), FixupStatus::Ok);
    // The update sequence array itself keeps its values; everything else matches.
    EXPECT_TRUE(std::equal(disk.begin() + 0x38, disk.end(), plain.begin() + 0x38));
    EXPECT_EQ(loadLe16(disk, 510), loadLe16(plain, 510));
}

TEST(NtfsFixupTest, TornBlockIsAMismatchAndLeavesTheRecordUnchanged) {
    NtfsImageBuilder builder;
    std::vector<std::byte> disk = diskRecord(builder, 0);
    storeLe16(disk, 1022, 0x1234);  // the second block was written by another update
    const std::vector<std::byte> before = disk;
    EXPECT_EQ(applyFixups(disk), FixupStatus::Mismatch);
    EXPECT_EQ(disk, before);
}

TEST(NtfsFixupTest, InvalidArrayLayout) {
    NtfsImageBuilder builder;
    const std::vector<std::byte> good = diskRecord(builder, 0);

    std::vector<std::byte> disk = good;
    storeLe16(disk, 6, 2);  // wrong entry count for two blocks
    EXPECT_EQ(applyFixups(disk), FixupStatus::InvalidLayout);
    disk = good;
    storeLe16(disk, 4, 0x31);  // odd offset
    EXPECT_EQ(applyFixups(disk), FixupStatus::InvalidLayout);
    disk = good;
    storeLe16(disk, 4, 506);  // array overlapping the first protected position
    EXPECT_EQ(applyFixups(disk), FixupStatus::InvalidLayout);
    disk = good;
    storeLe16(disk, 4, 2);  // inside the header's own fields
    EXPECT_EQ(applyFixups(disk), FixupStatus::InvalidLayout);
    std::vector<std::byte> odd(good.begin(), good.begin() + 700);
    EXPECT_EQ(applyFixups(odd), FixupStatus::InvalidLayout);
}

// ---------------------------------------------------------------------------
// Run lists.
// ---------------------------------------------------------------------------

TEST(NtfsRunListTest, SingleRun) {
    // Header 0x21: one length byte, two offset bytes.
    const RunList list = decodeRunList(bytes({0x21, 0x10, 0x00, 0x01, 0x00}), 0);
    EXPECT_EQ(list.problem, RunListProblem::None);
    ASSERT_EQ(list.runs.size(), 1u);
    EXPECT_EQ(list.runs[0], (DataRun{0, 0x10, 0x100}));
    EXPECT_EQ(list.vcnCount(), 0x10u);
}

TEST(NtfsRunListTest, OffsetsAreRelativeAndSigned) {
    const RunList list = decodeRunList(bytes({0x11, 0x04, 0x40, 0x11, 0x02, 0xE0, 0x21, 0x03, 0x00, 0x01, 0x00}), 0);
    EXPECT_EQ(list.problem, RunListProblem::None);
    ASSERT_EQ(list.runs.size(), 3u);
    EXPECT_EQ(list.runs[0], (DataRun{0, 4, 0x40}));
    EXPECT_EQ(list.runs[1], (DataRun{4, 2, 0x20}));    // 0x40 - 0x20
    EXPECT_EQ(list.runs[2], (DataRun{6, 3, 0x120}));   // 0x20 + 0x100
    EXPECT_EQ(list.vcnCount(), 9u);
}

TEST(NtfsRunListTest, SparseRunsHaveNoClusterAndKeepThePreviousOne) {
    const RunList list = decodeRunList(bytes({0x11, 0x02, 0x30, 0x01, 0x05, 0x11, 0x03, 0x10, 0x00}), 0);
    EXPECT_EQ(list.problem, RunListProblem::None);
    ASSERT_EQ(list.runs.size(), 3u);
    EXPECT_EQ(list.runs[0], (DataRun{0, 2, 0x30}));
    EXPECT_EQ(list.runs[1], (DataRun{2, 5, std::nullopt}));
    EXPECT_EQ(list.runs[2], (DataRun{7, 3, 0x40}));  // relative to 0x30, not to the hole
}

TEST(NtfsRunListTest, StartsAtTheGivenVcn) {
    const RunList list = decodeRunList(bytes({0x11, 0x02, 0x30, 0x00}), 100);
    ASSERT_EQ(list.runs.size(), 1u);
    EXPECT_EQ(list.runs[0].vcn, 100u);
    EXPECT_EQ(list.vcnCount(), 2u);
}

TEST(NtfsRunListTest, WideFields) {
    const RunList list = decodeRunList(
        bytes({0x84, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}), 0);
    EXPECT_EQ(list.problem, RunListProblem::None);
    ASSERT_EQ(list.runs.size(), 1u);
    EXPECT_EQ(list.runs[0].length, 0x01000000u);
    EXPECT_EQ(list.runs[0].lcn, 0x0001000000000000ULL);
}

TEST(NtfsRunListTest, MissingTerminatorKeepsTheRuns) {
    const RunList list = decodeRunList(bytes({0x11, 0x04, 0x40}), 0);
    EXPECT_EQ(list.problem, RunListProblem::Truncated);
    EXPECT_EQ(list.runs.size(), 1u);
    EXPECT_EQ(decodeRunList({}, 0).problem, RunListProblem::Truncated);
}

TEST(NtfsRunListTest, PairCutShort) {
    const RunList list = decodeRunList(bytes({0x11, 0x04, 0x40, 0x31, 0x04, 0x40}), 0);
    EXPECT_EQ(list.problem, RunListProblem::Truncated);
    EXPECT_EQ(list.runs.size(), 1u);
}

TEST(NtfsRunListTest, MalformedPairs) {
    EXPECT_EQ(decodeRunList(bytes({0x11, 0x00, 0x40, 0x00}), 0).problem, RunListProblem::ZeroLength);
    EXPECT_EQ(decodeRunList(bytes({0x10, 0x40, 0x00}), 0).problem, RunListProblem::InvalidHeader);  // no length
    EXPECT_EQ(decodeRunList(bytes({0x09, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}), 0).problem, RunListProblem::InvalidHeader);
    EXPECT_EQ(decodeRunList(bytes({0x91, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}), 0).problem,
              RunListProblem::InvalidHeader);
    // A run starting before cluster 0.
    const RunList negative = decodeRunList(bytes({0x11, 0x04, 0x10, 0x11, 0x04, 0xE0, 0x00}), 0);
    EXPECT_EQ(negative.problem, RunListProblem::InvalidCluster);
    EXPECT_EQ(negative.runs.size(), 1u);
    // A length that does not fit a signed 64-bit value.
    EXPECT_EQ(decodeRunList(bytes({0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00}), 0).problem,
              RunListProblem::InvalidCluster);
    // The VCN range overflows.
    EXPECT_EQ(decodeRunList(bytes({0x11, 0x04, 0x10, 0x00}), ~0ULL - 1).problem, RunListProblem::Overflow);
}

TEST(NtfsRunListTest, NumberOfRunsIsBoundedByTheInput) {
    std::vector<std::byte> list;
    for (int i = 0; i < 1000; ++i) {
        list.push_back(std::byte{0x01});  // a one-cluster hole
        list.push_back(std::byte{0x01});
    }
    const RunList decoded = decodeRunList(list, 0);
    EXPECT_EQ(decoded.problem, RunListProblem::Truncated);
    EXPECT_EQ(decoded.runs.size(), 1000u);
    EXPECT_EQ(decoded.vcnCount(), 1000u);
}

// ---------------------------------------------------------------------------
// FILE records.
// ---------------------------------------------------------------------------

TEST(NtfsRecordTest, ParsesTheMftRecord) {
    NtfsImageBuilder builder;
    std::vector<std::byte> disk = diskRecord(builder, 0);
    const Result<MftRecord> record = parseMftRecord(disk, 0);
    RECOVERY_ASSERT_OK(record);
    EXPECT_EQ(record->number, 0u);
    EXPECT_EQ(record->sequence, 1u);
    EXPECT_TRUE(record->inUse());
    EXPECT_FALSE(record->isDirectory());
    EXPECT_TRUE(record->isBaseRecord());
    EXPECT_TRUE(record->problems.empty()) << record->problems.front();
    ASSERT_TRUE(record->standardInformation.has_value());
    EXPECT_EQ(record->standardInformation->created, NtfsImageBuilder::createdTime());
    EXPECT_EQ(record->standardInformation->fileAttributes, 0x06u);
    ASSERT_EQ(record->names.size(), 1u);
    EXPECT_EQ(record->names[0].name, u"$MFT");
    EXPECT_EQ(record->names[0].parent, (FileReference{kRootRecord, 5}));
    EXPECT_EQ(record->names[0].nameSpace, NameSpace::Win32AndDos);
    const Attribute* data = record->find(kAttrData);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->nonResident);
    EXPECT_EQ(data->realSize, 128u * 1024u);
    EXPECT_EQ(data->runs.problem, RunListProblem::None);
    ASSERT_EQ(data->runs.runs.size(), 1u);
    EXPECT_EQ(data->runs.runs[0], (DataRun{0, 256, builder.mftCluster()}));
    EXPECT_EQ(data->lastVcn, 255u);
    EXPECT_NE(record->find(kAttrBitmap), nullptr);
    EXPECT_EQ(record->find(kAttrData, u"other"), nullptr);
}

TEST(NtfsRecordTest, ResidentDataAndNamedAttributes) {
    NtfsImageBuilder builder;
    const auto data = test::makePattern(600, 2);  // crosses a block end: needs the fixups
    const auto file = builder.addFile(NtfsImageBuilder::root(), "r\xC3\xA9sum\xC3\xA9.txt", data);
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    const Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    ASSERT_EQ(record->names.size(), 1u);
    EXPECT_EQ(record->names[0].name, u"résumé.txt");
    EXPECT_EQ(record->names[0].nameSpace, NameSpace::Win32);
    const Attribute* value = record->find(kAttrData);
    ASSERT_NE(value, nullptr);
    EXPECT_FALSE(value->nonResident);
    EXPECT_EQ(value->value, data);
    EXPECT_EQ(value->dataSize(), 600u);

    std::vector<std::byte> root = diskRecord(builder, kRootRecord);
    const Result<MftRecord> directory = parseMftRecord(root, kRootRecord);
    RECOVERY_ASSERT_OK(directory);
    EXPECT_TRUE(directory->isDirectory());
    EXPECT_NE(directory->find(kAttrIndexRoot, u"$I30"), nullptr);
    EXPECT_EQ(directory->find(kAttrIndexRoot), nullptr);  // the name must match
}

TEST(NtfsRecordTest, RejectsDamagedHeaders) {
    NtfsImageBuilder builder;
    const std::vector<std::byte> good = diskRecord(builder, 0);
    const auto rejects = [](std::vector<std::byte> record, std::uint64_t number = 0) {
        const Result<MftRecord> parsed = parseMftRecord(record, number);
        EXPECT_FALSE(parsed.ok());
        if (!parsed.ok()) {
            EXPECT_EQ(parsed.error().code, ErrorCode::InvalidFormat) << describe(parsed.error());
        }
    };

    std::vector<std::byte> r = good;
    std::memcpy(r.data(), "BAAD", 4);
    rejects(r);
    r = good;
    std::memcpy(r.data(), "INDX", 4);
    rejects(r);
    r = good;
    storeLe16(r, 510, 0x7777);  // torn write
    rejects(r);
    rejects(good, 7);  // found in the wrong place: the record says it is number 0
    r = good;
    storeLe32(r, 0x1C, 4096);  // allocated size
    rejects(r);
    r = good;
    storeLe32(r, 0x18, 2000);  // used size beyond the record
    rejects(r);
    r = good;
    storeLe16(r, 0x14, 0x20);  // first attribute inside the update sequence array
    rejects(r);
    r = good;
    storeLe16(r, 0x14, 1000);  // first attribute beyond the used size
    rejects(r);

    std::vector<std::byte> small(256);
    RECOVERY_EXPECT_ERROR(parseMftRecord(small, 0), ErrorCode::InvalidInput);
}

// Offset of the first attribute of `type` in a plain record.
std::size_t offsetOf(NtfsImageBuilder& builder, std::uint64_t record, std::uint32_t type) {
    return builder.attributeOffset(record, type);
}

TEST(NtfsRecordTest, BrokenAttributeChainKeepsWhatCameBefore) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(NtfsImageBuilder::root(), "a.txt", test::makePattern(10, 3));
    storeLe32(builder.record(file.record), offsetOf(builder, file.record, kAttrData) + 4, 0);  // zero length
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    const Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    ASSERT_EQ(record->problems.size(), 1u);
    EXPECT_NE(record->problems[0].find("invalid length"), std::string::npos);
    EXPECT_TRUE(record->standardInformation.has_value());
    EXPECT_EQ(record->names.size(), 1u);
    EXPECT_EQ(record->find(kAttrData), nullptr);
}

TEST(NtfsRecordTest, AttributeLongerThanTheRecordStopsTheChain) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(NtfsImageBuilder::root(), "a.txt", test::makePattern(10, 3));
    storeLe32(builder.record(file.record), offsetOf(builder, file.record, kAttrFileName) + 4, 0x10000);
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    const Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_EQ(record->problems.size(), 1u);
    EXPECT_TRUE(record->names.empty());
    EXPECT_TRUE(record->standardInformation.has_value());
}

TEST(NtfsRecordTest, MalformedAttributesAreSkipped) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(NtfsImageBuilder::root(), "a.txt", test::makePattern(10, 3));
    const std::size_t si = offsetOf(builder, file.record, kAttrStandardInformation);
    const std::size_t fn = offsetOf(builder, file.record, kAttrFileName);
    const std::size_t data = offsetOf(builder, file.record, kAttrData);
    std::span<std::byte> plain = builder.record(file.record);
    storeLe32(plain, si + 16, 0x1000);                                // value beyond the attribute
    plain[fn + 24 + 0x40] = std::byte{200};                           // name longer than the value
    plain[data + 8] = std::byte{7};                                   // unknown form code
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    const Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_EQ(record->problems.size(), 3u);
    EXPECT_FALSE(record->standardInformation.has_value());
    EXPECT_TRUE(record->names.empty());
    EXPECT_EQ(record->find(kAttrData), nullptr);
}

TEST(NtfsRecordTest, NameOutsideTheAttributeIsSkipped) {
    NtfsImageBuilder builder;
    const auto directory = builder.addDirectory(NtfsImageBuilder::root(), "dir");
    const std::size_t index = builder.attributeOffset(directory.record, kAttrIndexRoot, u"$I30");
    storeLe16(builder.record(directory.record), index + 10, 0x7FF0);
    std::vector<std::byte> disk = diskRecord(builder, directory.record);
    const Result<MftRecord> record = parseMftRecord(disk, directory.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_EQ(record->problems.size(), 1u);
    EXPECT_FALSE(record->has(kAttrIndexRoot));
    EXPECT_EQ(record->names.size(), 1u);
}

TEST(NtfsRecordTest, RunListOutsideTheAttributeIsSkipped) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(NtfsImageBuilder::root(), "big.bin", test::makePattern(5000, 4));
    storeLe16(builder.record(file.record), offsetOf(builder, file.record, kAttrData) + 32, 0x400);
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    const Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_EQ(record->problems.size(), 1u);
    EXPECT_EQ(record->find(kAttrData), nullptr);
}

TEST(NtfsRecordTest, MissingEndMarker) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(NtfsImageBuilder::root(), "a.txt", test::makePattern(10, 3));
    std::span<std::byte> plain = builder.record(file.record);
    const std::size_t used = loadLe32(plain, 0x18);
    storeLe32(plain, 0x18, static_cast<std::uint32_t>(used - 8));  // cut off the end marker
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    const Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    ASSERT_EQ(record->problems.size(), 1u);
    EXPECT_NE(record->problems[0].find("not terminated"), std::string::npos);
    EXPECT_NE(record->find(kAttrData), nullptr);
}

TEST(NtfsRecordTest, DeletedAndExtensionRecords) {
    NtfsImageBuilder builder;
    const auto file = builder.addFile(NtfsImageBuilder::root(), "a.txt", test::makePattern(10, 3));
    builder.deleteEntry(file);
    std::vector<std::byte> disk = diskRecord(builder, file.record);
    Result<MftRecord> record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_FALSE(record->inUse());
    EXPECT_EQ(record->sequence, 2u);  // incremented by the deletion
    EXPECT_EQ(record->names.size(), 1u);

    storeLe64(builder.record(file.record), 0x20, 64 | (1ULL << 48));
    disk = diskRecord(builder, file.record);
    record = parseMftRecord(disk, file.record);
    RECOVERY_ASSERT_OK(record);
    EXPECT_FALSE(record->isBaseRecord());
    EXPECT_EQ(record->baseRecord(), (FileReference{64, 1}));
}

TEST(NtfsRecordTest, FreeRecordWithoutANumber) {
    // mkntfs formats reserved records 16-23 as free FILE records numbered 0.
    NtfsImageBuilder builder;
    storeLe32(builder.record(20), 0x2C, 0);
    std::vector<std::byte> disk = diskRecord(builder, 20);
    const Result<MftRecord> record = parseMftRecord(disk, 20);
    RECOVERY_ASSERT_OK(record);
    EXPECT_FALSE(record->inUse());
    // In use, the same number is a misplaced record.
    storeLe16(builder.record(20), 0x16, kRecordInUse);
    disk = diskRecord(builder, 20);
    RECOVERY_EXPECT_ERROR(parseMftRecord(disk, 20), ErrorCode::InvalidFormat);
}

TEST(NtfsRecordTest, BlankRecords) {
    EXPECT_TRUE(isBlankRecord(std::vector<std::byte>(1024)));
    NtfsImageBuilder builder;
    EXPECT_FALSE(isBlankRecord(diskRecord(builder, 100)));  // formatted, not in use
}

// ---------------------------------------------------------------------------
// Values.
// ---------------------------------------------------------------------------

TEST(NtfsValueTest, FileTimes) {
    using namespace std::chrono;
    const auto created = fromFileTime(NtfsImageBuilder::createdTime());
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(*created, sys_days{year{2024} / 5 / 17} + hours{11} + minutes{45} + seconds{30} + milliseconds{500});
    EXPECT_EQ(fromFileTime(116'444'736'000'000'000ULL), sys_time<milliseconds>{});  // the Unix epoch
    EXPECT_EQ(fromFileTime(1)->time_since_epoch(), milliseconds{-11'644'473'600'000LL});
    EXPECT_FALSE(fromFileTime(0).has_value());
    EXPECT_FALSE(fromFileTime(1ULL << 63).has_value());
    EXPECT_TRUE(fromFileTime((1ULL << 63) - 1).has_value());
}

TEST(NtfsValueTest, ReferencesAndSequenceNumbers) {
    EXPECT_EQ(FileReference::fromRaw(0x0005000000000005ULL), (FileReference{5, 5}));
    EXPECT_EQ(FileReference::fromRaw(0xFFFF123456789ABCULL), (FileReference{0x123456789ABCULL, 0xFFFF}));
    EXPECT_EQ(previousSequence(5), 4u);
    EXPECT_EQ(previousSequence(1), 0xFFFFu);  // deletion skips zero
    EXPECT_EQ(previousSequence(0), 0u);       // a zero sequence number is never incremented
}

}  // namespace
}  // namespace recovery::filesystem::ntfs
