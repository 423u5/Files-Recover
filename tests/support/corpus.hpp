#pragma once

// P20: the test corpus. Reproducible scenarios, each a disk image as a card
// or a stick that has been used would hold it (old data in its free space),
// the regions imaging could not read when there are any, and the files it
// was made from with what recovery must give back of each.
//
// Shared by the corpus tests (tests/corpus: every scenario scanned, recovered
// and compared with its originals and a recorded baseline) and by
// tools/test_image_generator, which writes the scenarios as files.

#include "recovery/result.hpp"
#include "storage/bad_region.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test::corpus {

using Bytes = std::vector<std::byte>;

// What recovery must give back of an original file.
enum class Outcome : std::uint8_t {
    // The metadata names it: a candidate at its path holds its bytes, whole.
    Named,
    // No metadata names it any more: a candidate holds its bytes, whole, under
    // a made-up name (carving).
    Carved,
    // The metadata names it, but its data is damaged or partly lost: the
    // candidate at its path is not Complete.
    NamedDamaged,
    // The metadata's candidate at its path is damaged (not Complete), but
    // carving gives its bytes back whole.
    NamedDamagedCarved,
    // Nothing gives it back whole: no candidate holds its bytes, and none at
    // its path is Complete.
    Lost,
};

// The condition the candidate holding the bytes (Named, Carved,
// NamedDamagedCarved) must have.
enum class Expected : std::uint8_t {
    Complete,
    // Nothing can check the content (a text file).
    Unverified,
    // Not checked (what the engine does is recorded in the baseline only).
    Any,
};

[[nodiscard]] std::string_view toString(Outcome outcome) noexcept;
[[nodiscard]] std::string_view toString(Expected expected) noexcept;

struct Original {
    // Where the metadata had it ("/DCIM/IMG_0001.JPG"), on its volume, as the
    // engine reports it (a deleted FAT short name has lost its first letter:
    // "/_LD.JPG"); empty when no entry ever named it.
    std::string path;
    Bytes bytes;
    Outcome outcome = Outcome::Named;
    Expected condition = Expected::Complete;
    // Named: only a deep scan gives the bytes back (fragment reconstruction);
    // a quick scan's candidate at the path may hold other bytes. Carved and
    // NamedDamagedCarved are always deep only.
    bool deepOnly = false;
    // Why, for a reader of the manifest.
    std::string note;
};

struct Scenario {
    std::string name;
    std::string summary;
    // The plan's corpus categories it covers (categories()).
    std::vector<std::string> categories;
};

struct Image {
    Bytes bytes;
    std::uint32_t sectorSize = 512;
    // Regions imaging could not read: zeros in `bytes`, recorded in the
    // image's metadata (<image>.imgmeta) as unreadable.
    std::vector<storage::BadRegion> badRegions;
    std::vector<Original> originals;
};

// The plan's test corpus categories (plan/p20 TEST CORPUS).
[[nodiscard]] const std::vector<std::string>& categories();

// Every scenario, in a fixed order.
[[nodiscard]] const std::vector<Scenario>& scenarios();
// nullptr when there is none of that name.
[[nodiscard]] const Scenario* findScenario(std::string_view name);

// Builds the scenario's image: the same bytes every time.
[[nodiscard]] Image generate(const Scenario& scenario);

// The scenario as text: what it holds and what recovery must give back of each
// file (with sizes and SHA-256).
[[nodiscard]] std::string manifest(const Scenario& scenario, const Image& image);

// Writes `<folder>/<name>.img`, its metadata when it has unreadable regions or
// sectors other than 512 bytes (`<name>.img.imgmeta`), `<name>.manifest.txt`,
// and with `originals` the original files below `<folder>/<name>.originals/`.
// Never overwrites a file.
[[nodiscard]] Status write(const std::filesystem::path& folder, const Scenario& scenario, const Image& image,
                           bool originals);

}  // namespace recovery::test::corpus
