#pragma once

// Shared by the session tests (P16): the journal's bytes and records, and
// sessions put together from them (what a crash, damage or another engine
// leaves on disk).

#include "session/session_journal.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace recovery::session::test {

using Bytes = std::vector<std::byte>;

[[nodiscard]] std::filesystem::path journalPath(const std::filesystem::path& folder);
[[nodiscard]] Bytes journalBytes(const std::filesystem::path& folder);
// Makes `folder` (and its parents) and writes `bytes` as its journal.
void writeJournal(const std::filesystem::path& folder, std::span<const std::byte> bytes);

// The intact records of a journal, with their payloads, as the reader reads
// them (stopping where it stops).
[[nodiscard]] std::vector<JournalRecord> readRecords(const std::filesystem::path& folder);

// A journal of these records, as given (types, flags, times, payloads).
[[nodiscard]] Bytes buildJournal(const std::vector<JournalRecord>& records,
                                 std::uint32_t version = kJournalFormatVersion);

// The record's payload, decoded (ADD_FAILURE when it does not decode).
[[nodiscard]] RecordPayload decoded(const JournalRecord& record);
// A record holding `payload` (its type, the given time).
[[nodiscard]] JournalRecord recordOf(const RecordPayload& payload, SessionTime time = {});

}  // namespace recovery::session::test
