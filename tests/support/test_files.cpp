#include "support/test_files.hpp"

#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace recovery::test {

namespace {

std::uint64_t splitMix64(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

}  // namespace

TempDir::TempDir() {
    std::random_device device;
    const std::uint64_t id = (static_cast<std::uint64_t>(device()) << 32) | device();
    path_ = std::filesystem::temp_directory_path() / "recovery-tests" / std::to_string(id);
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

std::vector<std::byte> makePattern(std::size_t size, std::uint64_t seed) {
    std::vector<std::byte> data(size);
    std::uint64_t state = seed;
    std::size_t i = 0;
    while (i < size) {
        std::uint64_t word = splitMix64(state);
        for (int b = 0; b < 8 && i < size; ++b, ++i) {
            data[i] = static_cast<std::byte>(word & 0xFF);
            word >>= 8;
        }
    }
    return data;
}

void writeFile(const std::filesystem::path& path, const std::vector<std::byte>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create test file");
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) {
        throw std::runtime_error("cannot write test file");
    }
}

std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open test file");
    }
    const auto size = static_cast<std::size_t>(std::filesystem::file_size(path));
    std::vector<std::byte> data(size);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

}  // namespace recovery::test
