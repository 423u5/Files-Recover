// The session journal (P16): records are read back as they were written; a
// journal cut anywhere in its last record, or with garbage after it, ends
// torn; a record that does not check before intact ones is damage, and the
// intact records after it are counted; headers that are not a journal's, of
// another format or damaged are refused, and so are records with flags of a
// newer format; one writer at a time; appends from many threads all land.

#include "session/session_journal.hpp"

#include "session_test_support.hpp"
#include "support/test_files.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <random>
#include <thread>
#include <vector>

namespace recovery::session {
namespace {

using test::Bytes;

Bytes pattern(std::size_t size, std::uint64_t seed) {
    return ::recovery::test::makePattern(size, seed);
}

struct Written {
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    SessionTime time{};
    Bytes payload;
};

// Writes a journal of these records with JournalFile.
void writeWithJournalFile(const std::filesystem::path& path, const std::vector<Written>& records) {
    Result<std::unique_ptr<JournalFile>> journal = JournalFile::create(path);
    ASSERT_TRUE(journal.ok()) << describe(journal.error());
    for (const Written& record : records) {
        RECOVERY_ASSERT_OK(
            (*journal)->append(static_cast<RecordType>(record.type), record.flags, record.time, record.payload));
    }
}

std::vector<Written> someRecords(std::size_t count) {
    std::vector<Written> records;
    for (std::size_t i = 0; i < count; ++i) {
        Written record;
        record.type = static_cast<std::uint16_t>(1 + i % 8);
        record.flags = i % 3 == 0 ? kRecordOptional : 0;
        record.time = SessionTime{std::chrono::milliseconds{1'700'000'000'000 + static_cast<std::int64_t>(i) * 7}};
        record.payload = pattern(i * 37 % 300, i);
        records.push_back(std::move(record));
    }
    return records;
}

struct ReadBack {
    std::vector<JournalRecord> records;
    JournalEnd end = JournalEnd::Clean;
    std::uint64_t validEnd = 0;
    std::string reason;
};

ReadBack readBack(const std::filesystem::path& path) {
    ReadBack result;
    Result<JournalReader> reader = JournalReader::open(path);
    EXPECT_TRUE(reader.ok()) << (reader.ok() ? std::string() : describe(reader.error()));
    if (!reader.ok()) {
        return result;
    }
    for (;;) {
        Result<std::optional<JournalRecord>> next = reader->next();
        EXPECT_TRUE(next.ok()) << (next.ok() ? std::string() : describe(next.error()));
        if (!next.ok() || !next->has_value()) {
            break;
        }
        result.records.push_back(std::move(**next));
    }
    result.end = reader->end();
    result.validEnd = reader->validEnd();
    result.reason = reader->endReason();
    return result;
}

TEST(SessionJournalTest, RecordsAreReadBackAsTheyWereWritten) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path path = dir / "session.journal";
    const std::vector<Written> written = someRecords(20);
    writeWithJournalFile(path, written);

    const ReadBack back = readBack(path);
    ASSERT_EQ(back.records.size(), written.size());
    std::uint64_t offset = kJournalHeaderSize;
    for (std::size_t i = 0; i < written.size(); ++i) {
        const JournalRecord& record = back.records[i];
        EXPECT_EQ(record.type, written[i].type);
        EXPECT_EQ(record.flags, written[i].flags);
        EXPECT_EQ(record.optional(), written[i].flags == kRecordOptional);
        EXPECT_EQ(record.time, written[i].time);
        EXPECT_EQ(record.payload, written[i].payload);
        EXPECT_TRUE(record.payloadRead);
        EXPECT_EQ(record.offset, offset);
        EXPECT_EQ(record.size, kRecordHeaderSize + written[i].payload.size());
        offset += record.size;
    }
    EXPECT_EQ(back.end, JournalEnd::Clean);
    EXPECT_EQ(back.validEnd, std::filesystem::file_size(path));

    // Payloads of other types are skipped unread.
    Result<JournalReader> reader = JournalReader::open(path);
    RECOVERY_ASSERT_OK(reader);
    std::size_t read = 0;
    std::size_t skipped = 0;
    for (;;) {
        Result<std::optional<JournalRecord>> next = reader->next([](std::uint16_t type) { return type == 3; });
        RECOVERY_ASSERT_OK(next);
        if (!next->has_value()) {
            break;
        }
        if ((*next)->payloadRead) {
            EXPECT_EQ((*next)->type, 3);
            ++read;
        } else {
            EXPECT_TRUE((*next)->payload.empty());
            ++skipped;
        }
    }
    EXPECT_EQ(read + skipped, written.size());
    EXPECT_GT(read, 0U);
    EXPECT_GT(skipped, 0U);
}

TEST(SessionJournalTest, AJournalCutAnywhereInItsLastRecordEndsTorn) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path whole = dir / "whole.journal";
    writeWithJournalFile(whole, someRecords(4));
    const Bytes bytes = ::recovery::test::readFile(whole);
    const ReadBack all = readBack(whole);
    ASSERT_EQ(all.records.size(), 4U);
    const std::uint64_t lastStart = all.records.back().offset;

