// One behavioural contract, run against every IStorageSource implementation.
// Proves that image and physical sources answer identical requests identically.

#include "storage/disk_image_source.hpp"
#include "storage/physical_disk_source.hpp"

#include "support/fake_device.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <random>
#include <string>

namespace recovery::storage {
namespace {

// 1 MiB + 3 * 4 KiB: a multiple of every tested sector size.
constexpr std::size_t kSourceSize = 1 * kMiB + 3 * 4096;

using SourceFactory =
    std::function<std::unique_ptr<IStorageSource>(const std::vector<std::byte>& data, const test::TempDir& dir)>;

struct ContractCase {
    std::string name;
    std::uint32_t sectorSize;
    SourceFactory make;
};

std::unique_ptr<IStorageSource> makePhysical(const std::vector<std::byte>& data, std::uint32_t sectorSize) {
    auto config = std::make_shared<test::FakeDeviceConfig>();
    config->data = data;
    config->geometry = test::diskGeometry(data.size(), sectorSize);
    return std::make_unique<PhysicalDiskSource>(0, std::make_shared<test::MockDeviceOpener>(config));
}

std::unique_ptr<IStorageSource> makeImage(const std::vector<std::byte>& data, const test::TempDir& dir,
                                          std::uint32_t sectorSize) {
    const auto path = dir / ("image-" + std::to_string(sectorSize) + ".img");
    test::writeFile(path, data);
    return std::make_unique<DiskImageSource>(path, DiskImageOptions{sectorSize});
}

class SourceContractTest : public ::testing::TestWithParam<ContractCase> {
protected:
    void SetUp() override {
        data_ = test::makePattern(kSourceSize, 0xC0FFEE);
        source_ = GetParam().make(data_, dir_);
        RECOVERY_ASSERT_OK(source_->open());
    }

    test::TempDir dir_;
    std::vector<std::byte> data_;
    std::unique_ptr<IStorageSource> source_;
};

TEST_P(SourceContractTest, GeometryMatches) {
    EXPECT_EQ(source_->size(), kSourceSize);
    EXPECT_EQ(source_->sectorSize(), GetParam().sectorSize);
    EXPECT_TRUE(source_->getInfo().readOnly);
}

TEST_P(SourceContractTest, FullReadMatchesContent) {
    std::vector<std::byte> buffer(kSourceSize);
    RECOVERY_ASSERT_OK(source_->readExact(ByteOffset{0}, buffer));
    EXPECT_EQ(buffer, data_);
}

TEST_P(SourceContractTest, RandomReadsMatchContent) {
    std::mt19937_64 random(7);
    for (int i = 0; i < 500; ++i) {
        const std::size_t length = random() % 70000;
        const std::uint64_t offset = random() % (kSourceSize - length + 1);
        std::vector<std::byte> buffer(length);
        const ReadResult result = source_->read(ByteOffset{offset}, buffer);
        ASSERT_TRUE(result.ok()) << toString(result.status) << " at " << offset << "+" << length;
        ASSERT_EQ(result.bytesRead, length);
        ASSERT_TRUE(std::equal(buffer.begin(), buffer.end(), data_.begin() + static_cast<std::ptrdiff_t>(offset)));
    }
}

TEST_P(SourceContractTest, SectorReadsMatchContent) {
    const std::uint32_t sector = GetParam().sectorSize;
    const std::uint64_t sectors = kSourceSize / sector;
    std::vector<std::byte> buffer(2 * sector);
    for (std::uint64_t s = 0; s + 2 <= sectors; s += 37) {
        const ReadResult result = source_->readSectors(SectorNumber{s}, SectorCount{2}, buffer);
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE(std::equal(buffer.begin(), buffer.end(), data_.begin() + static_cast<std::ptrdiff_t>(s * sector)));
    }
    EXPECT_EQ(source_->readSectors(SectorNumber{sectors}, SectorCount{1}, buffer).status, ReadStatus::OutOfRange);
}

TEST_P(SourceContractTest, BoundaryRequestsBehaveIdentically) {
    std::vector<std::byte> one(1);
    std::vector<std::byte> two(2);
    const std::span<std::byte> none;
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

    EXPECT_EQ(source_->read(ByteOffset{0}, none).status, ReadStatus::Success);
    EXPECT_EQ(source_->read(ByteOffset{kSourceSize}, none).status, ReadStatus::Success);
    EXPECT_EQ(source_->read(ByteOffset{kSourceSize + 1}, none).status, ReadStatus::OutOfRange);
    EXPECT_EQ(source_->read(ByteOffset{kSourceSize - 1}, one).status, ReadStatus::Success);
    EXPECT_EQ(source_->read(ByteOffset{kSourceSize - 1}, two).status, ReadStatus::OutOfRange);
    EXPECT_EQ(source_->read(ByteOffset{kSourceSize}, one).status, ReadStatus::OutOfRange);
    EXPECT_EQ(source_->read(ByteOffset{kMax}, one).status, ReadStatus::InvalidArgument);
    EXPECT_EQ(source_->read(ByteOffset{kMaxAddressableOffset + 1}, one).status, ReadStatus::InvalidArgument);
    EXPECT_EQ(source_->readSectors(SectorNumber{kMax}, SectorCount{1}, two).status, ReadStatus::InvalidArgument);
}

TEST_P(SourceContractTest, ClosedSourceRejectsReads) {
    source_->close();
    std::vector<std::byte> buffer(16);
    EXPECT_EQ(source_->read(ByteOffset{0}, buffer).status, ReadStatus::NotOpen);
    EXPECT_EQ(source_->size(), 0u);
    RECOVERY_ASSERT_OK(source_->open());
    EXPECT_TRUE(source_->read(ByteOffset{0}, buffer).ok());
}

INSTANTIATE_TEST_SUITE_P(
    AllSources, SourceContractTest,
    ::testing::Values(
        ContractCase{"DiskImage512", 512, [](const auto& data, const auto& dir) { return makeImage(data, dir, 512); }},
        ContractCase{"DiskImage4096", 4096,
                     [](const auto& data, const auto& dir) { return makeImage(data, dir, 4096); }},
        ContractCase{"PhysicalDisk512", 512, [](const auto& data, const auto&) { return makePhysical(data, 512); }},
        ContractCase{"PhysicalDisk4096", 4096,
                     [](const auto& data, const auto&) { return makePhysical(data, 4096); }},
        ContractCase{"Memory512", 512,
                     [](const auto& data, const auto&) -> std::unique_ptr<IStorageSource> {
                         return std::make_unique<test::MemoryStorageSource>(data, 512);
                     }}),
    [](const auto& info) { return info.param.name; });

}  // namespace
}  // namespace recovery::storage
