#include "support/zip_builders.hpp"

#include "recovery/crc32.hpp"

#include <cstdint>
#include <utility>

namespace recovery::test {

namespace {

// 2020-06-08 14:03:16 as MS-DOS time and date.
constexpr std::uint16_t kTime = 0x7068;
constexpr std::uint16_t kDate = 0x50C8;

class Writer {
public:
    void put16(std::uint32_t value) {
        out_.push_back(static_cast<std::byte>(value & 0xFF));
        out_.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    }
    void put32(std::uint32_t value) {
        put16(value & 0xFFFF);
        put16(value >> 16);
    }
    void put(const std::string& text) {
        for (const char c : text) {
            out_.push_back(static_cast<std::byte>(c));
        }
    }
    void put(const std::vector<std::byte>& bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }
    [[nodiscard]] std::uint32_t size() const { return static_cast<std::uint32_t>(out_.size()); }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

}  // namespace

std::vector<std::byte> storedArchive(const std::vector<ZipMember>& members) {
    Writer w;
    std::vector<std::uint32_t> offsets;
    for (const ZipMember& m : members) {
        offsets.push_back(w.size());
        w.put32(0x04034B50);
        w.put16(20);  // version needed to extract: 2.0
        w.put16(0);   // flags
        w.put16(0);   // stored
        w.put16(kTime);
        w.put16(kDate);
        w.put32(crc32(m.bytes));
        w.put32(static_cast<std::uint32_t>(m.bytes.size()));
        w.put32(static_cast<std::uint32_t>(m.bytes.size()));
        w.put16(static_cast<std::uint32_t>(m.name.size()));
        w.put16(0);
        w.put(m.name);
        w.put(m.bytes);
    }
    const std::uint32_t directory = w.size();
    for (std::size_t i = 0; i < members.size(); ++i) {
        const ZipMember& m = members[i];
        w.put32(0x02014B50);
        w.put16(20);  // made by: MS-DOS, 2.0
        w.put16(20);
        w.put16(0);
        w.put16(0);
        w.put16(kTime);
        w.put16(kDate);
        w.put32(crc32(m.bytes));
        w.put32(static_cast<std::uint32_t>(m.bytes.size()));
        w.put32(static_cast<std::uint32_t>(m.bytes.size()));
        w.put16(static_cast<std::uint32_t>(m.name.size()));
        w.put16(0);  // extra fields
        w.put16(0);  // comment
        w.put16(0);  // disk
        w.put16(0);  // internal attributes
        w.put32(0);  // external attributes
        w.put32(offsets[i]);
        w.put(m.name);
    }
    const std::uint32_t directorySize = w.size() - directory;
    w.put32(0x06054B50);
    w.put16(0);
    w.put16(0);
    w.put16(static_cast<std::uint32_t>(members.size()));
    w.put16(static_cast<std::uint32_t>(members.size()));
    w.put32(directorySize);
    w.put32(directory);
    w.put16(0);
    return w.take();
}

}  // namespace recovery::test
