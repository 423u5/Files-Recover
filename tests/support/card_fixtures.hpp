#pragma once

// The files of the scan tests' card (tests/scan/scan_test_support.hpp, makeCard)
// as recovery should give them back, simulated physical disks, and a disk
// of three volumes: shared by the CLI (P18) and API (P19) tests.

#include "support/fake_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace recovery::test {

using Bytes = std::vector<std::byte>;

// The files the card was made from, by the name recovery gives them (the
// deleted ones lost their first letter: "_LD.JPG"). Files only carving finds
// are not here: their names depend on the order of carving.
[[nodiscard]] const std::map<std::string, Bytes>& cardOriginals();

// The files below `root`: relative path (with '/') -> bytes.
[[nodiscard]] std::map<std::string, Bytes> filesBelow(const std::filesystem::path& root);

// A simulated physical disk: `bytes`, its reads failing (ERROR_CRC) in the
// byte ranges `failing`. Give `opener` to the CLI's environment or to the
// API's platform hooks.
struct SimulatedDisk {
    std::shared_ptr<FakeDeviceConfig> config;
    std::shared_ptr<MockDeviceOpener> opener;
};
[[nodiscard]] SimulatedDisk simulatedDisk(Bytes bytes,
                                          std::vector<std::pair<std::uint64_t, std::uint64_t>> failing = {});

// A disk with an MBR and three partitions: FAT32, exFAT and NTFS, one file each.
[[nodiscard]] Bytes threeVolumeDisk();

}  // namespace recovery::test
