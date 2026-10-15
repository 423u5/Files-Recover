// P20: large drives. Disks far larger than memory are VirtualSources (their
// content computed, the volumes planted where their partitions are):
//  - a 16 TiB GPT disk whose FAT32, exFAT and NTFS volumes lie at 3, 8 and
//    15 TiB, past every 32-bit sector number and byte offset;
//  - an MBR disk whose partition starts just below the last sector an MBR can
//    address (2 TiB) and ends beyond it.
// Quick scans find every file at its 64-bit offset and identify it by the
// SHA-256 of its original. With RECOVERY_LARGE_IMAGE_DIR set, a sparse image
// file of RECOVERY_LARGE_IMAGE_GIB GiB (default 64) is also scanned deep
// through the API, and the time and the process's peak memory are reported.

#include "api/recovery_api.hpp"
#include "carving/format_registry.hpp"
#include "formats/archive_formats.hpp"
#include "formats/audio_formats.hpp"
#include "formats/document_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "partition/guid.hpp"
#include "recovery/sha256.hpp"
#include "recovery/text.hpp"
#include "scan/scan_coordinator.hpp"
#include "support/audio_builders.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "support/virtual_source.hpp"
#include "validation/deflate_decoder.hpp"
#include "validation/media_validator.hpp"

#include <gtest/gtest.h>

