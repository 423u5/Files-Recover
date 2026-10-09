// Every command of the CLI (P18), on the scan tests' card written as an image
// file, on a disk with three partitions, and on simulated physical disks:
//
//  * inspect: the source, its partition table and every volume's filesystem;
//  * image: a byte-for-byte copy, and unreadable sectors zero-filled and
//    listed, which later scans of the image take into account;
//  * scan: quick and deep scans deliver what the engine delivers, with the
//    settings given, in the sessions folder given;
//  * recover: end to end, the files the card was made from come back byte for
//    byte; filters; one folder converges, and nothing is written twice;
//  * report: text, JSON (checked against the engine), lists of sessions, a
//    session in use, and what a recovered file lacks (L42).

#include "cli/format.hpp"
#include "cli_test_support.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "imaging/image_metadata.hpp"
#include "metadata/candidate_metadata.hpp"
#include "recovery/text.hpp"
#include "session/recovery_session.hpp"
#include "support/audio_builders.hpp"
#include "support/image_builders.hpp"
#include "support/mp4_builders.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>

namespace recovery::cli {
namespace {

using test::arg;
using test::Bytes;
using test::Cli;
using test::CliResult;
namespace json = test::json;

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

class CliCommandsTest : public ::testing::Test {
protected:
    // Scans `source` (deep unless `extra` says otherwise) and returns the session's id.
    std::string scan(const std::filesystem::path& source, std::vector<std::string> extra = {},
                     ExitCode expected = ExitCode::Success) {
        std::vector<std::string> line = {"scan", "--source", arg(source)};
        line.insert(line.end(), extra.begin(), extra.end());
        const CliResult result = cli.run(std::move(line));
        EXPECT_EQ(result.code, expected) << result;
        const std::optional<std::string> id = test::sessionIdOf(result.out);
        EXPECT_TRUE(id.has_value()) << result;
        return id.value_or(std::string());
    }

    session::SessionInfo infoOf(const std::string& id, const std::filesystem::path& root) const {
        Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(root / id);
        EXPECT_TRUE(opened.ok()) << (opened.ok() ? std::string() : describe(opened.error()));
        return opened.ok() ? (*opened)->info() : session::SessionInfo{};
    }
    session::SessionInfo infoOf(const std::string& id) const { return infoOf(id, sessions); }

    json::Value reportJson(const std::string& id) {
        const CliResult result = cli.run({"report", "--session", id, "--format", "json"});
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        std::string error;
        std::optional<json::Value> value = json::parse(result.out, &error);
        EXPECT_TRUE(value.has_value()) << error << "\n" << result.out;
        return value.value_or(json::Value{nullptr});
    }

