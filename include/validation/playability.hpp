#pragma once

// Playability validation (P14): a platform decoder decodes the whole file, as
// a viewer or a player would. It is the last validation level, optional and
// off by default: platform decoders are not the engine's, they differ between
// machines (installed codecs), and they parse the untrusted content in this
// process. The Windows implementation is windows_playability.hpp.

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"
#include "validation/validation.hpp"

#include <string_view>

namespace recovery::validation {

class IPlayabilityChecker {
public:
    virtual ~IPlayabilityChecker() = default;

    // Decodes the whole file whose bytes are all of `content`. `formatId`
    // (FormatDescriptor::id) and `extension` (without the dot) say what the
    // file is, so the right decoder is chosen. Passed when every frame,
    // image or sample decodes; Failed with the decoder's error otherwise;
    // Unsupported when the platform has no decoder for it. Errors are only
    // the reader's (Cancelled, source failures).
    [[nodiscard]] virtual Result<LevelResult> check(carving::IContentReader& content, std::string_view formatId,
                                                    std::string_view extension) = 0;

protected:
    IPlayabilityChecker() = default;
    IPlayabilityChecker(const IPlayabilityChecker&) = default;
    IPlayabilityChecker& operator=(const IPlayabilityChecker&) = default;
    IPlayabilityChecker(IPlayabilityChecker&&) = default;
    IPlayabilityChecker& operator=(IPlayabilityChecker&&) = default;
};

}  // namespace recovery::validation
