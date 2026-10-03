#pragma once

#include "recovery/candidate_reader.hpp"
#include "recovery/filesystem_recovery.hpp"
#include "recovery/recovery_candidate.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

// A candidate reconstructed into memory, as a recovered file would be
// written: `data` has report.outputSize bytes, zeros where nothing was delivered.
struct Reconstructed {
    std::vector<std::byte> data;
    ReconstructionReport report;
    bool ok = false;
};

// Records a test failure when the reconstruction fails.
[[nodiscard]] Reconstructed reconstructToMemory(storage::IStorageSource& source, const RecoveryCandidate& candidate,
                                                const ReconstructionOptions& options = {});

// Candidates whose original path is `path` (several for duplicate names).
[[nodiscard]] std::vector<const RecoveryCandidate*> candidatesAt(const CandidateScan& scan, std::string_view path);
// The single candidate at `path`, or nullptr (a test failure when there are several).
[[nodiscard]] const RecoveryCandidate* candidateAt(const CandidateScan& scan, std::string_view path);

// One line per region and warning, for failure messages.
[[nodiscard]] std::string describeCandidate(const RecoveryCandidate& candidate);

// Checks the invariants every candidate must satisfy, whatever the metadata
// said: validateCandidate, stored regions inside [volumeOffset, volumeEnd),
// reallocation only on deleted entries.
void expectWellFormed(const RecoveryCandidate& candidate, std::uint64_t volumeOffset, std::uint64_t volumeEnd);

}  // namespace recovery::test