    ::recovery::test::TempDir dir;
    std::filesystem::path sessions = dir.path() / "sessions";
    Cli cli{sessions};
    std::filesystem::path image = test::writeImage(dir.path() / "card.img", test::card());
};

// ---------------------------------------------------------------------------
// inspect
// ---------------------------------------------------------------------------

TEST_F(CliCommandsTest, InspectShowsTheCard) {
    const CliResult result = cli.run({"inspect", "--source", arg(image)});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    for (const std::string& line :
         {"Source:          disk image " + arg(image), "Size:            " + formatBytes(test::card().size()),
          std::string("Sector size:     512 bytes"), std::string("Access:          read-only"),
          std::string("Partition table: none: sector 0 holds a volume boot record (the source is one volume)"),
          std::string("Volume 1: 2.0 MiB at offset 0"), std::string("  Filesystem:    FAT32"),
          std::string("  Label:         TESTVOL"), std::string("  Serial number: 1234-ABCD"),
          std::string("  Cluster size:  512 bytes (4,096 clusters, 512-byte sectors)")}) {
        EXPECT_TRUE(contains(result.out, line)) << line << "\n" << result;
    }
    EXPECT_TRUE(result.err.empty()) << result;
    // Only read: nothing new next to the image, no sessions.
    EXPECT_EQ(test::filesBelow(dir.path()).size(), 1u);
    EXPECT_FALSE(std::filesystem::exists(sessions));
}

TEST_F(CliCommandsTest, InspectShowsEveryVolume) {
    const std::filesystem::path disk = test::writeImage(dir.path() / "disk.img", test::threeVolumeDisk());
    const CliResult result = cli.run({"inspect", "--source", arg(disk)});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    std::size_t at = 0;
    // In this order: the table, then each volume and its filesystem.
    for (const std::string_view part :
         {"Partition table: MBR, 3 partitions", "Volume 1:", "(partition 0)", "Filesystem:    FAT32",
          "Label:         FATVOL", "Volume 2:", "(partition 1)", "Filesystem:    exFAT", "Label:         EXVOL",
          "Serial number: 0BAD-F00D", "Volume 3:", "(partition 2)", "Filesystem:    NTFS", "Label:         NTVOL",
          "Serial number: 1122334455667788"}) {
        const std::size_t found = result.out.find(part, at);
        ASSERT_NE(found, std::string::npos) << part << "\n" << result;
        at = found + part.size();
    }
}

TEST_F(CliCommandsTest, InspectOfDataWithoutAFilesystem) {
    const std::filesystem::path noise =
        test::writeImage(dir.path() / "noise.img", ::recovery::test::makePattern(1024 * 1024, 77));
    const CliResult result = cli.run({"inspect", "--source", arg(noise)});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    EXPECT_TRUE(contains(result.out, "Partition table: none recognised (the source is read as one volume)")) << result;
    EXPECT_TRUE(contains(result.out, "Filesystem:    none the engine reads")) << result;
    EXPECT_TRUE(contains(result.out, "can only be found by carving (recovery scan --mode deep)")) << result;
}

TEST_F(CliCommandsTest, InspectOfAPhysicalDisk) {
    const test::SimulatedDisk disk = test::simulatedDisk(test::card());
    cli.environment().diskOpener = disk.opener;
    const CliResult result = cli.run({"inspect", "--source", "\\\\.\\PhysicalDrive7"});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    EXPECT_TRUE(contains(result.out, "Source:          physical disk 7 (Simulated Card Reader)")) << result;
    EXPECT_TRUE(contains(result.out, "Device:          \\\\.\\PhysicalDrive7")) << result;
    EXPECT_TRUE(contains(result.out, "Removable:       yes")) << result;
    EXPECT_TRUE(contains(result.out, "Filesystem:    FAT32")) << result;
    const std::vector<::recovery::test::MockDeviceOpener::Call> calls = disk.opener->calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.front().path, std::filesystem::path(L"\\\\.\\PhysicalDrive7"));
    EXPECT_EQ(calls.front().kind, storage::DeviceKind::PhysicalDisk);
}

// ---------------------------------------------------------------------------
// image
// ---------------------------------------------------------------------------

TEST_F(CliCommandsTest, ImageCopiesASourceByteForByte) {
    const std::filesystem::path copy = dir.path() / "copy.img";
    const CliResult result = cli.run({"image", "--source", arg(image), "--output", arg(copy), "--block-size", "64K"});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    EXPECT_TRUE(::recovery::test::readFile(copy) == test::card());
    const Result<imaging::ImageMetadata> metadata = imaging::readImageMetadata(imaging::metadataPathFor(copy));
    ASSERT_TRUE(metadata.ok());
    EXPECT_EQ(metadata->state, imaging::ImageState::Completed);
    EXPECT_EQ(metadata->bytesCompleted, test::card().size());
    EXPECT_EQ(metadata->blockSize, 64u * 1024);
    EXPECT_TRUE(metadata->badRegions.empty());
    EXPECT_TRUE(contains(result.out, "State:           completed")) << result;
    EXPECT_TRUE(contains(result.out, "Unreadable:      0 bytes in 0 regions")) << result;
    EXPECT_TRUE(contains(result.err, "Imaging  ")) << result;

    // The image is a source like any other, with its metadata shown.
    const CliResult inspected = cli.run({"inspect", "--source", arg(copy)});
    ASSERT_EQ(inspected.code, ExitCode::Success) << inspected;
    EXPECT_TRUE(contains(inspected.out, "Image metadata:  completed")) << inspected;
    EXPECT_TRUE(contains(inspected.out, "Imaged from:   disk image " + arg(image))) << inspected;
}

TEST_F(CliCommandsTest, ImagingADiskWithBadSectors) {
    constexpr std::uint64_t kBad = 65536;
    const test::SimulatedDisk disk = test::simulatedDisk(test::card(), {{kBad, kBad + 1024}});
    cli.environment().diskOpener = disk.opener;
    const std::filesystem::path copy = dir.path() / "disk.img";
    const CliResult result = cli.run({"image", "--source", "\\\\.\\PhysicalDrive7", "--output", arg(copy)});
    ASSERT_EQ(result.code, ExitCode::Incomplete) << result;
    EXPECT_TRUE(contains(result.out, "Unreadable:      1,024 bytes (1.0 KiB) in 1 region")) << result;
    EXPECT_TRUE(contains(result.err, "of the source could not be read; the image holds zeros there")) << result;

    Bytes expected = test::card();
    std::fill_n(expected.begin() + static_cast<std::ptrdiff_t>(kBad), 1024, std::byte{0});
    EXPECT_TRUE(::recovery::test::readFile(copy) == expected);
    const Result<imaging::ImageMetadata> metadata = imaging::readImageMetadata(imaging::metadataPathFor(copy));
    ASSERT_TRUE(metadata.ok());
    ASSERT_EQ(metadata->badRegions.size(), 1u);
    EXPECT_EQ(metadata->badRegions.front(), (storage::BadRegion{kBad, 1024, 23}));

    // A scan of the image knows those zeros are not data, and that the
    // image lacks what they stand for.
    const CliResult scanned = cli.run({"scan", "--source", arg(copy)});
    EXPECT_EQ(scanned.code, ExitCode::Incomplete) << scanned;
    EXPECT_TRUE(contains(scanned.err, "The image's metadata lists 1 unreadable regions (1.0 KiB)")) << scanned;
    EXPECT_TRUE(contains(scanned.err, "1,024 bytes (1.0 KiB) of the image's source could not be read when it was "
                                      "imaged"))
        << scanned;
    const std::optional<std::string> id = test::sessionIdOf(scanned.out);
    ASSERT_TRUE(id.has_value());
    const session::SessionInfo info = infoOf(*id);
    ASSERT_EQ(info.configuration.knownBadRegions.size(), 1u);
    EXPECT_EQ(info.configuration.knownBadRegions.front(), (storage::BadRegion{kBad, 1024, 23}));
}

// ---------------------------------------------------------------------------
// scan
// ---------------------------------------------------------------------------

TEST_F(CliCommandsTest, DeepScanDeliversWhatTheEngineDelivers) {
    const CliResult result = cli.run({"scan", "--source", arg(image), "--mode", "deep"});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    const std::optional<std::string> id = test::sessionIdOf(result.out);
    ASSERT_TRUE(id.has_value()) << result;
    EXPECT_TRUE(result.out.starts_with("Session " + *id + "\n")) << result;
    EXPECT_TRUE(std::filesystem::exists(sessions / *id / "session.journal"));
    const std::vector<std::string> expected = test::expectedCard(ScanMode::Deep);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(test::sessionCandidates(sessions, *id), expected);

    for (const std::string& line :
         {std::string("Scan:            deep, completed in "),
          std::string("Candidates:      10: 8 COMPLETE, 2 CORRUPTED"),
          std::string("Kinds:           6 image, 2 audio, 2 video"), std::string("Duplicates:      2"),
          std::string("Found:           7 files in filesystem metadata, 10 carves, 2 MP4 candidates"),
          "Next:            recovery report --session " + *id,
          "                 recovery recover --session " + *id + " --output FOLDER"}) {
        EXPECT_TRUE(contains(result.out, line)) << line << "\n" << result;
    }
    // Progress, stage by stage.
    for (const std::string_view stage :
         {"Scanning 1/7 volumes", "Scanning 4/7 source pass", "Scanning 6/7 fragments", "Scanning 7/7 evaluation"}) {
        EXPECT_TRUE(contains(result.err, stage)) << stage;
    }
}

TEST_F(CliCommandsTest, QuickScan) {
    const CliResult result = cli.run({"scan", "--source", arg(image), "--mode", "quick"});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    const std::optional<std::string> id = test::sessionIdOf(result.out);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(test::sessionCandidates(sessions, *id), test::expectedCard(ScanMode::Quick));
    EXPECT_TRUE(contains(result.out, "Scan:            quick, completed")) << result;
    EXPECT_TRUE(contains(result.err, "Scanning 1/2 volumes")) << result;
    EXPECT_TRUE(contains(result.err, "Scanning 2/2 evaluation")) << result;
    EXPECT_FALSE(contains(result.err, "source pass")) << result;
}

TEST_F(CliCommandsTest, ScanSettingsAreKeptInTheSession) {
    const std::string id = scan(image, {"--no-carving", "--no-mp4", "--no-fragments", "--deleted-only", "--alignment",
                                        "512", "--sector-retries", "0", "--no-media", "--workers", "2",
                                        "--block-size", "64K", "--max-hits", "1000"});
    const session::SessionInfo info = infoOf(id);
    const scan::ScanConfiguration& c = info.configuration;
    EXPECT_EQ(c.mode, ScanMode::Deep);
    EXPECT_FALSE(c.carving);
    EXPECT_FALSE(c.mp4);
    EXPECT_FALSE(c.fragments);
    EXPECT_FALSE(c.includeActive);
    EXPECT_TRUE(c.includeDeleted);
    EXPECT_EQ(c.alignment, 512u);
    EXPECT_EQ(c.sectorRetryCount, 0u);
    EXPECT_EQ(c.maxHits, 1000u);
    EXPECT_FALSE(c.media);
    EXPECT_FALSE(info.playability);
    EXPECT_TRUE(c.sha256);
    // Only the two deleted files of the card are candidates.
    EXPECT_EQ(info.scan.metrics.candidates, 2u);
}

TEST_F(CliCommandsTest, ScanInAnotherSessionsFolder) {
    const std::filesystem::path other = dir.path() / "my sessions";
    const CliResult result = cli.run({"scan", "--source", arg(image), "--sessions-dir", arg(other)});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    const std::optional<std::string> id = test::sessionIdOf(result.out);
    ASSERT_TRUE(id.has_value());
    EXPECT_TRUE(std::filesystem::exists(other / *id / "session.journal"));
    EXPECT_FALSE(std::filesystem::exists(sessions));
    // The commands that go on name the folder, quoted for its space.
    EXPECT_TRUE(contains(result.out, "recovery report --session " + *id + " --sessions-dir \"" + arg(other) + "\""))
        << result;
}

// ---------------------------------------------------------------------------
// recover
// ---------------------------------------------------------------------------

TEST_F(CliCommandsTest, RecoverWritesTheFilesTheCardWasMadeFrom) {
    const std::string id = scan(image);
    const std::filesystem::path out = dir.path() / "Recovered";
    const CliResult result = cli.run({"recover", "--session", id, "--output", arg(out)});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    EXPECT_TRUE(contains(result.out, "Destination:     " + arg(out))) << result;
    EXPECT_TRUE(contains(result.out, "Selected:        10 of 10 candidates")) << result;
    EXPECT_TRUE(contains(result.out, "Recovered:       10 files")) << result;
    EXPECT_TRUE(contains(result.err, "Recovering 10 files to " + arg(out) + " (recovery job 1)")) << result;

    const std::map<std::string, Bytes> files = test::filesBelow(out);
    ASSERT_EQ(files.size(), 10u);
    for (const auto& [name, bytes] : test::cardOriginals()) {
        const auto found = files.find(name);
        ASSERT_NE(found, files.end()) << name;
        EXPECT_TRUE(found->second == bytes) << name;
    }
    // The files only carving finds: a GIF, an MP3 and a copy of the MP4.
    std::multiset<Bytes> carved;
    for (const auto& [name, bytes] : files) {
        if (name.starts_with("recovered_")) {
            carved.insert(bytes);
        }
    }
    EXPECT_EQ(carved, (std::multiset<Bytes>{::recovery::test::makeGif({}), ::recovery::test::makeMp3({}),
                                            ::recovery::test::makeMp4({}).bytes}));

    // Again: nothing is written twice.
    const CliResult again = cli.run({"recover", "--session", id, "--output", arg(out)});
    ASSERT_EQ(again.code, ExitCode::Success) << again;
    EXPECT_TRUE(contains(again.err, "Nothing new to recover")) << again;
    EXPECT_TRUE(contains(again.out, "Recovered:       10 files")) << again;
    EXPECT_EQ(test::filesBelow(out), files);
    EXPECT_EQ(infoOf(id).jobs.size(), 1u);
}

TEST_F(CliCommandsTest, RecoverFilters) {
    const std::string id = scan(image);
    const auto recover = [&](std::string_view folder, std::vector<std::string> filters) {
        std::vector<std::string> line = {"recover", "--session", id, "--output", arg(dir.path() / folder)};
        line.insert(line.end(), filters.begin(), filters.end());
        const CliResult result = cli.run(std::move(line));
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        std::set<std::string> names;
        for (const auto& [name, bytes] : test::filesBelow(dir.path() / folder)) {
            names.insert(name);
        }
        return names;
    };
    EXPECT_EQ(recover("images", {"--kind", "image"}),
              (std::set<std::string>{"COPY.JPG", "PHOTO.JPG", "PICTURE.PNG", "_LD.JPG", "_RAG.JPG",
                                     "recovered_000008.gif"}));
    EXPECT_EQ(recover("av", {"--kind", "audio,video"}),
              (std::set<std::string>{"CLIP.MP4", "SOUND.WAV", "recovered_000009.mp3", "recovered_000010.mp4"}));
    EXPECT_EQ(recover("ids", {"--ids", "1,3-4"}), (std::set<std::string>{"CLIP.MP4", "PHOTO.JPG", "SOUND.WAV"}));
    const std::set<std::string> unique = recover("unique", {"--skip-duplicates"});
    EXPECT_EQ(unique.size(), 8u);
    EXPECT_FALSE(unique.contains("COPY.JPG"));
    EXPECT_FALSE(unique.contains("recovered_000010.mp4"));
    EXPECT_EQ(recover("corrupted", {"--condition", "corrupted"}),
              (std::set<std::string>{"CLIP.MP4", "recovered_000010.mp4"}));
    EXPECT_EQ(recover("both", {"--condition", "complete", "--kind", "video"}), (std::set<std::string>{}));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "both"));

    // The images' folder takes the rest without writing an image again.
    const CliResult rest = cli.run({"recover", "--session", id, "--output", arg(dir.path() / "images")});
    ASSERT_EQ(rest.code, ExitCode::Success) << rest;
    EXPECT_TRUE(contains(rest.err, "Recovering 4 files")) << rest;
    EXPECT_EQ(test::filesBelow(dir.path() / "images").size(), 10u);
    const session::SessionInfo info = infoOf(id);
    EXPECT_EQ(info.jobs.size(), 6u);
}

