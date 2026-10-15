// recovery_benchmark: the engine's performance on this computer (P20), for
// development. It makes its inputs in a work folder (an image of noise, a card
// full of media files, a card of many small files), runs the engine on them as
// the CLI and the API do, and reports throughput and memory against the
// targets docs/testing/performance.md sets.
//
//   recovery_benchmark --work FOLDER [--only NAME[,NAME...]] [--noise-mib N] [--card-mib N]
//                      [--files N] [--workers N] [--json FILE] [--keep]
//   recovery_benchmark --sizes     (the sizes of what a scan keeps per file found)
//
// Benchmarks: read (sequential reads of an image file), deep-noise (a deep
// scan of noise: the content of a used card's free space, and of compressed
// media), deep-media (a deep scan of a FAT32 card full of photos, videos and
// music), quick-files (a quick scan of a card of many small files), recover
// (writing every candidate of the media card), imaging (imaging the noise
// image). Run one at a time (--only) to see each one's peak memory alone.

#include "carving/format_registry.hpp"
#include "formats/archive_formats.hpp"
#include "formats/audio_formats.hpp"
#include "formats/document_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "imaging/image_writer.hpp"
#include "recovery/sha256.hpp"
#include "recovery/text.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "storage/disk_image_source.hpp"
#include "support/audio_builders.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/mp4_samples.hpp"
#include "validation/deflate_decoder.hpp"
#include "validation/media_validator.hpp"

#include <windows.h>
#include <malloc.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Heap accounting (after P20, L212): every operator new of the process is
// counted (the engine is linked in), so a benchmark can tell the heap a scan
// keeps per file found and its peak, apart from the working set.
namespace heap {
std::atomic<std::int64_t> live{0};
std::atomic<std::int64_t> peak{0};

void added(std::size_t bytes) noexcept {
    const auto size = static_cast<std::int64_t>(bytes);
    const std::int64_t now = live.fetch_add(size) + size;
    std::int64_t seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
}

void removed(std::size_t bytes) noexcept {
    live.fetch_sub(static_cast<std::int64_t>(bytes));
}

// Restarts the peak at the live bytes now.
void resetPeak() noexcept {
    peak.store(live.load());
}
}  // namespace heap

void* operator new(std::size_t size) {
    void* block = std::malloc(size == 0 ? 1 : size);
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    heap::added(_msize(block));
    return block;
}

void operator delete(void* block) noexcept {
    if (block != nullptr) {
        heap::removed(_msize(block));
        std::free(block);
    }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    void* block = _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    heap::added(_aligned_msize(block, static_cast<std::size_t>(alignment), 0));
    return block;
}

void operator delete(void* block, std::align_val_t alignment) noexcept {
    if (block != nullptr) {
        heap::removed(_aligned_msize(block, static_cast<std::size_t>(alignment), 0));
        _aligned_free(block);
    }
}

namespace {

using namespace recovery;
using Bytes = std::vector<std::byte>;
using Clock = std::chrono::steady_clock;

struct Settings {
    std::filesystem::path work;
    std::vector<std::string> only;
    std::uint64_t noiseMib = 2048;
    std::uint64_t cardMib = 1024;
    std::uint32_t files = 20000;
    std::uint32_t workers = 4;
    std::filesystem::path json;
    bool keep = false;
};

struct Measure {
    std::string name;
    std::uint64_t bytes = 0;
    double seconds = 0;
    std::uint64_t items = 0;
    std::string itemName;
    std::uint64_t peakMib = 0;
    std::string target;
    bool met = true;
    std::string note;
};

std::uint64_t peakWorkingSetMib() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.PeakWorkingSetSize >> 20;
}

double mbPerSecond(std::uint64_t bytes, double seconds) {
    return seconds > 0 ? static_cast<double>(bytes) / 1e6 / seconds : 0;
}

void fail(const std::string& what, const Error& error) {
    throw std::runtime_error(what + ": " + describe(error));
}

