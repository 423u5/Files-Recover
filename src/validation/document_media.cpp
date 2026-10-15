// The media level of PDF documents and OLE2 compound files: nothing of
// their own to decode. The structural level of PDF (formats/pdf_format.hpp)
// inflates every Flate stream and checks every JPEG image already, and an
// OLE2 file holds no checksums to check (formats/ole2_format.hpp).

#include "media_decoders.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace recovery::validation::detail {

namespace {

class DocumentMediaValidator final : public IMediaValidator {
public:
    DocumentMediaValidator(std::string_view id, std::string checker, std::string why)
        : id_(id), checker_(std::move(checker)), why_(std::move(why)) {}

    [[nodiscard]] std::string_view formatId() const noexcept override { return id_; }

    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& /*content*/,
                                               const MediaLimits& /*limits*/) const override {
        return MediaVerdict(checker_).notApplicable(why_);
    }

private:
    std::string_view id_;
    std::string checker_;
    std::string why_;
};

}  // namespace

std::shared_ptr<const IMediaValidator> makePdfMediaValidator() {
    return std::make_shared<DocumentMediaValidator>(
        "pdf", "pdf streams",
        "a document codes no media of its own: the structural level inflates every Flate stream and checks every "
        "JPEG image");
}

std::shared_ptr<const IMediaValidator> makeOle2MediaValidator() {
    return std::make_shared<DocumentMediaValidator>(
        "ole2", "ole2 streams",
        "a compound file codes no media of its own, and holds no checksums of its streams' data");
}

}  // namespace recovery::validation::detail