    const std::filesystem::path cut = dir / "cut.journal";
    for (std::uint64_t size = lastStart; size < bytes.size(); ++size) {
        SCOPED_TRACE(size);
        ::recovery::test::writeFile(cut, Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)));
        const ReadBack back = readBack(cut);
        EXPECT_EQ(back.records.size(), 3U);
        EXPECT_EQ(back.validEnd, lastStart);
        EXPECT_EQ(back.end, size == lastStart ? JournalEnd::Clean : JournalEnd::TornTail);
    }

    // Garbage after the last record: what a crash leaves when the file grew
    // and its data did not reach the device.
    for (const Bytes& garbage : {Bytes(4096, std::byte{0}), pattern(5000, 7), Bytes{std::byte{'S'}}}) {
        Bytes longer = bytes;
        longer.insert(longer.end(), garbage.begin(), garbage.end());
        ::recovery::test::writeFile(cut, longer);
        const ReadBack back = readBack(cut);
        EXPECT_EQ(back.records.size(), 4U);
        EXPECT_EQ(back.end, JournalEnd::TornTail);
        EXPECT_EQ(back.validEnd, bytes.size());
    }
}

TEST(SessionJournalTest, DamageBeforeIntactRecordsIsToldFromATornTail) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path whole = dir / "whole.journal";
    std::vector<Written> written = someRecords(6);
    for (Written& record : written) {
        record.payload = pattern(200, record.payload.size());
    }
    writeWithJournalFile(whole, written);
    const Bytes bytes = ::recovery::test::readFile(whole);
    const ReadBack all = readBack(whole);
    ASSERT_EQ(all.records.size(), 6U);

    const std::filesystem::path damaged = dir / "damaged.journal";
    struct Case {
        std::size_t record;
        // Bytes into the record: in its header (magic, type, length, CRCs) or its payload.
        std::uint64_t at;
        JournalEnd end;
    };
    const std::vector<Case> cases = {
        {2, 0, JournalEnd::Damaged},  {2, 5, JournalEnd::Damaged},   {2, 17, JournalEnd::Damaged},
        {2, 30, JournalEnd::Damaged}, {2, 100, JournalEnd::Damaged}, {0, 40, JournalEnd::Damaged},
        {5, 3, JournalEnd::TornTail}, {5, 150, JournalEnd::TornTail},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::to_string(c.record) + " at " + std::to_string(c.at));
        Bytes copy = bytes;
        copy[all.records[c.record].offset + c.at] ^= std::byte{0x40};
        ::recovery::test::writeFile(damaged, copy);
        const ReadBack back = readBack(damaged);
        EXPECT_EQ(back.records.size(), c.record);
        EXPECT_EQ(back.end, c.end);
        EXPECT_EQ(back.validEnd, all.records[c.record].offset);
        EXPECT_FALSE(back.reason.empty());
        Result<JournalReader> reader = JournalReader::open(damaged);
        RECOVERY_ASSERT_OK(reader);
        const Result<std::uint64_t> intact = reader->countIntactRecords(back.validEnd);
        RECOVERY_ASSERT_OK(intact);
        EXPECT_EQ(*intact, 5 - c.record);
    }
}

TEST(SessionJournalTest, HeadersThatAreNotAJournalsOfThisFormatAreRefused) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path path = dir / "session.journal";
    const auto refusedWith = [&](const Bytes& bytes, std::string_view why) {
        ::recovery::test::writeFile(path, bytes);
        const Result<JournalReader> reader = JournalReader::open(path);
        ASSERT_FALSE(reader.ok()) << why;
        EXPECT_EQ(reader.error().code, ErrorCode::InvalidFormat);
        EXPECT_NE(reader.error().message.find(why), std::string::npos) << reader.error().message;
        // Refusing changes nothing.
        EXPECT_EQ(::recovery::test::readFile(path), bytes);
    };
    refusedWith({}, "not a session journal");
    refusedWith(pattern(100, 1), "not a session journal");
    const Bytes header = encodeJournalHeader();
    refusedWith(Bytes(header.begin(), header.begin() + 10), "not a session journal");
    refusedWith(encodeJournalHeader(kJournalFormatVersion + 1), "newer than this engine reads");
    refusedWith(encodeJournalHeader(0), "does not read");
    Bytes damaged = encodeJournalHeader();
    damaged[9] ^= std::byte{1};
    refusedWith(damaged, "damaged");

    // A record with a flag of a newer format.
    Bytes withFlag = encodeJournalHeader();
    const Bytes record = encodeRecord(3, 0x0002, SessionTime{}, pattern(10, 2));
    withFlag.insert(withFlag.end(), record.begin(), record.end());
    ::recovery::test::writeFile(path, withFlag);
    Result<JournalReader> reader = JournalReader::open(path);
    RECOVERY_ASSERT_OK(reader);
    const Result<std::optional<JournalRecord>> next = reader->next();
    ASSERT_FALSE(next.ok());
    EXPECT_EQ(next.error().code, ErrorCode::InvalidFormat);
    EXPECT_NE(next.error().message.find("newer"), std::string::npos) << next.error().message;

    // No file at all.
    const Result<JournalReader> missing = JournalReader::open(dir / "nothing.journal");
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::InvalidInput);
}