const carving::FormatRegistry& formats() {
    static const carving::FormatRegistry registry = [] {
        carving::FormatRegistry made;
        const bool ok = formats::registerImageFormats(made).ok() && formats::registerAudioFormats(made).ok() &&
                        formats::registerVideoFormats(made).ok() &&
                        formats::registerArchiveFormats(made, validation::deflateDecoder()).ok() &&
                        formats::registerDocumentFormats(made, validation::deflateDecoder()).ok();
        if (!ok) {
            throw std::runtime_error("registering the formats");
        }
        return made;
    }();
    return registry;
}

const validation::MediaValidatorRegistry& media() {
    static const validation::MediaValidatorRegistry registry = [] {
        validation::MediaValidatorRegistry made;
        if (!validation::registerMediaValidators(made).ok()) {
            throw std::runtime_error("registering the media decoders");
        }
        return made;
    }();
    return registry;
}

// Writes `size` bytes made by `fill` (called with a buffer and its offset) to `path`, in 16 MiB pieces.
void writeLarge(const std::filesystem::path& path, std::uint64_t size,
                const std::function<void(std::span<std::byte>, std::uint64_t)>& fill) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Bytes buffer(16 << 20);
    for (std::uint64_t at = 0; at < size; at += buffer.size()) {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - at));
        fill(std::span<std::byte>(buffer).first(count), at);
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(count));
    }
    if (!out) {
        throw std::runtime_error("cannot write " + toUtf8(path));
    }
}

void noise(std::span<std::byte> buffer, std::uint64_t at) {
    std::uint64_t state = at * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL;
    for (std::size_t i = 0; i + 8 <= buffer.size(); i += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        std::memcpy(buffer.data() + i, &state, 8);
    }
}

std::filesystem::path noiseImage(const Settings& settings) {
    const std::filesystem::path path = settings.work / ("noise-" + std::to_string(settings.noiseMib) + "MiB.img");
    if (!std::filesystem::exists(path) || std::filesystem::file_size(path) != settings.noiseMib << 20) {
        std::cout << "making " << toUtf8(path) << "\n";
        writeLarge(path, settings.noiseMib << 20, noise);
    }
    return path;
}

// A FAT32 card with 32 KiB clusters, filled to about 90% with photos, videos,
// music and recordings in folders of 100, every tenth file deleted.
std::filesystem::path mediaCard(const Settings& settings) {
    const std::filesystem::path path = settings.work / ("media-card-" + std::to_string(settings.cardMib) + "MiB.img");
    if (std::filesystem::exists(path)) {
        return path;
    }
    std::cout << "making " << toUtf8(path) << "\n";
    std::vector<std::pair<std::string, Bytes>> pool;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        test::JpegOptions jpeg;
        jpeg.width = 1024;
        jpeg.height = 768;
        jpeg.seed = 900 + seed;
        jpeg.exifThumbnail = true;
        jpeg.exifMake = "Benchmark";
        pool.emplace_back("jpg", test::makeJpeg(jpeg));
    }
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        test::Mp3Options mp3;
        mp3.frames = 3000;
        mp3.id3v2 = 3;
        mp3.seed = 950 + seed;
        pool.emplace_back("mp3", test::makeMp3(mp3));
    }
    {
        test::WavOptions wav;
        wav.frames = 1'000'000;
        pool.emplace_back("wav", test::makeWav(wav));
    }
    // Real H.264 video: the samples of a libx264 file, repeated.
    for (const char* name : {"ffmpeg_h264_aac.mp4", "gpac_h264_aac.mp4", "mediafoundation_h264_aac.mp4"}) {
        pool.emplace_back("mp4", test::mp4_samples::named(name).data());
    }
    test::Fat32BuilderOptions options;
    options.sectorsPerCluster = 64;
    options.clusterCount = static_cast<std::uint32_t>((settings.cardMib << 20) / (32 * 1024));
    options.label = "BENCH";
    test::Fat32ImageBuilder builder(options);
    const std::uint64_t budget = (settings.cardMib << 20) * 9 / 10;
    std::uint64_t used = 0;
    std::uint32_t folder = 0;
    std::uint32_t count = 0;
    std::vector<test::Fat32ImageBuilder::Entry> toDelete;
    while (true) {
        const auto& [extension, bytes] = pool[count % pool.size()];
        const std::uint64_t clusters = (bytes.size() + 32 * 1024 - 1) / (32 * 1024);
        if (used + clusters * 32 * 1024 > budget) {
            break;
        }
        if (count % 100 == 0) {
            folder = builder.addDirectory(test::Fat32ImageBuilder::root(), "DIR" + std::to_string(count / 100))
                         .clusters.front();
        }
        const auto entry = builder.addFile(folder, "FILE" + std::to_string(count % 100) + "." + extension, bytes);
        if (count % 10 == 9) {
            toDelete.push_back(entry);
        }
        used += clusters * 32 * 1024;
        ++count;
    }
    for (const auto& entry : toDelete) {
        builder.deleteEntry(entry);
    }
    const Bytes image = builder.build();
    writeLarge(path, image.size(), [&](std::span<std::byte> buffer, std::uint64_t at) {
        std::memcpy(buffer.data(), image.data() + at, buffer.size());
    });
    std::cout << "  " << count << " files, " << toDelete.size() << " deleted\n";
    return path;
}

