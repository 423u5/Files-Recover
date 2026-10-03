#include "support/candidate_helpers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace recovery::test {

namespace {

// Test images are small; anything larger is a bug in the test or the engine.
constexpr std::uint64_t kMaxTestFileSize = 64ULL * 1024 * 1024;

}  // namespace

Reconstructed reconstructToMemory(storage::IStorageSource& source, const RecoveryCandidate& candidate,
                                  const ReconstructionOptions& options) {
    Reconstructed out;
    const CandidateSink sink = [&out](std::uint64_t offset, std::span<const std::byte> data) -> Status {
        if (offset > kMaxTestFileSize || data.size() > kMaxTestFileSize - offset) {
            return makeError(ErrorCode::InternalError, "reconstructed test file too large");
        }
        const auto end = static_cast<std::size_t>(offset + data.size());
        if (out.data.size() < end) {
            out.data.resize(end);
        }
        std::memcpy(out.data.data() + offset, data.data(), data.size());
        return success();
    };
    Result<ReconstructionReport> report = reconstructCandidate(source, candidate, sink, options);
    if (!report.ok()) {
        ADD_FAILURE() << "reconstruction of " << candidate.filesystemEvidence.path << " failed: "
                      << describe(report.error());
        return out;
    }
    out.report = std::move(report).value();
    if (out.report.outputSize > kMaxTestFileSize) {
        ADD_FAILURE() << "reconstructed file too large: " << out.report.outputSize;
        return out;
    }
    out.data.resize(static_cast<std::size_t>(out.report.outputSize));
    out.ok = true;
    return out;
}

std::vector<const RecoveryCandidate*> candidatesAt(const CandidateScan& scan, std::string_view path) {
    std::vector<const RecoveryCandidate*> found;
    for (const RecoveryCandidate& candidate : scan.candidates) {
        if (candidate.filesystemEvidence.path == path) {
            found.push_back(&candidate);
        }
    }
    return found;
}

const RecoveryCandidate* candidateAt(const CandidateScan& scan, std::string_view path) {
    const std::vector<const RecoveryCandidate*> found = candidatesAt(scan, path);
    if (found.size() > 1) {
        ADD_FAILURE() << found.size() << " candidates at " << path;
    }
    return found.empty() ? nullptr : found.front();
}

std::string describeCandidate(const RecoveryCandidate& candidate) {
    std::string text = "#" + std::to_string(candidate.id.value()) + " " + candidate.filesystemEvidence.path + " (" +
                       std::to_string(candidate.expectedSize) + " bytes, " +
                       std::string(filesystem::toString(candidate.filesystemEvidence.allocation.method)) + ", layout " +
                       std::string(toString(candidate.filesystemEvidence.allocation.layout)) + ")\n";
    for (const SourceRegion& region : candidate.sourceRegions) {
        text += "  [" + std::to_string(region.fileOffset) + ", +" + std::to_string(region.length) + ") " +
                std::string(toString(region.kind));
        if (region.kind == RegionKind::Stored || region.kind == RegionKind::Embedded) {
            text += " @" + std::to_string(region.sourceOffset);
        }
        if (region.reallocated) {
            text += " reallocated";
        }
        text += "\n";
    }
    for (const CandidateWarning warning : candidate.warnings) {
        text += "  warning " + std::string(toString(warning)) + "\n";
    }
    for (const filesystem::AllocationIssue issue : candidate.filesystemEvidence.allocation.issues) {
        text += "  allocation " + std::string(filesystem::toString(issue)) + "\n";
    }
    for (const filesystem::EntryIssue issue : candidate.filesystemEvidence.entryIssues) {
        text += "  entry " + std::string(filesystem::toString(issue)) + "\n";
    }
    return text;
}

void expectWellFormed(const RecoveryCandidate& candidate, std::uint64_t volumeOffset, std::uint64_t volumeEnd) {
    const Status valid = validateCandidate(candidate);
    EXPECT_TRUE(valid.ok()) << describe(valid.error()) << "\n" << describeCandidate(candidate);
    EXPECT_EQ(candidate.method, RecoveryMethod::Filesystem);
    for (const SourceRegion& region : candidate.sourceRegions) {
        if (region.kind == RegionKind::Stored) {
            EXPECT_GE(region.sourceOffset, volumeOffset) << describeCandidate(candidate);
            EXPECT_LE(region.sourceOffset, volumeEnd) << describeCandidate(candidate);
            EXPECT_LE(region.length, volumeEnd - std::min(region.sourceOffset, volumeEnd))
                << describeCandidate(candidate);
        }
        if (region.reallocated) {
            EXPECT_TRUE(candidate.isDeleted()) << describeCandidate(candidate);
        }
    }
    EXPECT_EQ(candidate.hasWarning(CandidateWarning::DataMissing), candidate.bytes(RegionKind::Missing) > 0);
    EXPECT_EQ(candidate.hasWarning(CandidateWarning::ClustersReallocated), candidate.reallocatedBytes() > 0);
    EXPECT_EQ(candidate.hasWarning(CandidateWarning::LayoutGuessed),
              candidate.filesystemEvidence.allocation.layout == LayoutEvidence::Guessed);
}

}  // namespace recovery::test