#include <windows.h>
#include <psapi.h>
#include <winioctl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace recovery::test::corpus {
namespace {

using Bytes = std::vector<std::byte>;
constexpr std::uint64_t kTiB = 1ULL << 40;
constexpr std::uint64_t kSector = 512;

const carving::FormatRegistry& formatsOnce() {
    static const carving::FormatRegistry registry = [] {
        carving::FormatRegistry formats;
        const bool images = formats::registerImageFormats(formats).ok();
        const bool audio = formats::registerAudioFormats(formats).ok();
        const bool video = formats::registerVideoFormats(formats).ok();
        const bool archives = formats::registerArchiveFormats(formats, validation::deflateDecoder()).ok();
        const bool documents = formats::registerDocumentFormats(formats, validation::deflateDecoder()).ok();
        EXPECT_TRUE(images && audio && video && archives && documents);
        return formats;
    }();
    return registry;
}

const validation::MediaValidatorRegistry& mediaOnce() {
    static const validation::MediaValidatorRegistry registry = [] {
        validation::MediaValidatorRegistry media;
        const bool registered = validation::registerMediaValidators(media).ok();
        EXPECT_TRUE(registered);
        return media;
    }();
    return registry;
}

Bytes photo(std::uint64_t seed) {
    JpegOptions options;
    options.width = 64;
    options.height = 48;
    options.seed = seed;
    return makeJpeg(options);
}

// A volume of each filesystem with two files; `files` gets name -> bytes.
Bytes fat32Volume(std::map<std::string, Bytes>& files) {
    Fat32BuilderOptions options;
    options.clusterCount = 4096;
    options.label = "FARFAT";
    Fat32ImageBuilder builder(options);
    files["FAR.JPG"] = photo(71);
    files["FAR.WAV"] = makeWav({});
    (void)builder.addFile(Fat32ImageBuilder::root(), "FAR.JPG", files["FAR.JPG"]);
    (void)builder.addFile(Fat32ImageBuilder::root(), "FAR.WAV", files["FAR.WAV"]);
    return builder.build();
}

Bytes exfatVolume(std::map<std::string, Bytes>& files) {
    ExFatBuilderOptions options;
    options.clusterCount = 4096;
    options.label = "FAREX";
    ExFatImageBuilder builder(options);
    files["farther.png"] = makePng({});
    files["farther.jpg"] = photo(72);
    (void)builder.addFile(builder.root(), "farther.png", files["farther.png"]);
    (void)builder.addFile(builder.root(), "farther.jpg", files["farther.jpg"]);
    return builder.build();
}

Bytes ntfsVolume(std::map<std::string, Bytes>& files) {
    NtfsBuilderOptions options;
    options.label = "FARNT";
    NtfsImageBuilder builder(options);
    GifOptions gif;  // large enough to be stored in clusters, not in its record
    gif.width = 64;
    gif.height = 48;
    gif.colorBits = 8;
    files["farthest.gif"] = makeGif(gif);
    files["farthest.jpg"] = photo(73);
    (void)builder.addFile(NtfsImageBuilder::root(), "farthest.gif", files["farthest.gif"]);
    (void)builder.addFile(NtfsImageBuilder::root(), "farthest.jpg", files["farthest.jpg"]);
    return builder.build();
}

// What a quick scan of `source` delivers.
std::vector<evaluation::EvaluatedCandidate> quickScan(storage::IStorageSource& source) {
    scan::ScanConfiguration configuration;
    configuration.mode = ScanMode::Quick;
    scan::ScanRunOptions options;
    options.workerThreads = 4;
    options.progressInterval = std::chrono::milliseconds{0};
    std::vector<evaluation::EvaluatedCandidate> candidates;
    scan::ScanCoordinator coordinator(source, formatsOnce(), mediaOnce(), configuration, options);
    const Result<scan::ScanSummary> summary = coordinator.run([&](const scan::ScanUpdate& update) {
        candidates.insert(candidates.end(), update.candidates.begin(), update.candidates.end());
        return success();
    });
    EXPECT_TRUE(summary.ok()) << (summary.ok() ? std::string() : describe(summary.error()));
    if (summary.ok()) {
        EXPECT_EQ(summary->outcome, scan::ScanOutcome::Completed);
    }
    return candidates;
}

// Every original is a candidate of its name, its bytes beyond `beyond`, with the original's SHA-256.
void expectFound(const std::vector<evaluation::EvaluatedCandidate>& candidates,
                 const std::map<std::string, Bytes>& originals, std::uint64_t beyond) {
    for (const auto& [name, bytes] : originals) {
        SCOPED_TRACE(name);
        const evaluation::EvaluatedCandidate* found = nullptr;
        for (const evaluation::EvaluatedCandidate& candidate : candidates) {
            if (candidate.data.filename == name) {
                found = &candidate;
            }
        }
        ASSERT_NE(found, nullptr);
        ASSERT_TRUE(found->sourceOffset().has_value());
        EXPECT_GT(*found->sourceOffset(), beyond);
        ASSERT_TRUE(found->identity.sha256.has_value());
        EXPECT_EQ(*found->identity.sha256, sha256(bytes));
    }
}

TEST(LargeDriveTest, VolumesBeyondTwoTibOnASixteenTibGptDiskAreFound) {
    constexpr std::uint64_t kSectors = 16 * kTiB / kSector;
    std::map<std::string, Bytes> originals;
    const std::vector<std::pair<std::uint64_t, Bytes>> volumes = {
        {3 * kTiB / kSector, fat32Volume(originals)},
        {8 * kTiB / kSector, exfatVolume(originals)},
        {15 * kTiB / kSector, ntfsVolume(originals)},
    };
    GptSpec spec;
    std::uint64_t unique = 1;
    for (const auto& [lba, bytes] : volumes) {
        GptPartitionSpec entry;
        entry.type = partition::gpt_types::kMicrosoftBasicData;
        entry.unique = *partition::Guid::parse("FA2D15C0-0000-4000-8000-00000000000" + std::to_string(unique++));
        entry.firstLba = lba;
        entry.lastLba = lba + bytes.size() / kSector - 1;
        entry.name = u"Far";
        spec.partitions.push_back(entry);
    }
    const GptRegions gpt = gptRegions(kSectors, spec);
    VirtualSource source(kSectors * kSector);
    source.plant(0, gpt.head);
    source.plant(gpt.tailLba * kSector, gpt.tail);
    for (const auto& [lba, bytes] : volumes) {
        source.plant(lba * kSector, bytes);
    }
    RECOVERY_ASSERT_OK(source.open());

    const auto started = std::chrono::steady_clock::now();
    const std::vector<evaluation::EvaluatedCandidate> candidates = quickScan(source);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    RecordProperty("quick_scan_seconds", std::to_string(seconds));
    EXPECT_EQ(candidates.size(), originals.size());
    expectFound(candidates, originals, 2 * kTiB);
    // A quick scan reads metadata and files, not the disk: a fraction of a gigabyte at most.
    EXPECT_LT(source.stats().bytes, 1ULL << 30);
}

TEST(LargeDriveTest, AnMbrPartitionAcrossTheTwoTibBoundaryIsFound) {
    std::map<std::string, Bytes> originals;
    const Bytes volume = fat32Volume(originals);
    const std::uint64_t volumeSectors = volume.size() / kSector;
    // It starts below sector 2^32, the last an MBR entry can name, and ends beyond it.
    const std::uint64_t start = 0x100000000ULL - volumeSectors / 2;
    ASSERT_GT(start + volumeSectors, 0x100000000ULL);
    VirtualSource source(3 * kTiB);
    Bytes mbr(kSector);
    writeMbrSector(mbr, kSector, 0,
                   {{0x00, 0x0C, static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(volumeSectors)}});
    source.plant(0, mbr);
    source.plant(start * kSector, volume);
    RECOVERY_ASSERT_OK(source.open());
    const std::vector<evaluation::EvaluatedCandidate> candidates = quickScan(source);
    EXPECT_EQ(candidates.size(), originals.size());
    expectFound(candidates, originals, (0x100000000ULL - volumeSectors) * kSector);
}

// ---------------------------------------------------------------------------
// A real file of tens of gigabytes (opt-in)
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path> largeImageFolder() {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"RECOVERY_LARGE_IMAGE_DIR") != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path folder(value);
    std::free(value);
    return folder;
}