TEST_F(CliCommandsTest, EachFolderGetsItsOwnCopy) {
    const std::string id = scan(image);
    for (const std::string_view folder : {"first", "second"}) {
        const CliResult result = cli.run({"recover", "--session", id, "--output", arg(dir.path() / folder)});
        ASSERT_EQ(result.code, ExitCode::Success) << result;
        EXPECT_EQ(test::filesBelow(dir.path() / folder).size(), 10u) << folder;
    }
    // The same folder written differently ("FIRST\", relative) is the same folder.
    CliResult same;
    {
        struct WorkingDirectory {
            std::filesystem::path previous = std::filesystem::current_path();
            ~WorkingDirectory() { std::filesystem::current_path(previous); }
        } restore;
        std::filesystem::current_path(dir.path());
        same = cli.run({"recover", "--session", id, "--output", "FIRST\\"});
    }
    ASSERT_EQ(same.code, ExitCode::Success) << same;
    EXPECT_TRUE(contains(same.err, "Nothing new to recover")) << same;
    EXPECT_EQ(infoOf(id).jobs.size(), 2u);
}

// ---------------------------------------------------------------------------
// report
// ---------------------------------------------------------------------------

TEST_F(CliCommandsTest, TextReport) {
    const std::string id = scan(image);
    const std::filesystem::path out = dir.path() / "out";
    ASSERT_EQ(cli.run({"recover", "--session", id, "--output", arg(out)}).code, ExitCode::Success);
    const CliResult result = cli.run({"report", "--session", id});
    ASSERT_EQ(result.code, ExitCode::Success) << result;
    for (const std::string& line :
         {"Session " + id, std::string("  State:         completed"),
          std::string("Volumes (partition table: Unpartitioned)"), std::string("Candidates (10)"),
          std::string("Duplicates (2)"), std::string("1 /PHOTO.JPG (original), 5 /COPY.JPG"),
          std::string("Recovery jobs (1)"), "  Job 1 to " + arg(out),
          std::string("10 of 10 files done: 10 recovered, 0 failed")}) {
        EXPECT_TRUE(contains(result.out, line)) << line << "\n" << result;
    }
    // One row per candidate, with its condition and what recovery did.
    std::istringstream lines(result.out);
    std::size_t rows = 0;
    for (std::string line; std::getline(lines, line);) {
        if (contains(line, "  recovered  ") && (contains(line, "COMPLETE") || contains(line, "CORRUPTED"))) {
            ++rows;
        }
    }
    EXPECT_EQ(rows, 10u) << result;
    EXPECT_FALSE(contains(result.out, "Candidate 1: ")) << result;

    const CliResult details = cli.run({"report", "--session", id, "--details"});
    ASSERT_EQ(details.code, ExitCode::Success) << details;
    EXPECT_TRUE(contains(details.out, "Candidate 1: /PHOTO.JPG")) << details;
    EXPECT_TRUE(contains(details.out, "  Duplicate of:  1")) << details;
    EXPECT_TRUE(contains(details.out, "  Structural level: passed")) << details;
    EXPECT_TRUE(contains(details.out, "  Created:       2024-05-17 13:45:30 (local time)")) << details;
}

