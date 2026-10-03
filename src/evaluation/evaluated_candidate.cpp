#include "evaluation/evaluated_candidate.hpp"

#include <algorithm>
#include <array>

namespace recovery::evaluation {

namespace {

std::string plural(std::uint64_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

std::string hex(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    do {
        text.insert(text.begin(), kDigits[value & 0xFU]);
        value >>= 4;
    } while (value != 0);
    return "0x" + text;
}

std::string hex32(std::uint32_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(8, '0');
    for (int i = 7; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kDigits[value & 0xFU];
        value >>= 4;
    }
    return text;
}

std::string_view toString(formats::mp4::FileStatus status) noexcept {
    switch (status) {
    case formats::mp4::FileStatus::Valid:
        return "valid";
    case formats::mp4::FileStatus::Truncated:
        return "truncated";
    case formats::mp4::FileStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}

std::string_view toString(formats::mp4::MediaKind kind) noexcept {
    switch (kind) {
    case formats::mp4::MediaKind::Audio:
        return "audio";
    case formats::mp4::MediaKind::Video:
        return "video";
    case formats::mp4::MediaKind::Neither:
        return "neither audio nor video";
    }
    return "unknown";
}

template <typename Warning>
std::string listOf(const std::vector<Warning>& warnings) {
    std::string text;
    for (const Warning warning : warnings) {
        text += (text.empty() ? "" : ", ") + std::string(toString(warning));
    }
    return text;
}

std::string levelLine(std::string_view name, const validation::LevelResult& level) {
    std::string line = std::string(name) + ": " + std::string(validation::toString(level.status));
    if (!level.checker.empty()) {
        line += " (" + level.checker + ")";
    }
    if (level.status == validation::LevelStatus::Passed && level.coverage == validation::Coverage::Partial) {
        line += ", partial coverage";
    }
    if (level.offset.has_value() &&
        (level.status == validation::LevelStatus::Failed || level.status == validation::LevelStatus::Truncated)) {
        line += " at file offset " + std::to_string(*level.offset);
    }
    if (!level.detail.empty()) {
        line += ": " + level.detail;
    }
    return line;
}

}  // namespace

std::string_view toString(EvaluationWarning warning) noexcept {
    switch (warning) {
    case EvaluationWarning::StructureInvalid:
        return "STRUCTURE_INVALID";
    case EvaluationWarning::ContentTruncated:
        return "CONTENT_TRUNCATED";
    case EvaluationWarning::MediaInvalid:
        return "MEDIA_INVALID";
    case EvaluationWarning::PlaybackFailed:
        return "PLAYBACK_FAILED";
    case EvaluationWarning::FormatUnknown:
        return "FORMAT_UNKNOWN";
    case EvaluationWarning::DataUnreadable:
        return "DATA_UNREADABLE";
    case EvaluationWarning::DuplicateContent:
        return "DUPLICATE_CONTENT";
    case EvaluationWarning::AlternativeLayout:
        return "ALTERNATIVE_LAYOUT";
    case EvaluationWarning::ReconstructionFailed:
        return "RECONSTRUCTION_FAILED";
    case EvaluationWarning::InsideActiveFile:
        return "INSIDE_ACTIVE_FILE";
    }
    return "UNKNOWN";
}

bool EvaluatedCandidate::hasWarning(EvaluationWarning warning) const noexcept {
    return std::find(warnings.begin(), warnings.end(), warning) != warnings.end();
}

std::vector<std::string> explain(const EvaluatedCandidate& candidate) {
    std::vector<std::string> lines;
    const RecoveryCandidate& data = candidate.data;

    // What the file is.
    std::string summary = "Candidate " + std::to_string(candidate.id.value()) + ": " + data.filename + " (" +
                          (candidate.formatId.empty() ? std::string("no known format") : candidate.formatId) +
                          "), " + std::string(toString(data.method)) + ": " +
                          plural(candidate.recoveredSize(), "byte", "bytes") + " recovered of " +
                          std::to_string(data.expectedSize) + " expected";
    if (const std::optional<std::uint64_t> offset = candidate.sourceOffset()) {
        summary += ", from source offset " + std::to_string(*offset) + " (" + hex(*offset) + ")";
    }
    summary += ", " + plural(candidate.fragmentationCount(), "fragment", "fragments");
    if (!data.fragmentation.known) {
        summary += " (the layout is inferred, not recorded)";
    }
    lines.push_back(summary);
    const std::uint64_t missing = data.bytes(RegionKind::Missing);
    const std::uint64_t zeros = data.bytes(RegionKind::Zeros);
    if (missing > 0 || zeros > 0 || data.reallocatedBytes() > 0) {
        lines.push_back("Layout: " + plural(data.bytes(RegionKind::Stored), "byte", "bytes") + " stored, " +
                        std::to_string(data.bytes(RegionKind::Embedded)) + " embedded, " + std::to_string(zeros) +
                        " known zeros, " + std::to_string(missing) + " missing, " +
                        std::to_string(data.reallocatedBytes()) + " in clusters allocated again since");
    }

    // Filesystem evidence.
    if (candidate.hasFilesystemEvidence()) {
        const FilesystemEvidence& fs = data.filesystemEvidence;
        std::string line = "Filesystem: " + std::string(filesystem::toString(fs.type)) + " volume at offset " +
                           std::to_string(fs.volumeOffset) + ", " +
                           (fs.state == filesystem::EntryState::Deleted ? "deleted" : "active") + " entry " +
                           fs.path + " (metadata at " + std::to_string(fs.metadataOffset) + ")";
        if (fs.parentDeleted) {
            line += " in a deleted directory";
        }
        line += "; layout " + std::string(toString(fs.allocation.layout));
        if (!fs.allocation.issues.empty()) {
            line += "; allocation issues: " + listOf(fs.allocation.issues);
        }
        if (!fs.entryIssues.empty()) {
            line += "; entry issues: " + listOf(fs.entryIssues);
        }
        lines.push_back(line);
    }
    if (!data.warnings.empty()) {
        lines.push_back("Layout warnings: " + listOf(data.warnings));
    }

    // Carving evidence.
    if (candidate.carve.has_value()) {
        const carving::FileCandidate& carve = *candidate.carve;
        std::string line = "Carving: " + carve.formatId + " signature '" + carve.signature.signatureName +
                           "' at source offset " + std::to_string(carve.signature.matchOffset) + ", " +
                           plural(carve.length, "byte", "bytes") + "; end " +
                           std::string(carving::toString(carve.end.method)) + " " +
                           std::string(carving::toString(carve.end.status));
        if (!carve.end.detail.empty()) {
            line += " (" + carve.end.detail + ")";
        }
        line += "; the carve's structure " + std::string(carving::toString(carve.validation.status));
        if (!carve.warnings.empty()) {
            line += "; " + listOf(carve.warnings);
        }
        lines.push_back(line);
        // A filesystem candidate's own layout, decided with the carve (an MP4
        // candidate's or a reconstruction's carve is their evidence).
        const bool reconstructed = candidate.fragments.has_value() && candidate.fragments->source.has_value();
        if (candidate.hasFilesystemEvidence() && !candidate.mp4Candidate.has_value() && !reconstructed) {
            lines.push_back(data.method == RecoveryMethod::Hybrid
                                ? "Merged: the carve that starts where the metadata's file starts gives the layout "
                                  "(the metadata's own layout did not validate)"
                                : "Merged: a carve starts where the metadata's file starts; the metadata's layout "
                                  "stands");
        }
    }
    for (const carving::FileCandidate& other : candidate.otherCarves) {
        lines.push_back("Another carve starts here: " + other.formatId + ", " +
                        plural(other.length, "byte", "bytes") + ", structure " +
                        std::string(carving::toString(other.validation.status)));
    }
    if (candidate.allocation.has_value()) {
        const AllocationEvidence& allocation = *candidate.allocation;
        std::string line = "Allocation: " + std::string(filesystem::toString(allocation.filesystem)) +
                           " volume at offset " + std::to_string(allocation.volumeOffset) + ", " +
                           plural(allocation.clusters, "cluster", "clusters") + ": " +
                           std::to_string(allocation.freeClusters) + " free, " +
                           std::to_string(allocation.allocatedClusters) + " allocated now, " +
                           std::to_string(allocation.otherClusters) + " other";
        if (!allocation.complete) {
            line += " (not all checked)";
        }
        if (allocation.insideActiveFile && !allocation.activeFiles.empty()) {
            line += "; inside the active file " + allocation.activeFiles.front();
        } else if (!allocation.activeFiles.empty()) {
            std::string files;
            for (const std::string& path : allocation.activeFiles) {
                files += (files.empty() ? "" : ", ") + path;
            }
            line += "; overlaps active files " + files;
        }
        lines.push_back(line);
    }

    // MP4 evidence.
    if (candidate.mp4.has_value()) {
        const Mp4Structure& mp4 = *candidate.mp4;
        std::string line = "MP4: structure " + std::string(toString(mp4.status));
        if (!mp4.detail.empty()) {
            line += " (" + mp4.detail + ")";
        }
        line += "; " + std::string(toString(mp4.kind));
        if (mp4.moovOffset.has_value()) {
            line += ", moov at " + std::to_string(*mp4.moovOffset);
            if (mp4.moovFoundBySearch) {
                line += " (found by search)";
            }
        }
        line += "; " + plural(mp4.tracks.size(), "track", "tracks") + ", " +
                plural(mp4.samples(), "sample", "samples") + ": " + std::to_string(mp4.samplesIntact()) +
                " intact, " + std::to_string(mp4.samplesDamaged()) + " damaged, " +
                std::to_string(mp4.samplesMisframed()) + " misframed";
        lines.push_back(line);
    }
    if (!candidate.mp4Warnings.empty()) {
        lines.push_back("MP4 warnings: " + listOf(candidate.mp4Warnings));
    }

    // Fragment reconstruction.
    if (candidate.fragments.has_value()) {
        const FragmentEvidence& fragments = *candidate.fragments;
        std::string line = "Fragment reconstruction (" + std::string(toString(fragments.origin)) + " seed): " +
                           std::string(toString(fragments.status));
        if (!fragments.reason.empty()) {
            line += " (" + fragments.reason + ")";
        }
        if (fragments.source.has_value() && fragments.evidence.has_value()) {
            const HypothesisEvidence& evidence = *fragments.evidence;
            line += "; layout from " + std::string(toString(*fragments.source)) + ": " +
                    plural(evidence.fragments, "fragment", "fragments") + ", " +
                    plural(evidence.clusters, "cluster", "clusters") + " (" +
                    std::to_string(evidence.allocatedClusters) + " allocated now, " +
                    std::to_string(evidence.claimedClusters) + " claimed by other files), " +
                    std::to_string(evidence.placedBytes) + " bytes placed, " +
                    std::to_string(evidence.missingBytes) + " missing";
            if (!evidence.dataChecked) {
                line += "; the joins rest on the allocation evidence alone";
            }
        }
        line += "; " + plural(fragments.hypotheses, "layout", "layouts") + " reported";
        if (fragments.tied > 1) {
            line += ", " + std::to_string(fragments.tied) + " tied: this is " + std::to_string(fragments.alternative) +
                    " of " + std::to_string(fragments.tied);
        }
        if (!fragments.search.complete) {
            line += "; the search stopped at a limit (" + fragments.search.limit + ")";
        }
        lines.push_back(line);
    }

    // Validation.
    lines.push_back(levelLine("Structural level", candidate.validation.structural));
    lines.push_back(levelLine("Media level", candidate.validation.media));
    lines.push_back(levelLine("Playability level", candidate.validation.playability));
    lines.push_back("Validation status: " + std::string(carving::toString(candidate.validationStatus())));

    // Identity.
    std::string identity = "Content: " + plural(candidate.identity.size, "byte", "bytes");
    if (candidate.identity.sha256.has_value()) {
        identity += ", SHA-256 " + candidate.identity.sha256->hex();
    }
    if (candidate.identity.preliminary.has_value()) {
        identity += "; preliminary hash: CRC-32 " + hex32(candidate.identity.preliminary->head) + " of the first and " +
                    hex32(candidate.identity.preliminary->tail) + " of the last " +
                    std::to_string(std::min(candidate.identity.size, PreliminaryHash::kWindow)) + " bytes";
    }
    if (candidate.unreadableBytes > 0) {
        identity += "; " + plural(candidate.unreadableBytes, "byte", "bytes") + " unreadable, read as zeros";
    }
    lines.push_back(identity);
    if (candidate.duplicateOf.has_value()) {
        lines.push_back("Duplicate: the same content (SHA-256) as candidate " +
                        std::to_string(candidate.duplicateOf->value()));
    }
    if (candidate.container.has_value()) {
        lines.push_back("Part of candidate " + std::to_string(candidate.container->value()) +
                        ": the carve lies inside that active file's data");
    }
    if (!candidate.warnings.empty()) {
        lines.push_back("Warnings: " + listOf(candidate.warnings));
    }
    return lines;
}

}  // namespace recovery::evaluation
