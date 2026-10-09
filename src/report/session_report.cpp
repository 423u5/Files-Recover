#include "report/session_report.hpp"

#include "report/json_writer.hpp"
#include "report/text_format.hpp"
#include "recovery/text.hpp"
#include "recovery/version.hpp"
#include "validation/validation.hpp"

#include <format>
#include <sstream>

namespace recovery::report {

namespace {

constexpr std::string_view kReportFormat = "recovery-session-report";
constexpr std::string_view kListFormat = "recovery-session-list";

std::string serialText(std::optional<filesystem::FilesystemType> type, std::uint64_t serial) {
    if (type == filesystem::FilesystemType::Ntfs) {
        return std::format("{:016X}", serial);
    }
    return std::format("{:04X}-{:04X}", (serial >> 16) & 0xFFFFu, serial & 0xFFFFu);
}

std::string sourceText(const session::SessionSource& source) {
    return describeSource(source.type, source.path, source.diskNumber, source.vendor, source.product);
}

std::string stateText(const std::optional<session::SessionState>& state, bool interrupted) {
    if (!state.has_value()) {
        return "not started";
    }
    std::string text(session::toString(*state));
    if (interrupted) {
        text += " (interrupted: the program ended while it ran)";
    }
    return text;
}

std::string onOff(bool value, std::string_view name) {
    return std::string(value ? "" : "no ") + std::string(name);
}

std::string settingsText(const session::SessionInfo& info) {
    const scan::ScanConfiguration& c = info.configuration;
    std::string text;
    if (c.mode == ScanMode::Deep) {
        text += onOff(c.carving, "carving") + ", " + onOff(c.mp4, "MP4 recovery") + ", " +
                onOff(c.fragments, "fragments") + ", alignment " + std::to_string(c.alignment) + ", at most " +
                formatCount(c.maxHits) + " signatures; ";
    }
    text += c.includeActive ? (c.includeDeleted ? "active and deleted files" : "active files")
                            : (c.includeDeleted ? "deleted files" : "no filesystem files");
    text += "; " + onOff(c.media, "media validation") + ", " + onOff(info.playability, "playability") + ", " +
            onOff(c.sha256, "SHA-256") + "; " + std::to_string(c.sectorRetryCount) + " sector retries";
    return text;
}

std::string historyLine(const session::StateChange& change) {
    std::string text = formatTime(change.time) + "  " + std::string(session::toString(change.state)) + " (engine " +
                       printable(change.engineVersion) + ")";
    if (change.error.has_value()) {
        text += ": " + printable(describe(*change.error));
    }
    return text;
}

std::string lackingText(const ReconstructionReport& report) {
    std::string text;
    const auto add = [&](std::uint64_t bytes, std::string_view what) {
        if (bytes != 0) {
            text += text.empty() ? "" : ", ";
            text += formatBytes(bytes) + " " + std::string(what);
        }
    };
    add(report.missingBytes, "missing");
    add(report.unreadableBytes, "unreadable");
    add(report.reallocatedBytes, "from reallocated clusters");
    return text;
}

std::string displayName(const metadata::CandidateMetadata& candidate) {
    return printable(candidate.path.empty() ? candidate.name : candidate.path);
}

// ---------------------------------------------------------------------------
// JSON pieces
// ---------------------------------------------------------------------------

void jsonError(JsonWriter& json, std::string_view name, const std::optional<Error>& error) {
    if (!error.has_value()) {
        json.nullField(name);
        return;
    }
    json.key(name);
    json.beginObject();
    json.field("code", toString(error->code));
    json.field("message", error->message);
    json.field("systemErrorCode", error->systemErrorCode);
    json.endObject();
}

void jsonRegions(JsonWriter& json, std::string_view name, const std::vector<storage::BadRegion>& regions) {
    json.key(name);
    json.beginArray();
    for (const storage::BadRegion& region : regions) {
        json.beginObject();
        json.field("offset", region.offset);
        json.field("length", region.length);
        json.field("errorCode", region.errorCode);
        json.endObject();
    }
    json.endArray();
}

void jsonHistory(JsonWriter& json, const std::vector<session::StateChange>& history) {
    json.key("history");
    json.beginArray();
    for (const session::StateChange& change : history) {
        json.beginObject();
        json.field("state", session::toString(change.state));
        json.field("time", formatIsoTime(change.time));
        json.field("engineVersion", change.engineVersion);
        jsonError(json, "error", change.error);
        json.endObject();
    }
    json.endArray();
}

void jsonState(JsonWriter& json, const std::optional<session::SessionState>& state) {
    if (state.has_value()) {
        json.field("state", session::toString(*state));
    } else {
        json.nullField("state");
    }
}

void jsonScanMetrics(JsonWriter& json, const scan::ScanMetrics& metrics) {
    json.key("metrics");
    json.beginObject();
    json.field("sourceSize", metrics.sourceSize);
    json.field("bytesScanned", metrics.bytesScanned);
    json.field("bytesRead", metrics.bytesRead);
    json.field("scanSpeed", metrics.scanSpeed);
    json.field("filesFound", metrics.filesFound);
    json.field("carves", metrics.carves);
    json.field("mp4Candidates", metrics.mp4Candidates);
    json.field("fragmentCandidates", metrics.fragmentCandidates);
    json.field("candidates", metrics.candidates);
    json.field("validationFailures", metrics.validationFailures);
    json.field("duplicates", metrics.duplicates);
    json.field("unreadableBytes", metrics.unreadableBytes);
    json.field("elapsedMs", static_cast<std::int64_t>(metrics.elapsed.count()));
    json.endObject();
}

void jsonJobMetrics(JsonWriter& json, const scan::RecoveryJobMetrics& metrics) {
    json.key("metrics");
    json.beginObject();
    json.field("files", metrics.files);
    json.field("recoveredFiles", metrics.recoveredFiles);
    json.field("failedFiles", metrics.failedFiles);
    json.field("bytesRecovered", metrics.bytesRecovered);
    json.field("unreadableBytes", metrics.unreadableBytes);
    json.field("bytesRead", metrics.bytesRead);
    json.field("speed", metrics.speed);
    json.field("elapsedMs", static_cast<std::int64_t>(metrics.elapsed.count()));
    json.endObject();
}

void jsonReconstruction(JsonWriter& json, const ReconstructionReport& report) {
    json.key("report");
    json.beginObject();
    json.field("expectedSize", report.expectedSize);
    json.field("outputSize", report.outputSize);
    json.field("storedBytes", report.storedBytes);
    json.field("embeddedBytes", report.embeddedBytes);
    json.field("zeroBytes", report.zeroBytes);
    json.field("missingBytes", report.missingBytes);
    json.field("unreadableBytes", report.unreadableBytes);
    json.field("outsideSourceBytes", report.outsideSourceBytes);
    json.field("reallocatedBytes", report.reallocatedBytes);
    json.field("allBytesRead", report.allBytesRead());
    jsonRegions(json, "unreadableRegions", report.unreadableRegions);
    json.endObject();
}

void jsonSource(JsonWriter& json, const session::SessionSource& source) {
    json.key("source");
    json.beginObject();
    json.field("type", storage::toString(source.type));
    json.field("path", source.path);
    json.field("size", source.size);
    json.field("sectorSize", source.sectorSize);
    json.field("physicalSectorSize", source.physicalSectorSize);
    json.field("diskNumber", source.diskNumber);
    json.field("vendor", source.vendor);
    json.field("product", source.product);
    json.field("removable", source.removable);
    json.key("fingerprint");
    json.beginObject();
    json.field("sha256", source.fingerprint.digest.hex());
    json.field("bytes", source.fingerprint.bytes);
    json.field("unreadableBytes", source.fingerprint.unreadableBytes);
    json.endObject();
    json.endObject();
}

void jsonConfiguration(JsonWriter& json, const session::SessionInfo& info) {
    const scan::ScanConfiguration& c = info.configuration;
    json.key("configuration");
    json.beginObject();
    json.field("mode", toString(c.mode));
    json.field("carving", c.carving);
    json.field("mp4", c.mp4);
    json.field("fragments", c.fragments);
    json.field("includeActive", c.includeActive);
    json.field("includeDeleted", c.includeDeleted);
    json.field("alignment", c.alignment);
    json.field("maxHits", c.maxHits);
    json.field("sectorRetryCount", c.sectorRetryCount);
    jsonRegions(json, "knownBadRegions", c.knownBadRegions);
    json.field("media", c.media);
    json.field("playability", info.playability);
    json.field("sha256", c.sha256);
    json.field("preliminaryHash", c.preliminaryHash);
    json.endObject();
}

void jsonTimestamp(JsonWriter& json, std::string_view name, const std::optional<filesystem::Timestamp>& time) {
    if (!time.has_value()) {
        json.nullField(name);
        return;
    }
    json.key(name);
    json.beginObject();
    json.field("time", formatIsoTimestamp(*time));
    json.field("local", time->local);
    json.endObject();
}

void jsonCandidate(JsonWriter& json, const SessionReport& report, std::size_t index) {
    const evaluation::EvaluatedCandidate& candidate = report.candidates[index];
    const metadata::CandidateMetadata& d = report.described[index];
    json.beginObject();
    json.field("id", d.id.value());
    json.field("name", d.name);
    json.field("path", d.path);
    json.field("extension", d.extension);
    json.field("deleted", d.deleted);
    json.field("method", toString(d.method));
    json.field("kind", metadata::toString(d.kind));
    json.field("format", d.formatId);
    json.field("mediaType", d.mediaType);
    json.field("size", d.size);
    json.field("expectedSize", d.expectedSize);
    json.field("sourceOffset", d.sourceOffset);
    json.field("fragments", d.fragments);
    jsonTimestamp(json, "created", d.created);
    jsonTimestamp(json, "modified", d.modified);
    json.key("validation");
    json.beginObject();
    json.field("status", carving::toString(d.validation));
    json.field("structural", validation::toString(d.structural));
    json.field("media", validation::toString(d.media));
    json.field("playability", validation::toString(d.playability));
    if (d.deepestPassed.has_value()) {
        json.field("deepestPassed", validation::toString(*d.deepestPassed));
    } else {
        json.nullField("deepestPassed");
    }
    json.endObject();
    json.field("condition", metadata::toString(d.condition));
    json.key("reasons");
    json.beginArray();
    for (const metadata::ConditionReason reason : d.reasons) {
        json.string(metadata::toString(reason));
    }
    json.endArray();
    if (d.sha256.has_value()) {
        json.field("sha256", d.sha256->hex());
    } else {
        json.nullField("sha256");
    }
    json.field("duplicateOf", d.duplicateOf.has_value() ? std::optional(d.duplicateOf->value()) : std::nullopt);
    json.field("container", d.container.has_value() ? std::optional(d.container->value()) : std::nullopt);
    json.field("unreadableBytes", candidate.unreadableBytes);
    json.key("warnings");
    json.beginArray();
    for (const evaluation::EvaluationWarning warning : candidate.warnings) {
        json.string(evaluation::toString(warning));
    }
    json.endArray();
    const metadata::CandidateRecovery recovery = report.recovery.recoveryOf(d.id);
    json.key("recovery");
    json.beginObject();
    json.field("state", metadata::toString(recovery.state));
    json.field("complete", recovery.complete);
    json.key("jobs");
    json.beginArray();
    for (const metadata::JobRecovery& job : recovery.jobs) {
        json.beginObject();
        json.field("job", job.job);
        json.field("state", metadata::toString(job.state));
        if (job.state == metadata::RecoveryState::Recovered) {
            json.field("path", toUtf8(job.path));
        } else {
            json.nullField("path");
        }
        json.field("complete", job.complete);
        jsonError(json, "error", job.error);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    json.key("evidence");
    json.beginArray();
    for (const std::string& line : evaluation::explain(candidate)) {
        json.string(line);
    }
    json.endArray();
    json.endObject();
}

void jsonSummaryFields(JsonWriter& json, const session::SessionSummary& summary) {
    json.field("id", summary.id);
    json.field("folder", toUtf8(summary.folder));
    jsonError(json, "error", summary.error);
    json.field("journalFormat", summary.formatVersion);
    json.field("engineVersion", summary.engineVersion);
    json.field("created", formatIsoTime(summary.created));
    json.field("updated", formatIsoTime(summary.updated));
    json.field("sourceType", storage::toString(summary.sourceType));
    json.field("sourcePath", summary.sourcePath);
    json.field("sourceSize", summary.sourceSize);
    json.field("mode", toString(summary.mode));
    jsonState(json, summary.state);
    json.field("stage", scan::toString(summary.stage));
    jsonScanMetrics(json, summary.metrics);
    json.field("jobs", summary.jobs);
}

void jsonHeader(JsonWriter& json, std::string_view format) {
    json.field("format", format);
    json.field("formatVersion", kReportFormatVersion);
    json.field("engine", std::string(kEngineName) + " " + std::string(kEngineVersion));
    json.field("generated", formatIsoTime(session::sessionNow()));
}

void textSummaryLines(std::ostringstream& out, const session::SessionSummary& summary) {
    out << "Session " << printable(summary.id) << '\n';
    out << "  Folder:        " << displayPath(summary.folder) << '\n';
    if (summary.error.has_value()) {
        out << "  Unreadable:    " << printable(describe(*summary.error)) << '\n';
        return;
    }
    out << "  Created:       " << formatTime(summary.created) << " by engine " << printable(summary.engineVersion)
        << '\n';
    out << "  Updated:       " << formatTime(summary.updated) << '\n';
    out << "  Source:        "
        << describeSource(summary.sourceType, summary.sourcePath, std::nullopt, std::string_view{},
                          std::string_view{})
        << ", " << formatSize(summary.sourceSize) << '\n';
    out << "  Scan:          " << toString(summary.mode) << ", " << stateText(summary.state, false);
    if (summary.state.has_value() && *summary.state != session::SessionState::Completed) {
        out << " at stage " << stageName(summary.stage);
    }
    out << "; " << formatCount(summary.metrics.candidates) << " candidates\n";
    out << "  Recovery jobs: " << summary.jobs << '\n';
}

}  // namespace

SessionReport gatherReport(const session::RecoverySession& session) {
    SessionReport report;
    report.info = session.info();
    report.candidates = session.candidates();
    report.described.reserve(report.candidates.size());
    for (const evaluation::EvaluatedCandidate& candidate : report.candidates) {
        report.described.push_back(metadata::describeCandidate(candidate));
    }
    report.duplicates = metadata::DuplicateGroups::build(report.candidates);
    report.recovery = metadata::RecoveryJobIndex::fromSession(session);
    for (const session::RecoveryJobStatus& job : report.info.jobs) {
        report.recovered[job.id] = session.recoveredItems(job.id);
    }
    report.errors = session.errors();
    report.unreadable = session.unreadableRegions();
    return report;
}

std::string textReport(const SessionReport& report, bool details) {
    const session::SessionInfo& info = report.info;
    std::ostringstream out;
    out << "Session " << printable(info.id) << '\n';
    out << "  Folder:        " << displayPath(info.folder) << '\n';
    out << "  Created:       " << formatTime(info.created) << " by engine " << printable(info.engineVersion) << '\n';
    out << "  Updated:       " << formatTime(info.updated) << '\n';
    out << "  Journal:       format " << info.formatVersion;
    if (!info.damage.empty()) {
        out << "; damage repaired " << info.damage.size() << (info.damage.size() == 1 ? " time" : " times");
    }
    out << '\n';

    out << "\nSource\n";
    out << "  Source:        " << sourceText(info.source) << '\n';
    out << "  Size:          " << formatBytes(info.source.size) << '\n';
    out << "  Sector size:   " << info.source.sectorSize << " bytes\n";
    out << "  Fingerprint:   SHA-256 " << info.source.fingerprint.digest.hex() << " ("
        << formatSize(info.source.fingerprint.bytes) << " hashed";
    if (info.source.fingerprint.unreadableBytes != 0) {
        out << ", " << formatSize(info.source.fingerprint.unreadableBytes) << " unreadable";
    }
    out << ")\n";

    const scan::ScanMetrics& metrics = info.scan.metrics;
    out << "\nScan\n";
    out << "  Mode:          " << toString(info.configuration.mode) << '\n';
    out << "  Settings:      " << settingsText(info) << '\n';
    if (!info.configuration.knownBadRegions.empty()) {
        out << "  Known bad:     " << formatCount(info.configuration.knownBadRegions.size())
            << " regions listed by the image's metadata, read as unreadable\n";
    }
    out << "  State:         " << stateText(info.scan.state, info.scan.interrupted);
    if (info.scan.state.has_value() && *info.scan.state != session::SessionState::Completed) {
        out << " at stage " << stageName(info.scan.stage);
    }
    out << '\n';
    if (!info.scan.runnable && info.scan.state != session::SessionState::Completed) {
        out << "  Cannot resume: " << printable(info.scan.notRunnable) << '\n';
    }
    for (std::size_t i = 0; i < info.scan.history.size(); ++i) {
        out << (i == 0 ? "  History:       " : "                 ") << historyLine(info.scan.history[i]) << '\n';
    }
    out << "  Updates:       " << formatCount(info.scan.updates) << '\n';
    out << "  Read:          " << formatSize(metrics.bytesScanned) << " scanned, " << formatSize(metrics.bytesRead)
        << " read, " << formatSize(metrics.unreadableBytes) << " unreadable, in " << formatDuration(metrics.elapsed)
        << '\n';
    out << "  Found:         " << formatCount(metrics.filesFound) << " files in filesystem metadata, "
        << formatCount(metrics.carves) << " carves, " << formatCount(metrics.mp4Candidates) << " MP4 candidates, "
        << formatCount(metrics.fragmentCandidates) << " reconstructions\n";
    out << "  Candidates:    " << formatCount(report.candidates.size()) << " (" << formatCount(metrics.duplicates)
        << " duplicates, " << formatCount(metrics.validationFailures) << " failed validation)\n";

    if (info.partitionScheme.has_value() || !info.volumes.empty()) {
        out << "\nVolumes";
        if (info.partitionScheme.has_value()) {
            out << " (partition table: " << partition::toString(*info.partitionScheme) << ")";
        }
        out << '\n';
        Table volumes({{"#", true},
                       {"Offset", true},
                       {"Size", true},
                       {"Partition", true},
                       {"Filesystem"},
                       {"Label"},
                       {"Serial"},
                       {"Cluster", true},
                       {"Files", true},
                       {"Note"}});
        for (std::size_t i = 0; i < info.volumes.size(); ++i) {
            const session::VolumeSummary& v = info.volumes[i];
            volumes.addRow({std::to_string(i + 1), formatCount(v.offset), formatSize(v.size),
                            v.partition.has_value() ? std::to_string(*v.partition) : "-",
                            v.filesystem.has_value() ? std::string(filesystem::toString(*v.filesystem)) : "-",
                            printable(v.label),
                            v.filesystem.has_value() ? serialText(v.filesystem, v.serialNumber) : "",
                            v.clusterSize != 0 ? formatCount(v.clusterSize) : "", formatCount(v.files),
                            v.error.has_value() ? printable(describe(*v.error))
                                                : (v.scanned ? std::string() : std::string("not scanned yet"))});
        }
        volumes.print(out);
    }

    out << "\nCandidates (" << formatCount(report.candidates.size()) << ")\n";
    if (!report.candidates.empty()) {
        Table rows({{"ID", true},
                    {"Condition"},
                    {"Kind"},
                    {"Format"},
                    {"Size", true},
                    {"Method"},
                    {"Deleted"},
                    {"Validation"},
                    {"Recovery"},
                    {"Name"}});
        for (const metadata::CandidateMetadata& d : report.described) {
            rows.addRow({std::to_string(d.id.value()), std::string(metadata::toString(d.condition)),
                         std::string(metadata::toString(d.kind)), d.formatId.empty() ? "-" : printable(d.formatId),
                         formatSize(d.size), std::string(toString(d.method)), d.deleted ? "yes" : "no",
                         std::string(carving::toString(d.validation)),
                         std::string(metadata::toString(report.recovery.stateOf(d.id))), displayName(d)});
        }
        rows.print(out);
    }
    if (details) {
        for (std::size_t i = 0; i < report.candidates.size(); ++i) {
            const metadata::CandidateMetadata& d = report.described[i];
            out << "\nCandidate " << d.id.value() << ": " << displayName(d) << '\n';
            out << "  Condition:     " << metadata::toString(d.condition);
            for (std::size_t r = 0; r < d.reasons.size(); ++r) {
                out << (r == 0 ? " (" : ", ") << metadata::toString(d.reasons[r]);
            }
            out << (d.reasons.empty() ? "" : ")") << '\n';
            out << "  Validation:    structural " << validation::toString(d.structural) << ", media "
                << validation::toString(d.media) << ", playability " << validation::toString(d.playability) << '\n';
            if (d.sha256.has_value()) {
                out << "  SHA-256:       " << d.sha256->hex() << '\n';
            }
            if (d.duplicateOf.has_value()) {
                out << "  Duplicate of:  " << d.duplicateOf->value() << '\n';
            }
            if (d.created.has_value()) {
                out << "  Created:       " << formatTimestamp(*d.created) << '\n';
            }
            if (d.modified.has_value()) {
                out << "  Modified:      " << formatTimestamp(*d.modified) << '\n';
            }
            for (const std::string& line : evaluation::explain(report.candidates[i])) {
                out << "  " << printable(line) << '\n';
            }
        }
    }

    if (!report.duplicates.groups().empty()) {
        out << "\nDuplicates (" << formatCount(report.duplicates.duplicateCount()) << ")\n";
        for (const metadata::DuplicateGroup& group : report.duplicates.groups()) {
            out << "  " << formatSize(group.size) << ", SHA-256 "
                << (group.sha256.has_value() ? group.sha256->hex().substr(0, 16) + "..." : std::string("-")) << ":";
            for (std::size_t m = 0; m < group.members.size(); ++m) {
                const std::uint64_t id = group.members[m].value();
                out << (m == 0 ? " " : ", ") << id;
                if (id >= 1 && id <= report.described.size()) {
                    out << " " << displayName(report.described[id - 1]);
                }
                out << (m == 0 ? " (original)" : "");
            }
            out << '\n';
        }
    }

    out << "\nRecovery jobs (" << info.jobs.size() << ")\n";
    for (const session::RecoveryJobStatus& job : info.jobs) {
        out << "  Job " << job.id << " to " << printable(job.destination) << '\n';
        out << "    Created:     " << formatTime(job.created) << '\n';
        out << "    State:       " << stateText(job.state, job.interrupted) << "; " << formatCount(job.done) << " of "
            << formatCount(job.candidates.size()) << " files done: " << formatCount(job.recovered) << " recovered, "
            << formatCount(job.failed) << " failed; " << formatSize(job.metrics.bytesRecovered) << " written in "
            << formatDuration(job.metrics.elapsed) << '\n';
        if (job.filesInProgress != 0) {
            out << "    In progress: " << formatCount(job.filesInProgress)
                << " files begun and not finished (written again when the job resumes)\n";
        }
        for (std::size_t i = 0; i < job.history.size(); ++i) {
            out << (i == 0 ? "    History:     " : "                 ") << historyLine(job.history[i]) << '\n';
        }
        const auto found = report.recovered.find(job.id);
        if (found == report.recovered.end() || found->second.empty()) {
            continue;
        }
        Table files({{"ID", true}, {"Result"}, {"File or error"}});
        for (const scan::RecoveredItem& item : found->second) {
            if (item.file.has_value()) {
                const ReconstructionReport& r = item.file->report;
                std::string text = displayPath(item.file->path);
                if (const std::string lack = lackingText(r); !lack.empty()) {
                    text += " (" + lack + ")";
                }
                files.addRow({std::to_string(item.candidate.value()), r.allBytesRead() ? "complete" : "incomplete",
                              text});
            } else {
                files.addRow({std::to_string(item.candidate.value()), "failed",
                              item.error.has_value() ? printable(describe(*item.error)) : "unknown error"});
            }
        }
        files.print(out, "    ");
    }

    if (!report.errors.empty()) {
        out << "\nErrors (" << report.errors.size() << ")\n";
        for (const session::SessionError& error : report.errors) {
            out << "  " << formatTime(error.time) << "  " << printable(error.context) << ": "
                << printable(describe(error.error)) << '\n';
        }
    }
    if (!report.unreadable.empty()) {
        std::uint64_t bytes = 0;
        Table regions({{"Offset", true}, {"Length", true}, {"Error", true}});
        for (const storage::BadRegion& region : report.unreadable) {
            bytes += region.length;
            regions.addRow({formatCount(region.offset), formatCount(region.length), std::to_string(region.errorCode)});
        }
        out << "\nUnreadable source regions (" << report.unreadable.size() << ", " << formatBytes(bytes) << ")\n";
        regions.print(out);
    }
    if (!info.damage.empty()) {
        out << "\nJournal damage\n";
        for (const session::SessionDamage& damage : info.damage) {
            out << "  " << formatTime(damage.time) << ": " << formatBytes(damage.damage.bytesDropped)
                << " dropped at offset " << formatCount(damage.damage.offset) << " ("
                << formatCount(damage.damage.recordsDropped) << " intact records after it); "
                << printable(damage.damage.reason) << "; copy kept as " << printable(damage.damage.backup) << '\n';
        }
    }
    return out.str();
}

std::string jsonReport(const SessionReport& report) {
    const session::SessionInfo& info = report.info;
    JsonWriter json;
    json.beginObject();
    jsonHeader(json, kReportFormat);

    json.key("session");
    json.beginObject();
    json.field("id", info.id);
    json.field("folder", toUtf8(info.folder));
    json.field("journalFormat", info.formatVersion);
    json.field("engineVersion", info.engineVersion);
    json.field("created", formatIsoTime(info.created));
    json.field("updated", formatIsoTime(info.updated));
    json.field("tornBytesDropped", info.tornBytesDropped);
    json.field("recordsSkipped", info.recordsSkipped);
    json.key("damage");
    json.beginArray();
    for (const session::SessionDamage& damage : info.damage) {
        json.beginObject();
        json.field("time", formatIsoTime(damage.time));
        json.field("offset", damage.damage.offset);
        json.field("bytesDropped", damage.damage.bytesDropped);
        json.field("recordsDropped", damage.damage.recordsDropped);
        json.field("reason", damage.damage.reason);
        json.field("backup", damage.damage.backup);
        json.endObject();
    }
    json.endArray();
    json.endObject();

    jsonSource(json, info.source);

    json.key("scan");
    json.beginObject();
    jsonConfiguration(json, info);
    jsonState(json, info.scan.state);
    json.field("interrupted", info.scan.interrupted);
    json.field("runnable", info.scan.runnable);
    json.field("notRunnable", info.scan.notRunnable);
    json.field("stage", scan::toString(info.scan.stage));
    json.field("updates", info.scan.updates);
    json.field("candidates", info.scan.candidates);
    jsonHistory(json, info.scan.history);
    jsonScanMetrics(json, info.scan.metrics);
    json.endObject();

    if (info.partitionScheme.has_value()) {
        json.field("partitionScheme", partition::toString(*info.partitionScheme));
    } else {
        json.nullField("partitionScheme");
    }
    json.key("volumes");
    json.beginArray();
    for (const session::VolumeSummary& v : info.volumes) {
        json.beginObject();
        json.field("offset", v.offset);
        json.field("size", v.size);
        json.field("partition", v.partition);
        json.field("scanned", v.scanned);
        if (v.filesystem.has_value()) {
            json.field("filesystem", filesystem::toString(*v.filesystem));
        } else {
            json.nullField("filesystem");
        }
        json.field("label", v.label);
        json.field("serialNumber", v.serialNumber);
        json.field("clusterSize", v.clusterSize);
        json.field("files", v.files);
        jsonError(json, "error", v.error);
        json.endObject();
    }
    json.endArray();

    json.key("candidates");
    json.beginArray();
    for (std::size_t i = 0; i < report.candidates.size(); ++i) {
        jsonCandidate(json, report, i);
    }
    json.endArray();

    json.key("duplicateGroups");
    json.beginArray();
    for (const metadata::DuplicateGroup& group : report.duplicates.groups()) {
        json.beginObject();
        json.field("original", group.original().value());
        json.key("members");
        json.beginArray();
        for (const evaluation::EvaluatedCandidateId member : group.members) {
            json.number(member.value());
        }
        json.endArray();
        if (group.sha256.has_value()) {
            json.field("sha256", group.sha256->hex());
        } else {
            json.nullField("sha256");
        }
        json.field("size", group.size);
        json.endObject();
    }
    json.endArray();

    json.key("jobs");
    json.beginArray();
    for (const session::RecoveryJobStatus& job : info.jobs) {
        json.beginObject();
        json.field("id", job.id);
        json.field("destination", job.destination);
        json.field("created", formatIsoTime(job.created));
        jsonState(json, job.state);
        json.field("interrupted", job.interrupted);
        json.field("candidates", job.candidates.size());
        json.field("done", job.done);
        json.field("recovered", job.recovered);
        json.field("failed", job.failed);
        json.field("filesInProgress", job.filesInProgress);
        jsonHistory(json, job.history);
        jsonJobMetrics(json, job.metrics);
        json.key("files");
        json.beginArray();
        if (const auto found = report.recovered.find(job.id); found != report.recovered.end()) {
            for (const scan::RecoveredItem& item : found->second) {
                json.beginObject();
                json.field("candidate", item.candidate.value());
                if (item.file.has_value()) {
                    json.field("path", toUtf8(item.file->path));
                    jsonReconstruction(json, item.file->report);
                } else {
                    json.nullField("path");
                    json.nullField("report");
                }
                jsonError(json, "error", item.error);
                json.endObject();
            }
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();

    json.key("errors");
    json.beginArray();
    for (const session::SessionError& error : report.errors) {
        json.beginObject();
        json.field("time", formatIsoTime(error.time));
        json.field("context", error.context);
        jsonError(json, "error", error.error);
        json.endObject();
    }
    json.endArray();
    jsonRegions(json, "unreadableRegions", report.unreadable);
    json.endObject();
    return json.finish();
}

std::string textSummary(const session::SessionSummary& summary) {
    std::ostringstream out;
    textSummaryLines(out, summary);
    return out.str();
}

std::string jsonSummary(const session::SessionSummary& summary) {
    JsonWriter json;
    json.beginObject();
    jsonHeader(json, kReportFormat);
    json.field("inUse", true);
    json.key("summary");
    json.beginObject();
    jsonSummaryFields(json, summary);
    json.endObject();
    json.endObject();
    return json.finish();
}

std::string textSessionList(const std::filesystem::path& root, const std::vector<session::SessionSummary>& sessions) {
    std::ostringstream out;
    out << "Sessions in " << displayPath(root) << ": " << sessions.size() << '\n';
    if (sessions.empty()) {
        return out.str();
    }
    Table rows({{"ID"}, {"Created"}, {"Mode"}, {"Scan"}, {"Candidates", true}, {"Jobs", true}, {"Source"}});
    for (const session::SessionSummary& s : sessions) {
        if (s.error.has_value()) {
            rows.addRow({printable(s.id), "", "", "unreadable", "", "", printable(describe(*s.error))});
            continue;
        }
        std::string state = stateText(s.state, false);
        if (s.state.has_value() && *s.state != session::SessionState::Completed) {
            state += " (" + stageName(s.stage) + ")";
        }
        rows.addRow({printable(s.id), formatTime(s.created), std::string(toString(s.mode)), state,
                     formatCount(s.metrics.candidates), std::to_string(s.jobs),
                     describeSource(s.sourceType, s.sourcePath, std::nullopt, {}, {})});
    }
    rows.print(out);
    return out.str();
}

std::string jsonSessionList(const std::filesystem::path& root, const std::vector<session::SessionSummary>& sessions) {
    JsonWriter json;
    json.beginObject();
    jsonHeader(json, kListFormat);
    json.field("folder", toUtf8(root));
    json.key("sessions");
    json.beginArray();
    for (const session::SessionSummary& summary : sessions) {
        json.beginObject();
        jsonSummaryFields(json, summary);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    return json.finish();
}

}  // namespace recovery::report
