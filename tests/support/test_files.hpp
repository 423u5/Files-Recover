#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace recovery::test {

// Unique directory under %TEMP%\recovery-tests, removed on destruction.
// Tests only ever create files here; they never touch real devices.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

// Deterministic pseudo-random bytes (SplitMix64). Every byte depends on its
// position, so misplaced or shifted data is always detected.
[[nodiscard]] std::vector<std::byte> makePattern(std::size_t size, std::uint64_t seed = 0x5EED);

void writeFile(const std::filesystem::path& path, const std::vector<std::byte>& data);
[[nodiscard]] std::vector<std::byte> readFile(const std::filesystem::path& path);

}  // namespace recovery::test
