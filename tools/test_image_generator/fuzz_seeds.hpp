#pragma once

#include "recovery/result.hpp"

#include <filesystem>

namespace recovery::test::corpus {

// Writes seed inputs for every fuzz target (fuzz/fuzz_<target>.cpp) below
// `folder`/<target>/: small partition tables, FAT32, exFAT and NTFS volumes,
// files of every format (the builders' and the embedded files of other
// encoders), journal records and files of a real session, image metadata,
// and small cards. Never overwrites a file.
[[nodiscard]] Status writeFuzzSeeds(const std::filesystem::path& folder);

}  // namespace recovery::test::corpus
