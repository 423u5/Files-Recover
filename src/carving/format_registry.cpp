#include "carving/format_registry.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace recovery::carving {

namespace {

// Extensions that stand for a format other than by its own extension or id.
constexpr std::array<std::pair<std::string_view, std::string_view>, 52> kExtensionFormats = {{
    {"jpeg", "jpeg"}, {"jpe", "jpeg"},  {"jfif", "jpeg"}, {"mov", "mp4"},  {"qt", "mp4"},    {"m4v", "mp4"},
    {"3gp", "mp4"},   {"3g2", "mp4"},   {"3gpp", "mp4"},  {"3gp2", "mp4"}, {"m4b", "m4a"},   {"m4p", "m4a"},
    {"adts", "aac"},  {"docx", "zip"},  {"docm", "zip"},  {"dotx", "zip"}, {"dotm", "zip"},  {"xlsx", "zip"},
    {"xlsm", "zip"},  {"xltx", "zip"},  {"xltm", "zip"},  {"xlam", "zip"}, {"pptx", "zip"},  {"pptm", "zip"},
    {"potx", "zip"},  {"potm", "zip"},  {"ppsx", "zip"},  {"ppsm", "zip"}, {"vsdx", "zip"},  {"xps", "zip"},
    {"oxps", "zip"},  {"odt", "zip"},   {"ods", "zip"},   {"odp", "zip"},  {"odg", "zip"},   {"epub", "zip"},
    {"jar", "zip"},   {"apk", "zip"},   {"doc", "ole2"},  {"dot", "ole2"}, {"xls", "ole2"},  {"xlt", "ole2"},
    {"xla", "ole2"},  {"ppt", "ole2"},  {"pot", "ole2"},  {"pps", "ole2"}, {"msg", "ole2"},  {"msi", "ole2"},
    {"msp", "ole2"},  {"mst", "ole2"},  {"vsd", "ole2"},  {"pub", "ole2"},
}};

std::string lowerExtension(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 == name.size()) {
        return {};
    }
    std::string extension(name.substr(dot + 1));
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension;
}

}  // namespace

std::optional<bool> startsFileNamed(const FormatRegistry& formats, std::string_view name,
                                    std::span<const std::byte> head) {
    const std::string extension = lowerExtension(name);
    if (extension.empty()) {
        return std::nullopt;
    }
    std::string_view id;
    for (const auto& [from, to] : kExtensionFormats) {
        if (from == extension) {
            id = to;
        }
    }
    bool known = false;
    for (const std::shared_ptr<const IFileFormat>& format : formats.formats()) {
        const FormatDescriptor& descriptor = format->descriptor();
        if (descriptor.id != id && descriptor.id != extension && descriptor.extension != extension) {
            continue;
        }
        known = true;
        const bool signature = std::any_of(
            descriptor.signatures.begin(), descriptor.signatures.end(), [&](const FileSignature& candidate) {
                return candidate.reach() <= head.size() && candidate.matches(head.subspan(candidate.offset));
            });
        if (signature &&
            format->checkHeader(head.first(std::min<std::size_t>(descriptor.headerSize, head.size()))).plausible) {
            return true;
        }
    }
    return known ? std::optional<bool>(false) : std::nullopt;
}

Status FormatRegistry::add(std::shared_ptr<const IFileFormat> format) {
    if (format == nullptr) {
        return makeError(ErrorCode::InvalidInput, "cannot register a null format");
    }
    const FormatDescriptor& descriptor = format->descriptor();
    if (Status valid = validateDescriptor(descriptor); !valid.ok()) {
        return valid;
    }
    if (find(descriptor.id) != nullptr) {
        return makeError(ErrorCode::InvalidInput, "format '" + descriptor.id + "' is already registered");
    }
    formats_.push_back(std::move(format));
    return success();
}

const IFileFormat* FormatRegistry::find(std::string_view id) const noexcept {
    const std::optional<std::size_t> index = indexOf(id);
    return index.has_value() ? formats_[*index].get() : nullptr;
}

std::optional<std::size_t> FormatRegistry::indexOf(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < formats_.size(); ++i) {
        if (formats_[i]->descriptor().id == id) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace recovery::carving
