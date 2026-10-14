// The media level of ZIP archives: there is nothing of its own to decode.
// The structural level (formats/zip_format.hpp) decodes every member's data
// and checks its CRC-32 already, so that the carver can trust an archive.

#include "media_decoders.hpp"

#include <memory>

namespace recovery::validation::detail {

namespace {

class ZipMediaValidator final : public IMediaValidator {
public:
    [[nodiscard]] std::string_view formatId() const noexcept override { return "zip"; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& /*content*/,
                                               const MediaLimits& /*limits*/) const override {
        return MediaVerdict("zip members")
            .notApplicable("an archive codes no media of its own: the structural level decodes every member and "
                           "checks its CRC-32");
    }
};

}  // namespace

std::shared_ptr<const IMediaValidator> makeZipMediaValidator() {
    return std::make_shared<ZipMediaValidator>();
}

}  // namespace recovery::validation::detail
