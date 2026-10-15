#pragma once

// OLE2 compound files ([MS-CFB]) built here for the tests: version 3
// (512-byte sectors) or 4 (4096-byte sectors), streams in the root or in
// storages under it, the small ones in the mini stream. The file holds, in
// this order: the header, every large stream's sectors, the mini stream,
// the mini FAT, the directory, the FAT and the DIFAT sectors the FAT needs
// beyond the header's 109. The directory's trees are red-black trees, as
// [MS-CFB] 2.6.4 orders them. Files written by Windows' own implementation
// are the samples of document_samples.hpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recovery::test {

struct Ole2Stream {
    // The storage the stream is in ("" for the root), and its name (ASCII).
    std::string storage;
    std::string name;
    std::vector<std::byte> data;
};

struct Ole2Options {
    // 3 or 4.
    int version = 3;
    // The root entry's class (all zeros: none).
    std::array<std::uint8_t, 16> rootClass{};
    // Free sectors left at the end of the file (inside the FAT's last sector).
    std::size_t trailingFreeSectors = 0;
};

[[nodiscard]] std::vector<std::byte> compoundFile(const std::vector<Ole2Stream>& streams,
                                                  const Ole2Options& options = {});

// Word 97-2003, Excel 97-2003 and PowerPoint 97-2003 files as their
// applications name their streams (the streams' data is a pattern), and an
// Outlook message.
[[nodiscard]] std::vector<std::byte> wordDocument(std::size_t textBytes = 9000, std::uint64_t seed = 1);
[[nodiscard]] std::vector<std::byte> excelWorkbook(std::size_t bytes = 12000, std::uint64_t seed = 2);
[[nodiscard]] std::vector<std::byte> powerPointPresentation(std::size_t bytes = 20000, std::uint64_t seed = 3);
[[nodiscard]] std::vector<std::byte> outlookMessage(std::uint64_t seed = 4);

}  // namespace recovery::test
