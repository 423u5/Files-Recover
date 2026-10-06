#include "session_test_support.hpp"

#include "session/recovery_session.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <system_error>

namespace recovery::session::test {

std::filesystem::path journalPath(const std::filesystem::path& folder) {
    return folder / RecoverySession::kJournalName;
}

Bytes journalBytes(const std::filesystem::path& folder) {
    return ::recovery::test::readFile(journalPath(folder));
}

void writeJournal(const std::filesystem::path& folder, std::span<const std::byte> bytes) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    EXPECT_FALSE(ec) << ec.message();
    ::recovery::test::writeFile(journalPath(folder), Bytes(bytes.begin(), bytes.end()));
}

std::vector<JournalRecord> readRecords(const std::filesystem::path& folder) {
    std::vector<JournalRecord> records;
    Result<JournalReader> reader = JournalReader::open(journalPath(folder));
    if (!reader.ok()) {
        ADD_FAILURE() << describe(reader.error());
        return records;
    }
    for (;;) {
        Result<std::optional<JournalRecord>> next = reader->next();
        if (!next.ok()) {
            ADD_FAILURE() << describe(next.error());
            break;
        }
        if (!next->has_value()) {
            break;
        }
        records.push_back(std::move(**next));
    }
    return records;
}

Bytes buildJournal(const std::vector<JournalRecord>& records, std::uint32_t version) {
    Bytes bytes = encodeJournalHeader(version);
    for (const JournalRecord& record : records) {
        const Bytes encoded = encodeRecord(record.type, record.flags, record.time, record.payload);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return bytes;
}

RecordPayload decoded(const JournalRecord& record) {
    Result<RecordPayload> payload = decodePayload(static_cast<RecordType>(record.type), record.payload);
    if (!payload.ok()) {
        ADD_FAILURE() << describe(payload.error());
        return DamageRecord{};
    }
    return std::move(payload).value();
}

JournalRecord recordOf(const RecordPayload& payload, SessionTime time) {
    JournalRecord record;
    record.type = static_cast<std::uint16_t>(typeOf(payload));
    record.flags = typeOf(payload) == RecordType::Damage ? kRecordOptional : 0;
    record.time = time;
    record.payload = encodePayload(payload);
    record.payloadRead = true;
    return record;
}

}  // namespace recovery::session::test
