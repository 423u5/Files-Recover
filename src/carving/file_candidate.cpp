#include "carving/file_candidate.hpp"

#include "recovery/checked_math.hpp"

#include <algorithm>
#include <string>

namespace recovery::carving {

std::string_view toString(CarveWarning warning) noexcept {
    switch (warning) {
    case CarveWarning::TruncatedBySourceEnd:
        return "TruncatedBySourceEnd";
    case CarveWarning::TruncatedByMaximumSize:
        return "TruncatedByMaximumSize";
    case CarveWarning::StructureBroken:
        return "StructureBroken";
    case CarveWarning::EndUnknown:
        return "EndUnknown";
    case CarveWarning::UnreadableData:
        return "UnreadableData";
    }
    return "Unknown";
}

bool FileCandidate::hasWarning(CarveWarning warning) const noexcept {
    return std::find(warnings.begin(), warnings.end(), warning) != warnings.end();
}

Status validateFileCandidate(const FileCandidate& candidate) {
    const auto invalid = [&](const std::string& what) {
        return makeError(ErrorCode::InvalidInput,
                         "carved candidate " + std::to_string(candidate.id.value()) + ": " + what);
    };
    if (candidate.length == 0) {
        return invalid("empty candidate");
    }
    if (!checkedAdd(candidate.sourceOffset, candidate.length).has_value()) {
        return invalid("length overflows the source offset range");
    }
    std::uint64_t position = 0;
    for (const CarvedExtent& extent : candidate.extents) {
        if (extent.fileOffset != position) {
            return invalid("extents are not contiguous at file offset " + std::to_string(position));
        }
        if (extent.length == 0) {
            return invalid("empty extent at file offset " + std::to_string(position));
        }
        if (!checkedAdd(extent.sourceOffset, extent.length).has_value()) {
            return invalid("extent overflows the source offset range");
        }
        const std::optional<std::uint64_t> end = checkedAdd(extent.fileOffset, extent.length);
        if (!end.has_value()) {
            return invalid("extent length overflows");
        }
        position = *end;
    }
    if (position != candidate.length) {
        return invalid("extents cover " + std::to_string(position) + " bytes, expected " +
                       std::to_string(candidate.length));
    }
    switch (candidate.extraction) {
    case ExtractionStrategy::Contiguous:
        if (candidate.extents.size() != 1 || candidate.extents.front().sourceOffset != candidate.sourceOffset) {
            return invalid("a contiguous candidate needs exactly one extent at its source offset");
        }
        break;
    default:
        return invalid("unknown extraction strategy");
    }
    return success();
}

}  // namespace recovery::carving
