#include "support/card_fixtures.hpp"

#include "recovery/text.hpp"
#include "support/audio_builders.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/ntfs_builder.hpp"
#include "support/ole2_builders.hpp"
#include "support/partition_builder.hpp"
#include "support/pdf_builders.hpp"
#include "support/test_files.hpp"
#include "support/zip_builders.hpp"

#include <algorithm>
#include <system_error>

namespace recovery::test {

const std::map<std::string, Bytes>& cardOriginals() {
    static const std::map<std::string, Bytes> originals = [] {
        const auto photo = [](std::uint64_t seed, std::uint16_t width, std::uint16_t height) {
            JpegOptions options;
            options.width = width;
            options.height = height;
            options.seed = seed;
            return makeJpeg(options);
        };
        std::map<std::string, Bytes> files;
        files["PHOTO.JPG"] = photo(1, 128, 96);
        files["COPY.JPG"] = photo(1, 128, 96);
        files["PICTURE.PNG"] = makePng({});
        files["CLIP.MP4"] = makeMp4({}).bytes;
        files["SOUND.WAV"] = makeWav({});
        files["_LD.JPG"] = photo(2, 96, 64);
        files["_RAG.JPG"] = photo(3, 160, 120);
        return files;
    }();
    return originals;
}

std::map<std::string, Bytes> filesBelow(const std::filesystem::path& root) {
    std::map<std::string, Bytes> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) {
            std::string relative = toUtf8(std::filesystem::relative(it->path(), root));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            files[relative] = readFile(it->path());
        }
    }
    return files;
}

SimulatedDisk simulatedDisk(Bytes bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>> failing) {
    auto config = std::make_shared<FakeDeviceConfig>();
    config->geometry = diskGeometry(bytes.size());
    config->data = std::move(bytes);
    config->description.vendor = "Simulated";
    config->description.product = "Card Reader";
    config->description.removable = true;
    config->failingRanges = std::move(failing);
    SimulatedDisk disk;
    disk.config = config;
    disk.opener = std::make_shared<MockDeviceOpener>(config);
    return disk;
}

DocumentCard documentCard() {
    Fat32BuilderOptions options;
    options.clusterCount = 4096;
    Fat32ImageBuilder builder(options);
    const auto archive = [](std::string_view first, std::string_view second, std::uint64_t seed) {
        return storedArchive(
            {{std::string(first), makePattern(300, seed)}, {std::string(second), makePattern(900, seed + 1)}});
    };
    (void)builder.addFile(Fat32ImageBuilder::root(), "REPORT.PDF", simplePdf("report"));
    (void)builder.addFile(Fat32ImageBuilder::root(), "LETTER.DOC", wordDocument());
    (void)builder.addFile(Fat32ImageBuilder::root(), "BOOK.DOCX", archive("[Content_Types].xml", "word/document.xml", 1));
    (void)builder.addFile(Fat32ImageBuilder::root(), "APP.APK", archive("AndroidManifest.xml", "classes.dex", 3));
    (void)builder.addFile(Fat32ImageBuilder::root(), "NOTES.ZIP", archive("notes.txt", "todo.txt", 5));
    DocumentCard card;
    card.orphan = archive("[Content_Types].xml", "word/document.xml", 7);
    std::copy(card.orphan.begin(), card.orphan.end(),
              builder.raw().begin() + static_cast<std::ptrdiff_t>(builder.clusterOffset(3000)));
    card.image = builder.build();
    return card;
}

Bytes threeVolumeDisk() {
    Fat32BuilderOptions fatOptions;
    fatOptions.label = "FATVOL";
    Fat32ImageBuilder fat32(fatOptions);
    (void)fat32.addFile(Fat32ImageBuilder::root(), "A.JPG", makeJpeg({}));
    const Bytes fatVolume = fat32.build();

    ExFatBuilderOptions exfatOptions;
    exfatOptions.label = "EXVOL";
    exfatOptions.volumeSerial = 0x0BADF00D;
    ExFatImageBuilder exfat(exfatOptions);
    (void)exfat.addFile(exfat.root(), "B.PNG", makePng({}));
    const Bytes exfatVolume = exfat.build();

    NtfsBuilderOptions ntfsOptions;
    ntfsOptions.label = "NTVOL";
    NtfsImageBuilder ntfs(ntfsOptions);
    (void)ntfs.addFile(NtfsImageBuilder::root(), "C.WAV", makeWav({}));
    const Bytes ntfsVolume = ntfs.build();

    constexpr std::uint32_t kSector = 512;
    constexpr std::uint32_t kFirst = 2048;
    const auto sectors = [](const Bytes& volume) { return static_cast<std::uint32_t>(volume.size() / kSector); };
    const std::uint32_t second = kFirst + sectors(fatVolume) + 2048;
    const std::uint32_t third = second + sectors(exfatVolume) + 2048;
    Bytes disk((static_cast<std::size_t>(third) + sectors(ntfsVolume) + 64) * kSector);
    std::copy(fatVolume.begin(), fatVolume.end(), disk.begin() + std::ptrdiff_t{kFirst} * kSector);
    std::copy(exfatVolume.begin(), exfatVolume.end(), disk.begin() + static_cast<std::ptrdiff_t>(second) * kSector);
    std::copy(ntfsVolume.begin(), ntfsVolume.end(), disk.begin() + static_cast<std::ptrdiff_t>(third) * kSector);
    writeMbrSector(disk, kSector, 0,
                                     {{0x00, 0x0C, kFirst, sectors(fatVolume)},
                                      {0x00, 0x07, second, sectors(exfatVolume)},
                                      {0x00, 0x07, third, sectors(ntfsVolume)}});
    return disk;
}

}  // namespace recovery::test