// A FAT32 card of `files` small PNG files in folders of 500.
std::filesystem::path filesCard(const Settings& settings) {
    const std::filesystem::path path = settings.work / ("files-card-" + std::to_string(settings.files) + ".img");
    if (std::filesystem::exists(path)) {
        return path;
    }
    std::cout << "making " << toUtf8(path) << "\n";
    test::Fat32BuilderOptions options;
    options.sectorsPerCluster = 8;
    options.clusterCount = settings.files + settings.files / 400 + 4096;
    test::Fat32ImageBuilder builder(options);
    const Bytes png = test::makePng({});
    std::uint32_t folder = 0;
    for (std::uint32_t i = 0; i < settings.files; ++i) {
        if (i % 500 == 0) {
            folder = builder.addDirectory(test::Fat32ImageBuilder::root(), "D" + std::to_string(i / 500)).clusters.front();
        }
        (void)builder.addFile(folder, "P" + std::to_string(i % 500) + ".PNG", png);
    }
    const Bytes image = builder.build();
    writeLarge(path, image.size(), [&](std::span<std::byte> buffer, std::uint64_t at) {
        std::memcpy(buffer.data(), image.data() + at, buffer.size());
    });
    return path;
}

struct ScanResult {
    std::vector<evaluation::EvaluatedCandidate> candidates;
    // Candidates delivered (also when they are not kept).
    std::uint64_t delivered = 0;
    scan::ScanSummary summary;
    double seconds = 0;
};

// The heap's peak in each stage of a scan: the progress callback sees each
// stage begin. It restarts heap::peak, so the scan's own peak is the highest
// of the stages'.
struct StagePeaks {
    std::map<scan::ScanStage, std::int64_t> peak;
    std::optional<scan::ScanStage> current;

    void enter(scan::ScanStage stage) {
        if (current == stage) {
            return;
        }
        close();
        current = stage;
        heap::resetPeak();
    }
    void close() {
        if (current.has_value()) {
            std::int64_t& seen = peak[*current];
            seen = std::max(seen, heap::peak.load());
        }
    }
    [[nodiscard]] std::int64_t highest() const {
        std::int64_t most = 0;
        for (const auto& [stage, bytes] : peak) {
            most = std::max(most, bytes);
        }
        return most;
    }
};

