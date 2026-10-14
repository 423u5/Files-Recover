// Fuzz target: FAT32, exFAT and NTFS volumes and filesystem-based recovery.
// The input is a volume; it is opened for recovery (each module tried, the
// backup boot records too), its candidates listed (active and deleted, inside
// deleted directories), and every candidate's data reconstructed.

#include "fuzz_support.hpp"
#include "recovery/candidate_reader.hpp"
#include "recovery/cancellation.hpp"
#include "recovery/filesystem_recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    fuzz::FuzzSource source(fuzz::bytesOf(data, size));
    fuzz::require(source.open().ok(), "opening the source");
    Result<std::unique_ptr<FilesystemRecovery>> opened = openFilesystemRecovery(source, 0);
    if (!opened.ok()) {
        return 0;
    }
    CandidateOptions options;
    options.limits.maxEntries = 4096;
    options.limits.maxDepth = 16;
    const CancellationSource cancel;
    Result<CandidateScan> scan = (*opened)->findCandidates(options, cancel.token());
    if (!scan.ok()) {
        return 0;
    }
    std::size_t reconstructed = 0;
    for (const RecoveryCandidate& candidate : scan->candidates) {
        if (++reconstructed > 64) {
            break;
        }
        std::uint64_t delivered = 0;
        const Result<ReconstructionReport> report =
            reconstructCandidate(source, candidate, [&](std::uint64_t fileOffset, std::span<const std::byte> bytes) {
                // Data arrives in file order, inside the candidate's size.
                fuzz::require(fileOffset >= delivered, "data delivered out of order");
                delivered = fileOffset + bytes.size();
                return success();
            });
        if (report.ok()) {
            fuzz::require(delivered <= report->outputSize, "more data than the reported size");
        }
    }
    return 0;
}
