#pragma once

// Shared by the scan tests (P15): the registries, a used FAT32 card with a
// file for every stage of a scan, a sink that collects a scan's updates into
// a checkpoint, the stages of P7-P14 run one after the other (what a scan
// must deliver), and a comparison of evaluated candidates.

#include "carving/format_registry.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "recovery/result.hpp"
#include "scan/scan_coordinator.hpp"
#include "scan/scan_state.hpp"
#include "storage/storage_source.hpp"
#include "validation/media_validator.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace recovery::scan::test {

using Bytes = std::vector<std::byte>;

[[nodiscard]] const carving::FormatRegistry& allFormats();
[[nodiscard]] const validation::MediaValidatorRegistry& allMedia();

// A FAT32 card (512-byte clusters) that has been used: its free clusters hold
// old data, and it has files for every stage: active images, audio and an
// MP4; a deleted file whose guessed layout holds; a deleted fragmented file;
// a copy of a file under another name; files only carving finds (no entry
// names them). `clusters` sets its size.
[[nodiscard]] Bytes makeCard(std::uint32_t clusters = 4096, std::uint64_t seed = 0xCA7D);

// Collects a scan's updates: applies each to its own checkpoint (as the
// caller of a scan does) and keeps the evaluated candidates. `hook` runs
// before each update is taken: an error it returns is the sink's error (the
// update is not taken: what a crash before saving it would leave).
struct Collector {
    ScanCheckpoint checkpoint;
    std::vector<evaluation::EvaluatedCandidate> candidates;
    std::uint64_t updates = 0;
    std::function<Status(const ScanUpdate&)> hook;

    [[nodiscard]] ScanUpdateSink sink();
};

// The stages of P7, P8, P12, P13 and P14 run one after the other on a source
// that is one FAT32, exFAT or NTFS volume (or none): what a deep scan with
// the default configuration delivers. Quick: the volume's candidates only.
[[nodiscard]] std::vector<evaluation::EvaluatedCandidate> referenceScan(storage::IStorageSource& source,
                                                                        ScanMode mode = ScanMode::Deep);

// One line per candidate with everything a scan must reproduce (ids, names,
// method, layout, validation, identity, duplicates, evidence).
[[nodiscard]] std::string describe(const evaluation::EvaluatedCandidate& candidate);
[[nodiscard]] std::vector<std::string> describe(const std::vector<evaluation::EvaluatedCandidate>& candidates);

// Run options for tests: small checkpoints, no progress interval.
[[nodiscard]] ScanRunOptions testRunOptions(std::uint32_t workers = 4);

}  // namespace recovery::scan::test
