#pragma once

// Helpers shared by the MP4 parser tests (P11): parsing bytes in memory,
// comparing what the parser found with where the builder put every sample,
// a content reader that assembles a file from scattered pieces, and
// mutation fuzzing of the parser.

#include "carving/content_reader.hpp"
#include "formats/mp4_parser.hpp"
#include "support/mp4_builders.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace recovery::formats::testing {

using Bytes = std::vector<std::byte>;

// parseFile() on the bytes; a failed Result fails the test (in-memory reads never fail).
[[nodiscard]] mp4::Mp4File parseBytes(std::span<const std::byte> bytes, const mp4::ParseLimits& limits = {});

// Status, detail and every recorded issue, for failure messages.
[[nodiscard]] std::string describe(const mp4::Mp4File& file);

// The file is Valid and its tracks, chunks and samples are exactly where the builder wrote them.
[[nodiscard]] ::testing::AssertionResult matchesBuilt(const mp4::Mp4File& file, const test::Mp4Built& built);

// Some recorded issue's detail contains `text`.
[[nodiscard]] bool hasIssue(const mp4::Mp4File& file, std::string_view text);

// Content made of pieces of other memory, in order: offset 0 is the first
// byte of the first piece. Reads that cross pieces are assembled.
class ScatteredContentReader final : public carving::IContentReader {
public:
    explicit ScatteredContentReader(std::vector<std::span<const std::byte>> pieces);

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t offset, std::size_t length) override;

    [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

private:
    std::vector<std::span<const std::byte>> pieces_;
    std::uint64_t size_ = 0;
    Bytes buffer_;
    std::size_t reads_ = 0;
};

// Runs the parser on `iterations` damaged copies of `file` (bytes replaced,
// cut short, lengths inflated, runs of zeros or noise) and checks that every
// result stays within the data: boxes inside the content, placements that
// agree with the content's size, Valid only without issues. Crashes, hangs
// and out-of-bounds reads are what this is for (AddressSanitizer build).
// Returns how many copies were still Valid.
int fuzzParser(const Bytes& file, int iterations, std::uint64_t seed);

}  // namespace recovery::formats::testing