TEST(SessionJournalTest, OneWriterAtATime) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path path = dir / "session.journal";
    writeWithJournalFile(path, someRecords(3));
    {
        Result<std::unique_ptr<JournalFile>> first = JournalFile::open(path);
        RECOVERY_ASSERT_OK(first);
        const Result<std::unique_ptr<JournalFile>> second = JournalFile::open(path);
        ASSERT_FALSE(second.ok());
        EXPECT_EQ(second.error().code, ErrorCode::DestinationError);
        EXPECT_NE(second.error().message.find("in use"), std::string::npos) << second.error().message;
        // Reading is not writing: a journal in use can be read.
        EXPECT_EQ(readBack(path).records.size(), 3U);
        // Creating one where one exists fails.
        EXPECT_FALSE(JournalFile::create(path).ok());
    }
    // Closed: it opens again, and appends after its last record.
    Result<std::unique_ptr<JournalFile>> again = JournalFile::open(path);
    RECOVERY_ASSERT_OK(again);
    RECOVERY_ASSERT_OK((*again)->append(RecordType::Damage, kRecordOptional, SessionTime{}, pattern(5, 9)));
    again->reset();
    EXPECT_EQ(readBack(path).records.size(), 4U);
}

TEST(SessionJournalTest, ATruncatedJournalGoesOnAfterItsLastRecord) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path path = dir / "session.journal";
    writeWithJournalFile(path, someRecords(3));
    const ReadBack before = readBack(path);
    ASSERT_EQ(before.records.size(), 3U);
    {
        Result<std::unique_ptr<JournalFile>> journal = JournalFile::open(path);
        RECOVERY_ASSERT_OK(journal);
        RECOVERY_ASSERT_OK((*journal)->truncate(before.records[2].offset));
        EXPECT_EQ((*journal)->end(), before.records[2].offset);
        RECOVERY_ASSERT_OK((*journal)->append(RecordType::JobState, 0, SessionTime{}, pattern(64, 3)));
    }
    const ReadBack after = readBack(path);
    ASSERT_EQ(after.records.size(), 3U);
    EXPECT_EQ(after.records[1].payload, before.records[1].payload);
    EXPECT_EQ(after.records[2].payload, pattern(64, 3));
    EXPECT_EQ(after.end, JournalEnd::Clean);

    // A broken journal refuses every append.
    Result<std::unique_ptr<JournalFile>> journal = JournalFile::open(path);
    RECOVERY_ASSERT_OK(journal);
    (*journal)->markBroken(makeError(ErrorCode::InternalError, "out of step"));
    EXPECT_TRUE((*journal)->broken());
    const Status refused = (*journal)->append(RecordType::JobState, 0, SessionTime{}, pattern(4, 4));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().message, "out of step");
}

TEST(SessionJournalTest, AppendsFromManyThreadsAllLand) {
    ::recovery::test::TempDir dir;
    const std::filesystem::path path = dir / "session.journal";
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kEach = 150;
    {
        Result<std::unique_ptr<JournalFile>> journal = JournalFile::create(path);
        RECOVERY_ASSERT_OK(journal);
        JournalFile& file = **journal;
        std::vector<std::thread> threads;
        for (std::size_t t = 0; t < kThreads; ++t) {
            threads.emplace_back([&file, t] {
                for (std::size_t i = 0; i < kEach; ++i) {
                    Bytes payload = pattern(1 + (i * 13 + t * 7) % 500, t * 1000 + i);
                    payload[0] = static_cast<std::byte>(t);
                    EXPECT_TRUE(file.append(RecordType::FileStarted, 0, SessionTime{}, payload).ok());
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
    const ReadBack back = readBack(path);
    ASSERT_EQ(back.records.size(), kThreads * kEach);
    EXPECT_EQ(back.end, JournalEnd::Clean);
    // Each thread's records, in the order it wrote them.
    std::vector<std::size_t> next(kThreads, 0);
    for (const JournalRecord& record : back.records) {
        const auto t = static_cast<std::size_t>(record.payload.at(0));
        ASSERT_LT(t, kThreads);
        const std::size_t i = next[t]++;
        Bytes expected = pattern(1 + (i * 13 + t * 7) % 500, t * 1000 + i);
        expected[0] = static_cast<std::byte>(t);
        EXPECT_EQ(record.payload, expected);
    }
}

}  // namespace
}  // namespace recovery::session