TEST_F(CliCommandsTest, JsonReportMatchesTheEngine) {
    const std::string id = scan(image);
    const std::filesystem::path out = dir.path() / "out";
    ASSERT_EQ(cli.run({"recover", "--session", id, "--output", arg(out)}).code, ExitCode::Success);
    const json::Value report = reportJson(id);
    EXPECT_EQ(report["format"].string(), "recovery-session-report");
    EXPECT_EQ(report["formatVersion"].u64(), 1u);
    EXPECT_EQ(report["session"]["id"].string(), id);
    EXPECT_EQ(report["source"]["type"].string(), "DiskImage");
    EXPECT_EQ(report["source"]["path"].string(), arg(image));
    EXPECT_EQ(report["source"]["size"].u64(), test::card().size());
    EXPECT_EQ(report["scan"]["state"].string(), "completed");
    EXPECT_EQ(report["scan"]["configuration"]["mode"].string(), "deep");
    EXPECT_EQ(report["scan"]["metrics"]["candidates"].u64(), 10u);
    EXPECT_EQ(report["volumes"][0]["filesystem"].string(), "FAT32");

    Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(sessions / id);
    ASSERT_TRUE(opened.ok());
    const std::vector<evaluation::EvaluatedCandidate> candidates = (*opened)->candidates();
    opened->reset();
    const json::Value& listed = report["candidates"];
    ASSERT_EQ(listed.size(), candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const metadata::CandidateMetadata engine = metadata::describeCandidate(candidates[i]);
        const json::Value& candidate = listed[i];
        EXPECT_EQ(candidate["id"].u64(), engine.id.value());
        EXPECT_EQ(candidate["name"].string(), engine.name);
        EXPECT_EQ(candidate["path"].string(), engine.path);
        EXPECT_EQ(candidate["size"].u64(), engine.size);
        EXPECT_EQ(candidate["condition"].string(), metadata::toString(engine.condition));
        EXPECT_EQ(candidate["sha256"].string(), engine.sha256->hex());
        EXPECT_EQ(candidate["recovery"]["state"].string(), "recovered");
        EXPECT_EQ(candidate["evidence"].size(), evaluation::explain(candidates[i]).size());
        EXPECT_EQ(candidate["duplicateOf"].isNull(), !engine.duplicateOf.has_value());
    }
    const json::Value& groups = report["duplicateGroups"];
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0]["original"].u64(), 1u);
    EXPECT_EQ(groups[0]["members"][1].u64(), 5u);
    EXPECT_EQ(groups[1]["original"].u64(), 3u);
    EXPECT_EQ(groups[1]["members"][1].u64(), 10u);

    const json::Value& job = report["jobs"][0];
    EXPECT_EQ(job["state"].string(), "completed");
    ASSERT_EQ(job["files"].size(), 10u);
    for (std::size_t i = 0; i < 10; ++i) {
        const json::Value& file = job["files"][i];
        EXPECT_TRUE(file["report"]["allBytesRead"].boolean());
        const Result<std::filesystem::path> path = pathFromUtf8(file["path"].string());
        ASSERT_TRUE(path.ok());
        EXPECT_TRUE(std::filesystem::exists(*path)) << file["path"].string();
    }

    // To a file: the same report, and an existing file is never overwritten.
    const std::filesystem::path saved = dir.path() / "report.json";
    const CliResult written = cli.run({"report", "--session", id, "--format", "json", "--output", arg(saved)});
    ASSERT_EQ(written.code, ExitCode::Success) << written;
    EXPECT_TRUE(written.out.empty());
    EXPECT_TRUE(contains(written.err, "Report written to " + arg(saved))) << written;
    const Bytes bytes = ::recovery::test::readFile(saved);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::optional<json::Value> reread = json::parse(text);
    ASSERT_TRUE(reread.has_value());
    EXPECT_EQ((*reread)["candidates"].size(), 10u);
    EXPECT_EQ((*reread)["session"]["id"].string(), id);
    const CliResult again = cli.run({"report", "--session", id, "--output", arg(saved)});
    EXPECT_EQ(again.code, ExitCode::Error) << again;
    EXPECT_TRUE(contains(again.err, "an existing file is never overwritten")) << again;
    EXPECT_TRUE(::recovery::test::readFile(saved) == bytes);
}

