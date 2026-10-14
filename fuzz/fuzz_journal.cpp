// Fuzz target: what a session reads back from its folder after a crash or
// damage. With the first byte's top bit clear, its low bits pick a record type
// and the rest is that record's payload: it is decoded, and whatever decodes
// must encode and decode again to the same bytes. With the top bit set, the
// rest is a whole journal file, read record by record as a session opening it
// would (header, records, CRCs, torn tails and damage).

#include "fuzz_support.hpp"
#include "session/session_journal.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

const std::filesystem::path& journalFile() {
    static const std::filesystem::path path = [] {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "recovery-fuzz";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        return folder / ("journal-" + std::to_string(GetCurrentProcessId()) + ".bin");
    }();
    return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace recovery;
    using namespace recovery::session;
    if (size < 1) {
        return 0;
    }
    const std::span<const std::byte> rest = fuzz::bytesOf(data + 1, size - 1);
    if ((data[0] & 0x80) == 0) {
        const auto type = static_cast<RecordType>(1 + (data[0] & 0x7F) % 8);
        const Result<RecordPayload> decoded = decodePayload(type, rest);
        if (decoded.ok()) {
            const std::vector<std::byte> encoded = encodePayload(*decoded);
            const Result<RecordPayload> again = decodePayload(type, encoded);
            fuzz::require(again.ok(), "a payload that decodes encodes to bytes that do not");
            fuzz::require(encodePayload(*again) == encoded, "a payload does not encode the same twice");
        }
        return 0;
    }
    {
        std::ofstream out(journalFile(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(rest.data()), static_cast<std::streamsize>(rest.size()));
        fuzz::require(out.good(), "writing the journal file");
    }
    Result<JournalReader> reader = JournalReader::open(journalFile());
    if (!reader.ok()) {
        return 0;
    }
    std::uint64_t previousEnd = 0;
    for (int i = 0; i < 100000; ++i) {
        Result<std::optional<JournalRecord>> record = reader->next();
        if (!record.ok() || !record->has_value()) {
            break;
        }
        const JournalRecord& read = **record;
        fuzz::require(read.offset >= previousEnd && read.size <= reader->fileSize(), "a record out of place");
        previousEnd = read.offset + read.size;
        if (read.payloadRead && read.type >= 1 && read.type <= 8) {
            (void)decodePayload(static_cast<RecordType>(read.type), read.payload);
        }
    }
    return 0;
}
