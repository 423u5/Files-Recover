#pragma once

// Helpers shared by the format tests: running a format's steps on bytes in
// memory, the properties every intact file must have, byte surgery for
// corruption tests, deterministic mutation fuzzing, and carving a source
// with the image formats, the audio formats or both.

#include "carving/file_carver.hpp"
#include "carving/file_format.hpp"
#include "storage/storage_source.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace recovery::formats::testing {

using Bytes = std::vector<std::byte>;

[[nodiscard]] carving::HeaderCheck headerOf(const carving::IFileFormat& format, std::span<const std::byte> data);
[[nodiscard]] carving::EndDetection endOf(const carving::IFileFormat& format, std::span<const std::byte> data);
[[nodiscard]] carving::ValidationResult verdictOf(const carving::IFileFormat& format,
                                                  std::span<const std::byte> data);

[[nodiscard]] std::string describe(const carving::EndDetection& end);
[[nodiscard]] std::string describe(const carving::ValidationResult& result);

// A signature of the format matches at the start, the header is accepted,
// end detection finds the end at file.size() (also when more data follows),
// and validation says Valid for all of it.
[[nodiscard]] ::testing::AssertionResult isIntact(const carving::IFileFormat& format, std::span<const std::byte> file);

// End detection and validation of every proper prefix (all of the first
// 4 KiB, then a sample) say Truncated: a file cut short is never Valid. The
// prefixes of the lengths in `completeAt` are complete files themselves (a
// stream of frames cut at a frame boundary, an MP3 without its ID3v1 tag):
// they must be Found and Valid instead. Prefixes shorter than `rejectedBelow`
// may be rejected instead (Broken at 0: too little to be a file, such as a
// stream cut inside its first frame).
[[nodiscard]] ::testing::AssertionResult prefixesAreTruncated(const carving::IFileFormat& format,
                                                              std::span<const std::byte> file,
                                                              std::span<const std::size_t> completeAt = {},
                                                              std::size_t rejectedBelow = 0);

// Validation says Invalid, with at most `validBytes` consistent bytes when given.
[[nodiscard]] ::testing::AssertionResult isInvalid(const carving::IFileFormat& format, std::span<const std::byte> file,
                                                   std::optional<std::uint64_t> validBytes = std::nullopt);

// Byte surgery.
[[nodiscard]] Bytes concat(std::initializer_list<std::span<const std::byte>> parts);
[[nodiscard]] Bytes overwritten(Bytes file, std::size_t offset, std::initializer_list<std::uint8_t> bytes);
[[nodiscard]] Bytes overwritten(Bytes file, std::size_t offset, std::span<const std::byte> bytes);
[[nodiscard]] Bytes inserted(const Bytes& file, std::size_t offset, std::span<const std::byte> bytes);
[[nodiscard]] Bytes erased(const Bytes& file, std::size_t offset, std::size_t count);
[[nodiscard]] Bytes prefix(const Bytes& file, std::size_t length);
// Deterministic noise without the first byte of any image signature
// (0xFF, 0x89, 'R', 'G', 'B'), so it never starts a file by itself.
[[nodiscard]] Bytes quietNoise(std::size_t size, std::uint64_t seed);
// The same without the first byte of any audio signature (0xFF, 'I', 'R',
// and 'f' of "ftyp", which lies 4 bytes into a file).
[[nodiscard]] Bytes quietAudioNoise(std::size_t size, std::uint64_t seed);
// Deterministic noise with every byte value.
[[nodiscard]] Bytes noise(std::size_t size, std::uint64_t seed);

// Runs the format's steps on `iterations` damaged copies of `file` (bytes
// replaced, cut short, lengths inflated) and checks that every result stays
// within the data it was given. Crashes, hangs and out-of-bounds reads are
// what this is for (AddressSanitizer build).
void fuzz(const carving::IFileFormat& format, const Bytes& file, int iterations, std::uint64_t seed);

// Carving a whole source with registerImageFormats(), registerAudioFormats(),
// registerVideoFormats() (P12), or several of them.
struct Carved {
    carving::CarveReport report;
    std::vector<carving::FileCandidate> candidates;
};

// All: images and audio. Everything: images, audio and video.
enum class Formats : std::uint8_t { Images, Audio, All, Video, Everything };

[[nodiscard]] Carved carve(storage::IStorageSource& source, Formats formats, carving::CarveOptions options = {});
[[nodiscard]] Carved carveImages(storage::IStorageSource& source, carving::CarveOptions options = {});
[[nodiscard]] Carved carveAudio(storage::IStorageSource& source, carving::CarveOptions options = {});
[[nodiscard]] Bytes carvedBytes(storage::IStorageSource& source, const carving::FileCandidate& candidate);
[[nodiscard]] std::string describe(const carving::FileCandidate& candidate);

// The directory the environment variable `name` names, if any (for the
// optional reference corpora and builder exports). `mustExist` is false where
// the test creates the directory itself.
[[nodiscard]] std::optional<std::filesystem::path> directoryFromEnvironment(const wchar_t* name,
                                                                           bool mustExist = true);

}  // namespace recovery::formats::testing