TEST_F(CliCommandsTest, ListingSessions) {
    const CliResult none = cli.run({"report"});
    ASSERT_EQ(none.code, ExitCode::Success) << none;
    EXPECT_TRUE(contains(none.out, "Sessions in " + arg(sessions) + ": 0")) << none;

    const std::string first = scan(image);
    const std::string second = scan(image, {"--mode", "quick"});
    const CliResult text = cli.run({"report"});
    ASSERT_EQ(text.code, ExitCode::Success) << text;
    EXPECT_TRUE(contains(text.out, "Sessions in " + arg(sessions) + ": 2")) << text;
    EXPECT_TRUE(contains(text.out, first)) << text;
    EXPECT_TRUE(contains(text.out, second)) << text;

    const CliResult listed = cli.run({"report", "--format", "json"});
    ASSERT_EQ(listed.code, ExitCode::Success) << listed;
    const std::optional<json::Value> value = json::parse(listed.out);
    ASSERT_TRUE(value.has_value()) << listed;
    EXPECT_EQ((*value)["format"].string(), "recovery-session-list");
    ASSERT_EQ((*value)["sessions"].size(), 2u);
    std::set<std::string> ids;
    for (std::size_t i = 0; i < 2; ++i) {
        ids.insert((*value)["sessions"][i]["id"].string());
        EXPECT_EQ((*value)["sessions"][i]["state"].string(), "completed");
    }
    EXPECT_EQ(ids, (std::set<std::string>{first, second}));
}