std::uint64_t largeImageGib() {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"RECOVERY_LARGE_IMAGE_GIB") != 0 || value == nullptr) {
        return 64;
    }
    const std::uint64_t gib = std::wcstoull(value, nullptr, 10);
    std::free(value);
    return gib == 0 ? 64 : gib;
}

// Writes `bytes` at `offset` of the open file.
bool writeAt(HANDLE file, std::uint64_t offset, const Bytes& bytes) {
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written = 0;
    return ::SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
           ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
           written == bytes.size();
}

TEST(LargeDriveTest, ASparseImageFileOfTensOfGibIsScannedDeep) {
    const std::optional<std::filesystem::path> folder = largeImageFolder();
    if (!folder.has_value()) {
        GTEST_SKIP() << "set RECOVERY_LARGE_IMAGE_DIR to a folder with room for a sparse image file";
    }
    const std::uint64_t size = largeImageGib() << 30;
    std::filesystem::create_directories(*folder);
    const std::filesystem::path image = *folder / "large-drive-test.img";
    std::filesystem::remove(image);
    std::map<std::string, Bytes> originals;
    const Bytes volume = fat32Volume(originals);
    // Files no partition holds, near the end: found by carving.
    const Bytes lateJpeg = photo(74);
    const Bytes latePng = makePng({});
    {
        HANDLE file = ::CreateFileW(image.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        DWORD returned = 0;
        const bool sparse = ::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
        Bytes mbr(kSector);
        writeMbrSector(mbr, kSector, 0,
                       {{0x00, 0x0C, 2048, static_cast<std::uint32_t>(volume.size() / kSector)}});
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(size);
        const bool written = writeAt(file, 0, mbr) && writeAt(file, 2048 * kSector, volume) &&
                             writeAt(file, size - 64 * kMiB, lateJpeg) && writeAt(file, size - 8 * kMiB, latePng) &&
                             ::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && ::SetEndOfFile(file);
        ::CloseHandle(file);
        ASSERT_TRUE(sparse) << "the folder's filesystem does not take sparse files";
        ASSERT_TRUE(written);
    }

    api::ApiOptions options;
    options.sessionsRoot = *folder / "sessions";
    Result<std::unique_ptr<api::RecoveryApi>> created = api::RecoveryApi::create(options);
    ASSERT_TRUE(created.ok());
    api::ScanSettings settings;
    settings.alignment = 512;
    const auto started = std::chrono::steady_clock::now();
    Result<std::string> session = (*created)->startScan(api::SourceRef::imageFile(image), settings);
    ASSERT_TRUE(session.ok()) << describe(session.error());
    Result<api::Progress> done = (*created)->waitForOperation(*session, std::chrono::hours(6));
    ASSERT_TRUE(done.ok());
    ASSERT_EQ(done->state, api::OperationState::Completed);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    PROCESS_MEMORY_COUNTERS counters{};
    ASSERT_TRUE(::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)));
    RecordProperty("image_gib", std::to_string(size >> 30));
    RecordProperty("deep_scan_seconds", std::to_string(seconds));
    RecordProperty("peak_working_set_mib", std::to_string(counters.PeakWorkingSetSize >> 20));
    std::printf("[ large    ] %llu GiB deep scan: %.1f s (%.0f MB/s), peak working set %llu MiB\n",
                static_cast<unsigned long long>(size >> 30), seconds, static_cast<double>(size) / 1e6 / seconds,
                static_cast<unsigned long long>(counters.PeakWorkingSetSize >> 20));

    std::map<std::string, std::string> found;
    api::CandidateQuery query;
    query.count = api::CandidateQuery::kMaxPage;
    Result<api::CandidatePage> page = (*created)->getCandidates(*session, query);
    ASSERT_TRUE(page.ok());
    for (const api::CandidateInfo& candidate : page->candidates) {
        found[candidate.sha256] = candidate.name;
    }
    for (const auto& [name, bytes] : originals) {
        EXPECT_TRUE(found.contains(sha256(bytes).hex())) << name;
    }
    EXPECT_TRUE(found.contains(sha256(lateJpeg).hex()));
    EXPECT_TRUE(found.contains(sha256(latePng).hex()));
    created->reset();
    std::error_code ec;
    std::filesystem::remove(image, ec);
    std::filesystem::remove_all(*folder / "sessions", ec);
}

}  // namespace
}  // namespace recovery::test::corpus