// keep: the sink keeps every candidate, as a user interface does; otherwise
// it only counts them (the engine's own memory alone). cache: false runs
// without the scan's read cache (a fixed cost, up to 64 MiB).
ScanResult scanImage(const std::filesystem::path& image, ScanMode mode, std::uint32_t workers, bool keep = true,
                     StagePeaks* stages = nullptr, bool cache = true) {
    storage::DiskImageSource source(image);
    if (Status opened = source.open(); !opened.ok()) {
        fail("opening " + toUtf8(image), opened.error());
    }
    scan::ScanConfiguration configuration;
    configuration.mode = mode;
    scan::ScanRunOptions options;
    options.workerThreads = workers;
    if (!cache) {
        options.cacheBlocks = 0;
    }
    if (stages != nullptr) {
        options.onProgress = [stages](const scan::ScanProgress& progress) { stages->enter(progress.stage); };
    }
    ScanResult result;
    const auto started = Clock::now();
    scan::ScanCoordinator coordinator(source, formats(), media(), configuration, options);
    Result<scan::ScanSummary> summary = coordinator.run([&](const scan::ScanUpdate& update) {
        result.delivered += update.candidates.size();
        if (keep) {
            result.candidates.insert(result.candidates.end(), update.candidates.begin(), update.candidates.end());
        }
        return success();
    });
    if (stages != nullptr) {
        stages->close();
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (!summary.ok()) {
        fail("scanning " + toUtf8(image), summary.error());
    }
    result.summary = *summary;
    return result;
}

Measure benchRead(const Settings& settings) {
    const std::filesystem::path image = noiseImage(settings);
    storage::DiskImageSource source(image);
    if (Status opened = source.open(); !opened.ok()) {
        fail("opening the image", opened.error());
    }
    Bytes buffer(1 << 20);
    const auto started = Clock::now();
    std::uint64_t total = 0;
    for (std::uint64_t at = 0; at < source.size(); at += buffer.size()) {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - at));
        if (Status read = source.readExact(ByteOffset{at}, std::span<std::byte>(buffer).first(count)); !read.ok()) {
            fail("reading", read.error());
        }
        total += count;
    }
    Measure m;
    m.name = "read";
    m.bytes = total;
    m.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    m.target = "for reference: the engine's own reads of an image file";
    return m;
}

Measure benchDeepNoise(const Settings& settings) {
    const ScanResult result = scanImage(noiseImage(settings), ScanMode::Deep, settings.workers);
    Measure m;
    m.name = "deep-noise";
    m.bytes = result.summary.metrics.bytesScanned;
    m.seconds = result.seconds;
    m.items = result.candidates.size();
    m.itemName = "candidates";
    m.target = ">= 100 MB/s (USB 3 media)";
    m.met = mbPerSecond(m.bytes, m.seconds) >= 100;
    return m;
}

Measure benchDeepMedia(const Settings& settings) {
    const ScanResult result = scanImage(mediaCard(settings), ScanMode::Deep, settings.workers);
    Measure m;
    m.name = "deep-media";
    m.bytes = result.summary.metrics.bytesScanned;
    m.seconds = result.seconds;
    m.items = result.candidates.size();
    m.itemName = "candidates";
    m.target = ">= 40 MB/s (USB 2 media: every file decoded and hashed)";
    m.met = mbPerSecond(m.bytes, m.seconds) >= 40;
    return m;
}

Measure benchQuickFiles(const Settings& settings) {
    const std::filesystem::path card = filesCard(settings);
    const std::int64_t before = heap::live.load();
    heap::resetPeak();
    StagePeaks stages;
    const ScanResult result = scanImage(card, ScanMode::Quick, settings.workers, true, &stages);
    Measure m;
    m.name = "quick-files";
    m.bytes = result.summary.metrics.bytesRead;
    m.seconds = result.seconds;
    m.items = result.candidates.size();
    m.itemName = "files";
    m.target = ">= 2000 files/s";
    m.met = m.seconds > 0 && static_cast<double>(m.items) / m.seconds >= 2000;
    // Per file found: what the candidates a user interface holds take, the
    // scan's peak (in each stage), and the engine's own peak when the
    // candidates are not kept and nothing is cached (the cache is a fixed
    // cost: per file, it weighs on small scans only).
    if (m.items > 0) {
        const auto perFile = [&](std::int64_t bytes, std::int64_t base) {
            return static_cast<double>(bytes - base) / static_cast<double>(m.items);
        };
        const auto stagesOf = [&](const StagePeaks& peaks, std::int64_t base) {
            std::string text;
            for (const auto& [stage, bytes] : peaks.peak) {
                text += std::format("{}{} {:.0f}", text.empty() ? "" : ", ", scan::toString(stage), perFile(bytes, base));
            }
            return text;
        };
        const std::int64_t kept = heap::live.load();
        const std::int64_t peak = std::max(stages.highest(), heap::peak.load());
        const std::int64_t base = heap::live.load();
        heap::resetPeak();
        StagePeaks again;
        (void)scanImage(card, ScanMode::Quick, settings.workers, false, &again, false);
        const std::int64_t alone = std::max(again.highest(), heap::peak.load());
        m.note = std::format(
            "heap kept {:.0f} B/file, peak {:.0f} B/file ({}, {:.0f} MiB);\n"
            "             engine alone, no read cache: peak {:.0f} B/file ({})",
            perFile(kept, before), perFile(peak, before), stagesOf(stages, before),
            static_cast<double>(peak - before) / static_cast<double>(1 << 20), perFile(alone, base),
            stagesOf(again, base));
    }
    return m;
}

