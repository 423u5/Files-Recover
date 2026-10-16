#pragma once

// The set of formats a scan looks for.
//
// Formats are added explicitly: a format module (images in P9, audio in P10,
// ...) provides a function that adds its formats to a registry, and the
// application calls the ones it wants. There is deliberately no registration
// by static initialisers: the engine is built from static libraries, and the
// linker drops object files that nothing references, so such formats would
// silently go missing.

#include "carving/file_format.hpp"
#include "recovery/config.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace recovery::carving {

// Thread safety: add() must not run concurrently with anything else. The
// const functions may be called concurrently. Scanners take a snapshot of
// the formats when they are created, so adding formats later does not
// affect them.
class FormatRegistry {
public:
    // Adds `format` after the formats already registered. Fails with
    // InvalidInput, leaving the registry unchanged, when `format` is null,
    // its descriptor breaks a rule (validateDescriptor), or its id is taken.
    [[nodiscard]] Status add(std::shared_ptr<const IFileFormat> format);

    // The format with this id, or nullptr.
    [[nodiscard]] const IFileFormat* find(std::string_view id) const noexcept;
    // Position of the format with this id in formats(), if any.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view id) const noexcept;

    // In registration order. Hits at the same offset are reported in this order.
    [[nodiscard]] const std::vector<std::shared_ptr<const IFileFormat>>& formats() const noexcept { return formats_; }
    [[nodiscard]] std::size_t size() const noexcept { return formats_.size(); }
    [[nodiscard]] bool empty() const noexcept { return formats_.empty(); }

private:
    std::vector<std::shared_ptr<const IFileFormat>> formats_;
};

// Whether `head`, the first bytes at some place, start a file named `name`:
// true when a format the name's extension stands for matches one of its
// signatures there and its header check takes them, false when none does,
// and nullopt when no format stands for the extension (so a call with an
// empty head asks only that). An extension stands for the format whose
// extension or id it is, or by its common variants (".jpeg", ".mov", ...);
// Office, OpenDocument, EPUB and Java/Android packages for ZIP; Office 97-2003
// documents, Outlook messages and Windows Installer packages for OLE2.
// Places where a deleted file may start are told apart by it (FAT32, L7).
[[nodiscard]] std::optional<bool> startsFileNamed(const FormatRegistry& formats, std::string_view name,
                                                  std::span<const std::byte> head);

// Whether the file named `name` that would start at `offset` of `source`
// ends exactly `size` bytes on, by the structure of a format its extension
// stands for (as startsFileNamed): its header is taken there, and its end
// detection Finds the end at `size` (at most kEndCheckSlack bytes after it
// are read). False when it ends elsewhere, its end cannot be told, the name
// stands for no format, or the source fails. When several places start a
// file of its type, it tells where a deleted FAT32 file is (L242).
inline constexpr std::uint64_t kEndCheckSlack = 64 * kKiB;
[[nodiscard]] bool endsFileNamed(const FormatRegistry& formats, storage::IStorageSource& source, std::string_view name,
                                 std::uint64_t offset, std::uint64_t size);

}  // namespace recovery::carving