TEST_F(CliCommandsTest, ReportOfASessionInUse) {
    const std::string id = scan(image);
    Result<std::unique_ptr<session::RecoverySession>> held = session::RecoverySession::open(sessions / id);
    ASSERT_TRUE(held.ok());
    const CliResult text = cli.run({"report", "--session", id});
    ASSERT_EQ(text.code, ExitCode::Success) << text;
    EXPECT_TRUE(contains(text.err, "is in use by another recovery command: only its summary can be read now"))
        << text;
    EXPECT_TRUE(contains(text.out, "Session " + id)) << text;
    EXPECT_TRUE(contains(text.out, "Scan:          deep, completed; 10 candidates")) << text;
    const CliResult listed = cli.run({"report", "--session", id, "--format", "json"});
    ASSERT_EQ(listed.code, ExitCode::Success) << listed;
    const std::optional<json::Value> value = json::parse(listed.out);
    ASSERT_TRUE(value.has_value()) << listed;
    EXPECT_TRUE((*value)["inUse"].boolean());
    EXPECT_EQ((*value)["summary"]["id"].string(), id);
}

// L42: a recovered file whose bytes could not all be read says so, in the
// recovery's output and in the report.
TEST_F(CliCommandsTest, WhatARecoveredFileLacksIsReported) {
    // Where PHOTO.JPG's data is on the card.
    const std::string probe = scan(image);
    const json::Value probed = reportJson(probe);
    std::uint64_t photo = 0;
    for (std::size_t i = 0; i < probed["candidates"].size(); ++i) {
        if (probed["candidates"][i]["name"].string() == "PHOTO.JPG") {
            photo = probed["candidates"][i]["sourceOffset"].u64();
        }
    }
    ASSERT_NE(photo, 0u);
    const std::uint64_t bad = photo + 1024;
    const test::SimulatedDisk disk = test::simulatedDisk(test::card(), {{bad, bad + 512}});
    cli.environment().diskOpener = disk.opener;

    const std::string id = scan("\\\\.\\PhysicalDrive7", {}, ExitCode::Incomplete);
    const std::filesystem::path out = dir.path() / "out";
    const CliResult result = cli.run({"recover", "--session", id, "--output", arg(out)});
    ASSERT_EQ(result.code, ExitCode::Incomplete) << result;
    EXPECT_TRUE(contains(result.out, "Incomplete:      1 written with bytes missing or unreadable")) << result;
    EXPECT_TRUE(contains(result.out, "  1  PHOTO.JPG: 512 bytes unreadable")) << result;
    Bytes expected = test::cardOriginals().at("PHOTO.JPG");
    std::fill_n(expected.begin() + 1024, 512, std::byte{0});
    EXPECT_TRUE(test::filesBelow(out).at("PHOTO.JPG") == expected);

    const CliResult text = cli.run({"report", "--session", id});
    ASSERT_EQ(text.code, ExitCode::Success) << text;
    EXPECT_TRUE(contains(text.out, "incomplete  " + arg(out / "PHOTO.JPG") + " (512 bytes unreadable)")) << text;
    EXPECT_TRUE(contains(text.out, "Unreadable source regions (1, 512 bytes)")) << text;
    const json::Value report = reportJson(id);
    const json::Value& file = report["jobs"][0]["files"][0];
    EXPECT_EQ(file["candidate"].u64(), 1u);
    EXPECT_EQ(file["report"]["unreadableBytes"].u64(), 512u);
    EXPECT_FALSE(file["report"]["allBytesRead"].boolean());
    EXPECT_EQ(file["report"]["unreadableRegions"][0]["offset"].u64(), bad);
    EXPECT_EQ(report["unreadableRegions"][0]["offset"].u64(), bad);
    EXPECT_EQ(report["source"]["type"].string(), "PhysicalDisk");
}

TEST_F(CliCommandsTest, EngineLogFile) {
    const std::filesystem::path log = dir.path() / "run.log";
    const CliResult scanned = cli.run({"scan", "--source", arg(image), "--log", arg(log), "--quiet"});
    ASSERT_EQ(scanned.code, ExitCode::Success) << scanned;
    // --quiet: no progress, no notes.
    EXPECT_TRUE(scanned.err.empty()) << scanned;
    const Bytes first = ::recovery::test::readFile(log);
    const std::string text(reinterpret_cast<const char*>(first.data()), first.size());
    EXPECT_TRUE(contains(text, "scan started")) << text;
    EXPECT_TRUE(contains(text, "scan ended")) << text;
    // Appended to, never replaced.
    const std::optional<std::string> id = test::sessionIdOf(scanned.out);
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(cli.run({"report", "--session", *id, "--log", arg(log)}).code, ExitCode::Success);
    const Bytes second = ::recovery::test::readFile(log);
    ASSERT_GE(second.size(), first.size());
    EXPECT_TRUE(std::equal(first.begin(), first.end(), second.begin()));
}

}  // namespace
}  // namespace recovery::cli
