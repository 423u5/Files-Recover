#pragma once

// ZIP archives built here for the tests: members stored (method 0) as
// Python's zipfile writes them with ZIP_STORED, so their bytes show as they
// are (an image's signature too). Archives with compressed members are the
// samples of zip_samples.hpp.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace recovery::test {

struct ZipMember {
    std::string name;
    std::vector<std::byte> bytes;
};

// The archive of `members`, in this order: local headers and data, the
// central directory, the end of central directory record.
[[nodiscard]] std::vector<std::byte> storedArchive(const std::vector<ZipMember>& members);

}  // namespace recovery::test