Measure benchRecover(const Settings& settings) {
    const std::filesystem::path image = mediaCard(settings);
    const ScanResult scanned = scanImage(image, ScanMode::Quick, settings.workers);
    storage::DiskImageSource source(image);
    if (Status opened = source.open(); !opened.ok()) {
        fail("opening the card", opened.error());
    }
    const std::filesystem::path destination = settings.work / "recovered";
    std::error_code ec;
    std::filesystem::remove_all(destination, ec);
    scan::RecoveryJobOptions options;
    options.workerThreads = settings.workers;
    scan::RecoveryJob job(source, destination, options);
    const auto started = Clock::now();
    Result<scan::RecoveryJobSummary> summary =
        job.run(scanned.candidates, [](const scan::RecoveryJobUpdate&) { return success(); });
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (!summary.ok()) {
        fail("recovering", summary.error());
    }
    Measure m;
    m.name = "recover";
    m.bytes = summary->metrics.bytesRecovered;
    m.seconds = seconds;
    m.items = summary->metrics.recoveredFiles;
    m.itemName = "files";
    m.target = ">= 100 MB/s to an SSD";
    m.met = mbPerSecond(m.bytes, m.seconds) >= 100;
    if (!settings.keep) {
        std::filesystem::remove_all(destination, ec);
    }
    return m;
}

Measure benchImaging(const Settings& settings) {
    const std::filesystem::path image = noiseImage(settings);
    storage::DiskImageSource source(image);
    if (Status opened = source.open(); !opened.ok()) {
        fail("opening the image", opened.error());
    }
    const std::filesystem::path copy = settings.work / "imaged.img";
    std::error_code ec;
    std::filesystem::remove(copy, ec);
    std::filesystem::remove(copy.string() + ".imgmeta", ec);
    imaging::ImagingOptions options;
    const auto started = Clock::now();
    Result<imaging::ImagingSummary> summary = imaging::ImageWriter(source, copy, options).run();
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (!summary.ok()) {
        fail("imaging", summary.error());
    }
    Measure m;
    m.name = "imaging";
    m.bytes = source.size();
    m.seconds = seconds;
    m.target = ">= 100 MB/s (USB 3 media)";
    m.met = mbPerSecond(m.bytes, m.seconds) >= 100;
    std::filesystem::remove(copy, ec);
    std::filesystem::remove(copy.string() + ".imgmeta", ec);
    return m;
}

// The sizes of what a scan keeps per file found (L212), in this build.
void printSizes() {
    const auto line = [](const char* name, std::size_t bytes) { std::printf("%-34s %6zu bytes\n", name, bytes); };
    line("evaluation::EvaluatedCandidate", sizeof(evaluation::EvaluatedCandidate));
    line("  RecoveryCandidate (data)", sizeof(RecoveryCandidate));
    line("  FilesystemEvidence", sizeof(FilesystemEvidence));
    line("  validation::ValidationState", sizeof(validation::ValidationState));
    line("  carving::FileCandidate (carve)", sizeof(carving::FileCandidate));
    line("  Mp4Structure", sizeof(Mp4Structure));
    line("  evaluation::FragmentEvidence", sizeof(evaluation::FragmentEvidence));
    line("  evaluation::AllocationEvidence", sizeof(evaluation::AllocationEvidence));
    line("evaluation::EvaluationRecord", sizeof(evaluation::EvaluationRecord));
    line("filesystem::FileRecord", sizeof(filesystem::FileRecord));
    line("scan::VolumeRecord", sizeof(scan::VolumeRecord));
}

