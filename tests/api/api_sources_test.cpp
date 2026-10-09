// Sources and imaging through the API (P19): the disks listed with their
// drive letters and roles, what a source holds, sources that cannot be read
// (with what to do), and images: bad sectors zero-filled and listed,
// cancelled and resumed, never overwriting, never on the source.

#include "api_test_support.hpp"

#include "imaging/image_metadata.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <atomic>

namespace recovery::api::test {
namespace {

storage::AttachedDisk attached(std::uint32_t number, std::uint64_t size, std::string bus,
                               std::vector<std::filesystem::path> volumes) {
    storage::AttachedDisk disk;
    disk.number = number;
    disk.sizeBytes = size;
    disk.logicalSectorSize = 512;
    disk.vendor = "Simulated";
    disk.product = number == 0 ? "System Disk" : "Card Reader";
    disk.removable = number != 0;
    disk.bus = std::move(bus);
    disk.volumes = std::move(volumes);
    return disk;
}

TEST(ApiSourcesTest, TheDiskListTellsLettersAndRoles) {
    ApiWorld world;
    world.hooks().diskLister = [] {
        // In disk order or not: the list is by number.
        return Result<std::vector<storage::AttachedDisk>>(std::vector{
            attached(7, 2 * 1024 * 1024, "USB", {L"E:\\", L"F:\\"}),
            attached(0, 512ull * 1024 * 1024 * 1024, "NVMe", {L"C:\\"}),
            attached(9, 0, "SD", {}),
        });
    };
    const Result<std::vector<DiskInfo>> disks = world.api().listSources();
    RECOVERY_ASSERT_OK(disks);
    ASSERT_EQ(disks->size(), 3u);
    const DiskInfo& system = (*disks)[0];
    const DiskInfo& card = (*disks)[1];
    const DiskInfo& empty = (*disks)[2];
    EXPECT_EQ(system.number, 0u);
    EXPECT_TRUE(system.system);
    EXPECT_TRUE(system.holdsSessions);
    EXPECT_EQ(card.number, 7u);
    EXPECT_EQ(card.devicePath, "\\\\.\\PhysicalDrive7");
    EXPECT_FALSE(card.system);
    EXPECT_FALSE(card.holdsSessions);
    EXPECT_EQ(card.driveLetters, (std::vector<std::string>{"E:", "F:"}));
    EXPECT_EQ(card.removable, true);
    EXPECT_EQ(card.bus, "USB");
    EXPECT_EQ(card.description, "Disk 7: Simulated Card Reader, 2.0 MiB, USB (E: F:)");
    EXPECT_EQ(empty.size, 0u);
    EXPECT_EQ(empty.description, "Disk 9: Simulated Card Reader, no medium, SD");

    // Sessions kept on the card's disk: scanning it needs another folder.
    world.restart();
    std::filesystem::create_directories(world.sessions());
    world.placeOn(world.sessions(), 7);
    const Result<std::vector<DiskInfo>> moved = world.api().listSources();
    RECOVERY_ASSERT_OK(moved);
    EXPECT_FALSE((*moved)[0].holdsSessions);
    EXPECT_TRUE((*moved)[1].holdsSessions);
}

TEST(ApiSourcesTest, InspectingTheCardImage) {
    ApiWorld world;
    const Result<SourceInspection> inspection = world.api().inspectSource(world.cardSource());
    RECOVERY_ASSERT_OK(inspection);
    EXPECT_EQ(inspection->source.kind, SourceKind::DiskImage);
    EXPECT_EQ(inspection->source.size, std::filesystem::file_size(world.cardImage()));
    EXPECT_EQ(inspection->source.sectorSize, 512u);
    EXPECT_NE(inspection->source.description.find("disk image"), std::string::npos);
    EXPECT_FALSE(inspection->image.has_value());
    EXPECT_EQ(inspection->partitionScheme, PartitionScheme::Unpartitioned);
    ASSERT_EQ(inspection->volumes.size(), 1u);
    const VolumeInfo& volume = inspection->volumes[0];
    EXPECT_EQ(volume.filesystem, FilesystemKind::Fat32);
    EXPECT_EQ(volume.clusterSize, 512u);
    EXPECT_EQ(volume.offset, 0u);
    EXPECT_EQ(volume.serialNumber.size(), 9u);
    EXPECT_FALSE(volume.error.has_value());
}

TEST(ApiSourcesTest, InspectingADiskOfThreeVolumes) {
    ApiWorld world;
    const std::uint32_t disk = world.addDisk(::recovery::test::threeVolumeDisk());
    const Result<SourceInspection> inspection = world.api().inspectSource(SourceRef::physicalDisk(disk));
    RECOVERY_ASSERT_OK(inspection);
    EXPECT_EQ(inspection->source.kind, SourceKind::PhysicalDisk);
    EXPECT_EQ(inspection->source.disk, disk);
    EXPECT_EQ(inspection->source.vendor, "Simulated");
    EXPECT_EQ(inspection->partitionScheme, PartitionScheme::Mbr);
    ASSERT_EQ(inspection->partitions.size(), 3u);
    ASSERT_EQ(inspection->volumes.size(), 3u);
    EXPECT_EQ(inspection->volumes[0].filesystem, FilesystemKind::Fat32);
    EXPECT_EQ(inspection->volumes[0].label, "FATVOL");
    EXPECT_EQ(inspection->volumes[1].filesystem, FilesystemKind::ExFat);
    EXPECT_EQ(inspection->volumes[1].label, "EXVOL");
    EXPECT_EQ(inspection->volumes[1].serialNumber, "0BAD-F00D");
    EXPECT_EQ(inspection->volumes[2].filesystem, FilesystemKind::Ntfs);
    EXPECT_EQ(inspection->volumes[2].label, "NTVOL");
    EXPECT_EQ(inspection->volumes[2].serialNumber.size(), 16u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(inspection->volumes[i].partition, inspection->partitions[i].index);
        EXPECT_EQ(inspection->volumes[i].offset, inspection->partitions[i].offset);
    }
}

TEST(ApiSourcesTest, SourcesThatCannotBeReadSayWhy) {
    ApiWorld world;
    RECOVERY_EXPECT_ERROR(world.api().inspectSource(SourceRef::imageFile(world.folder() / "missing.img")),
                          ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().inspectSource(SourceRef::imageFile(world.folder())), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().inspectSource(SourceRef::imageFile("\\\\.\\C:")), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().inspectSource(SourceRef::imageFile({})), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().inspectSource(SourceRef::physicalDisk(5000)), ErrorCode::InvalidInput);

    const Result<SourceInspection> absent = world.api().inspectSource(SourceRef::physicalDisk(3));
    ASSERT_FALSE(absent.ok());
    EXPECT_NE(absent.error().message.find("no such disk"), std::string::npos) << describe(absent.error());

    const std::uint32_t disk = world.addDisk(::recovery::test::readFile(world.cardImage()));
    world.rack().failOpen(disk, makeError(ErrorCode::IoError, "access denied", 5));
    const Result<SourceInspection> denied = world.api().inspectSource(SourceRef::physicalDisk(disk));
    ASSERT_FALSE(denied.ok());
    EXPECT_NE(denied.error().message.find("administrator"), std::string::npos) << describe(denied.error());
    RECOVERY_EXPECT_ERROR(world.api().startScan(SourceRef::physicalDisk(disk)), ErrorCode::IoError);
}

TEST(ApiImagingTest, ADiskWithBadSectorsIsImagedWithZerosAndTheirList) {
    ApiWorld world;
    Bytes data = ::recovery::test::readFile(world.cardImage());
    const std::uint32_t disk = world.addDisk(data, {{4096, 8192}});
    const std::filesystem::path image = world.folder() / "disk.img";
    ImagingOptions options;
    options.image = image;
    options.sectorRetries = 0;
    const Result<ImagingId> imaging = world.api().createImage(SourceRef::physicalDisk(disk), options);
    RECOVERY_ASSERT_OK(imaging);
    const Result<Progress> done = world.api().waitForImaging(*imaging, kWait);
    RECOVERY_ASSERT_OK(done);
    EXPECT_EQ(done->operation, OperationKind::Imaging);
    ASSERT_EQ(done->state, OperationState::Completed) << (done->error ? describe(*done->error) : "");
    ASSERT_TRUE(done->imaging.has_value());
    EXPECT_EQ(done->imaging->bytesImaged, data.size());
    EXPECT_EQ(done->imaging->unreadableBytes, 4096u);
    EXPECT_EQ(done->imaging->unreadableRegions, 1u);
    EXPECT_EQ(done->imaging->fraction, 1.0);
    EXPECT_EQ(done->imaging->metadataFile, imaging::metadataPathFor(image));

    std::fill(data.begin() + 4096, data.begin() + 8192, std::byte{0});
    EXPECT_EQ(::recovery::test::readFile(image), data);

    // Its events, by imaging.
    ASSERT_TRUE(world.log().waitFor([&](const std::vector<Event>& events) {
        return std::any_of(events.begin(), events.end(), [&](const Event& event) {
            return event.imaging == *imaging && event.kind == EventKind::OperationFinished;
        });
    }));
    const std::vector<Event> events = world.log().eventsOf(*imaging);
    EXPECT_EQ(events.front().kind, EventKind::OperationStarted);
    EXPECT_TRUE(events.front().session.empty());

    // What the image's metadata says, and a scan of the image.
    const Result<SourceInspection> inspection = world.api().inspectSource(SourceRef::imageFile(image));
    RECOVERY_ASSERT_OK(inspection);
    ASSERT_TRUE(inspection->image.has_value());
    EXPECT_EQ(inspection->image->state, ImageState::Completed);
    EXPECT_EQ(inspection->image->unreadableBytes, 4096u);
    EXPECT_NE(inspection->image->imagedFrom.find("physical disk"), std::string::npos);
    const Result<std::string> id = world.api().startScan(SourceRef::imageFile(image));
    RECOVERY_ASSERT_OK(id);
    EXPECT_EQ(world.waitIdle(*id).state, OperationState::Completed);
}

TEST(ApiImagingTest, ACancelledImageResumes) {
    std::atomic<bool> cancelled{false};
    RecoveryApi* hooked = nullptr;
    ApiWorld world;
    const Bytes data = ::recovery::test::makePattern(8 * 1024 * 1024);
    const std::uint32_t disk = world.addDisk(data);
    const std::filesystem::path image = world.folder() / "big.img";
    world.hooks().onOperationProgress = [&](std::string_view session, const Progress& progress) {
        // The first imaging is number 1.
        if (session.empty() && progress.imaging.has_value() && progress.imaging->bytesImaged > 0 &&
            !cancelled.exchange(true)) {
            EXPECT_TRUE(hooked->cancelImaging(ImagingId{1}).ok());
        }
    };
    RecoveryApi& api = world.api();
    hooked = &api;
    ImagingOptions options;
    options.image = image;
    options.blockSize = 64 * 1024;
    const Result<ImagingId> first = api.createImage(SourceRef::physicalDisk(disk), options);
    RECOVERY_ASSERT_OK(first);
    EXPECT_EQ(first->value, 1u);
    const Result<Progress> stopped = api.waitForImaging(*first, kWait);
    RECOVERY_ASSERT_OK(stopped);
    EXPECT_EQ(stopped->state, OperationState::Cancelled);
    EXPECT_LT(stopped->imaging->bytesImaged, data.size());
    RECOVERY_EXPECT_ERROR(api.cancelImaging(*first), ErrorCode::InvalidInput);

    // Without `resume` the unfinished image is not touched.
    RECOVERY_EXPECT_ERROR(api.createImage(SourceRef::physicalDisk(disk), options), ErrorCode::DestinationError);
    options.resume = true;
    const Result<ImagingId> second = api.createImage(SourceRef::physicalDisk(disk), options);
    RECOVERY_ASSERT_OK(second);
    EXPECT_NE(second->value, first->value);
    const Result<Progress> done = api.waitForImaging(*second, kWait);
    RECOVERY_ASSERT_OK(done);
    EXPECT_EQ(done->state, OperationState::Completed);
    EXPECT_GT(done->imaging->resumedFrom, 0u);
    EXPECT_EQ(::recovery::test::readFile(image), data);
}

TEST(ApiImagingTest, AnImageIsNeverOverwrittenNorWrittenOnTheSource) {
    ApiWorld world;
    const std::uint32_t disk = world.addDisk(::recovery::test::readFile(world.cardImage()));
    ImagingOptions options;
    options.image = world.cardImage();  // exists
    RECOVERY_EXPECT_ERROR(world.api().createImage(SourceRef::physicalDisk(disk), options),
                          ErrorCode::DestinationError);
    options.image = world.folder() / "none.img";
    options.resume = true;
    RECOVERY_EXPECT_ERROR(world.api().createImage(SourceRef::physicalDisk(disk), options), ErrorCode::InvalidInput);
    options.resume = false;
    std::filesystem::create_directories(world.folder() / "on-the-disk");
    world.placeOn(world.folder() / "on-the-disk", disk);
    options.image = world.folder() / "on-the-disk" / "disk.img";
    RECOVERY_EXPECT_ERROR(world.api().createImage(SourceRef::physicalDisk(disk), options),
                          ErrorCode::DestinationError);
    options.image = world.folder() / "ok.img";
    options.blockSize = 0;
    RECOVERY_EXPECT_ERROR(world.api().createImage(SourceRef::physicalDisk(disk), options), ErrorCode::InvalidInput);
    options.blockSize = 1024 * 1024;
    options.sectorRetries = 1000;
    RECOVERY_EXPECT_ERROR(world.api().createImage(SourceRef::physicalDisk(disk), options), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world.api().getImagingProgress(ImagingId{42}), ErrorCode::InvalidInput);
    EXPECT_FALSE(std::filesystem::exists(world.folder() / "on-the-disk" / "disk.img"));
}

}  // namespace
}  // namespace recovery::api::test
