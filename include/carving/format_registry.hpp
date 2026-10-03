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
#include "recovery/result.hpp"

#include <cstddef>
#include <memory>
#include <optional>
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

}  // namespace recovery::carving