std::string json(const Measure& m) {
    std::ostringstream out;
    out << "{\"name\":\"" << m.name << "\",\"bytes\":" << m.bytes << ",\"seconds\":" << m.seconds
        << ",\"mb_per_s\":" << mbPerSecond(m.bytes, m.seconds) << ",\"items\":" << m.items << ",\"item\":\""
        << m.itemName << "\",\"peak_working_set_mib\":" << m.peakMib << ",\"target\":\"" << m.target
        << "\",\"met\":" << (m.met ? "true" : "false") << "}";
    return out.str();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Settings settings;
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            const auto next = [&]() -> std::wstring {
                if (i + 1 >= argc) {
                    throw std::runtime_error("an option needs a value");
                }
                return argv[++i];
            };
            if (argument == L"--work") {
                settings.work = next();
            } else if (argument == L"--only") {
                std::string list = toUtf8(std::filesystem::path(next()));
                std::stringstream stream(list);
                for (std::string name; std::getline(stream, name, ',');) {
                    settings.only.push_back(name);
                }
            } else if (argument == L"--noise-mib") {
                settings.noiseMib = std::stoull(next());
            } else if (argument == L"--card-mib") {
                settings.cardMib = std::stoull(next());
            } else if (argument == L"--files") {
                settings.files = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (argument == L"--workers") {
                settings.workers = static_cast<std::uint32_t>(std::stoul(next()));
            } else if (argument == L"--json") {
                settings.json = next();
            } else if (argument == L"--keep") {
                settings.keep = true;
            } else if (argument == L"--sizes") {
                printSizes();
                return 0;
            } else {
                std::cerr << "usage: recovery_benchmark --work FOLDER [--only NAME,...] [--noise-mib N] "
                             "[--card-mib N] [--files N] [--workers N] [--json FILE] [--keep]\n"
                             "       recovery_benchmark --sizes\n";
                return 1;
            }
        }
        if (settings.work.empty()) {
            std::cerr << "recovery_benchmark: --work FOLDER is needed (room for the images)\n";
            return 1;
        }
        std::filesystem::create_directories(settings.work);
        const std::vector<std::pair<std::string, std::function<Measure(const Settings&)>>> all = {
            {"read", benchRead},       {"deep-noise", benchDeepNoise}, {"deep-media", benchDeepMedia},
            {"quick-files", benchQuickFiles}, {"recover", benchRecover}, {"imaging", benchImaging},
        };
        bool allMet = true;
        for (const auto& [name, run] : all) {
            if (!settings.only.empty() &&
                std::find(settings.only.begin(), settings.only.end(), name) == settings.only.end()) {
                continue;
            }
            Measure m = run(settings);
            m.peakMib = peakWorkingSetMib();
            allMet = allMet && m.met;
            std::printf("%-12s %8.1f MB/s  %7.2f s  %10llu bytes", m.name.c_str(), mbPerSecond(m.bytes, m.seconds),
                        m.seconds, static_cast<unsigned long long>(m.bytes));
            if (!m.itemName.empty()) {
                std::printf("  %llu %s (%.0f/s)", static_cast<unsigned long long>(m.items), m.itemName.c_str(),
                            m.seconds > 0 ? static_cast<double>(m.items) / m.seconds : 0.0);
            }
            std::printf("  peak %llu MiB  target %s: %s\n", static_cast<unsigned long long>(m.peakMib),
                        m.target.c_str(), m.met ? "met" : "MISSED");
            if (!m.note.empty()) {
                std::printf("             %s\n", m.note.c_str());
            }
            if (!settings.json.empty()) {
                std::ofstream out(settings.json, std::ios::app);
                out << json(m) << "\n";
            }
        }
        return allMet ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "recovery_benchmark: " << error.what() << "\n";
        return 2;
    }
}
