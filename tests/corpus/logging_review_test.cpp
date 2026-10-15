// P20 logging review: what a scan and a recovery job log, at the most verbose
// level, on a card whose files hold a recognisable secret. Plan section 26:
// scan start and end, the source, filesystem detection, read errors and bad
// regions, candidates, validation and recovery failures, performance
// statistics (section 34's metrics); never the content of a file.

#include "carving/format_registry.hpp"
#include "diagnostics/logger.hpp"
#include "formats/archive_formats.hpp"
#include "formats/audio_formats.hpp"
#include "formats/document_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"
#include "validation/deflate_decoder.hpp"
#include "validation/media_validator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace recovery::test::corpus {
namespace {

using Bytes = std::vector<std::byte>;
using diagnostics::LogLevel;
using diagnostics::LogRecord;

constexpr std::string_view kSecret = "SECRET-CONTENT-7f3a9c";

Bytes secretText() {
    std::string text;
    for (int i = 0; i < 40; ++i) {
        text += std::string(kSecret) + " line " + std::to_string(i) + "\r\n";
    }
    Bytes bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

// A JPEG whose comment segment holds the secret: content a validator reads.
Bytes secretPhoto() {
    JpegOptions options;
    options.width = 48;
    options.height = 32;
    options.comment = std::string(kSecret);
    return makeJpeg(options);
}

const LogRecord* find(const std::vector<LogRecord>& records, std::string_view message) {
    for (const LogRecord& record : records) {
        if (record.message == message) {
            return &record;
        }
    }
    return nullptr;
}

bool hasField(const LogRecord& record, std::string_view key) {
    return std::any_of(record.fields.begin(), record.fields.end(),
                       [&](const diagnostics::LogField& field) { return field.key == key; });
}

TEST(LoggingReviewTest, ScansAndRecoveriesLogWhatThePlanAsksAndNoContent) {
    Fat32BuilderOptions geometry;
    geometry.clusterCount = 2048;
    Fat32ImageBuilder builder(geometry);
    (void)builder.addFile(Fat32ImageBuilder::root(), "NOTES.TXT", secretText());
    (void)builder.addFile(Fat32ImageBuilder::root(), "PHOTO.JPG", secretPhoto());
    const auto deleted = builder.addFile(Fat32ImageBuilder::root(), "OLD.JPG", secretPhoto());
    builder.deleteEntry(deleted);
    // A broken photo: a validation failure to log.
    Bytes broken = secretPhoto();
    broken.resize(broken.size() / 2);
    (void)builder.addFile(Fat32ImageBuilder::root(), "BROKEN.JPG", broken);
    MemoryStorageSource source(builder.build());
    // A bad sector inside NOTES.TXT's data: a read error to log.
    source.addBadSector(builder.clusterOffset(3) / 512);
    RECOVERY_ASSERT_OK(source.open());

    auto sink = std::make_shared<diagnostics::MemorySink>();
    diagnostics::Logger logger(LogLevel::Trace);
    logger.addSink(sink);

    carving::FormatRegistry formats;
    validation::MediaValidatorRegistry media;
    RECOVERY_ASSERT_OK(formats::registerImageFormats(formats));
    RECOVERY_ASSERT_OK(formats::registerAudioFormats(formats));
    RECOVERY_ASSERT_OK(formats::registerVideoFormats(formats));
    RECOVERY_ASSERT_OK(formats::registerArchiveFormats(formats, validation::deflateDecoder()));
    RECOVERY_ASSERT_OK(formats::registerDocumentFormats(formats, validation::deflateDecoder()));
    RECOVERY_ASSERT_OK(validation::registerMediaValidators(media));
    scan::ScanRunOptions options;
    options.workerThreads = 2;
    options.logger = &logger;
    std::vector<evaluation::EvaluatedCandidate> candidates;
    scan::ScanCoordinator coordinator(source, formats, media, scan::ScanConfiguration{}, options);
    RECOVERY_ASSERT_OK(coordinator.run([&](const scan::ScanUpdate& update) {
        candidates.insert(candidates.end(), update.candidates.begin(), update.candidates.end());
        return success();
    }));
    TempDir folder;
    scan::RecoveryJobOptions jobOptions;
    jobOptions.logger = &logger;
    scan::RecoveryJob job(source, folder / "out", jobOptions);
    RECOVERY_ASSERT_OK(job.run(candidates, [](const scan::RecoveryJobUpdate&) { return success(); }));

    const std::vector<LogRecord> records = sink->records();
    ASSERT_FALSE(records.empty());

    // Scan start: the source and the settings.
    const LogRecord* started = find(records, "scan started");
    ASSERT_NE(started, nullptr);
    for (const std::string_view key : {"mode", "source", "source_type", "size", "sector_size", "known_bad_regions"}) {
        EXPECT_TRUE(hasField(*started, key)) << key;
    }
    // Filesystem detection.
    const LogRecord* volume = find(records, "volume scanned");
    ASSERT_NE(volume, nullptr);
    EXPECT_TRUE(hasField(*volume, "filesystem"));
    EXPECT_TRUE(hasField(*volume, "candidates"));
    // Read errors and bad regions.
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const LogRecord& record) {
        return record.message.find("unreadable") != std::string::npos;
    }));
    // Candidates and validation failures.
    EXPECT_NE(find(records, "evaluation ended"), nullptr);
    // Scan end: section 34's metrics.
    const LogRecord* ended = find(records, "scan ended");
    ASSERT_NE(ended, nullptr);
    for (const std::string_view key :
         {"bytes_scanned", "bytes_read", "scan_speed_bps", "files_found", "candidates", "validation_failures",
          "duplicates", "unreadable_bytes", "elapsed_ms"}) {
        EXPECT_TRUE(hasField(*ended, key)) << key;
    }
    // Recovery: each file, and the job's totals.
    EXPECT_NE(find(records, "recovered file"), nullptr);
    const LogRecord* jobEnded = find(records, "recovery job ended");
    ASSERT_NE(jobEnded, nullptr);
    for (const std::string_view key : {"recovered", "failed", "bytes", "unreadable_bytes", "elapsed_ms"}) {
        EXPECT_TRUE(hasField(*jobEnded, key)) << key;
    }

    // No record holds the content of a file.
    for (const LogRecord& record : records) {
        const std::string line = diagnostics::formatRecord(record);
        EXPECT_EQ(line.find(kSecret), std::string::npos) << line;
    }
}

}  // namespace
}  // namespace recovery::test::corpus
