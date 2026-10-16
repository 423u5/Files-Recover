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

// The formats the extension of `name` stands for, in registration order.
std::vector<const IFileFormat*> formatsNamed(const FormatRegistry& formats, std::string_view name) {
    std::vector<const IFileFormat*> named;
    const std::string extension = lowerExtension(name);
    if (extension.empty()) {
        return named;
    }
    std::string_view id;
    for (const auto& [from, to] : kExtensionFormats) {
        if (from == extension) {
            id = to;
        }
    }
    for (const std::shared_ptr<const IFileFormat>& format : formats.formats()) {
        const FormatDescriptor& descriptor = format->descriptor();
        if (descriptor.id == id || descriptor.id == extension || descriptor.extension == extension) {
            named.push_back(format.get());
        }
    }
    return named;
}

// Whether `head` holds one of the format's signatures and its header check takes it.
bool startsFormat(const IFileFormat& format, std::span<const std::byte> head) {
    const FormatDescriptor& descriptor = format.descriptor();
    const bool signature =
        std::any_of(descriptor.signatures.begin(), descriptor.signatures.end(), [&](const FileSignature& candidate) {
            return candidate.reach() <= head.size() && candidate.matches(head.subspan(candidate.offset));
        });
    return signature &&
           format.checkHeader(head.first(std::min<std::size_t>(descriptor.headerSize, head.size()))).plausible;
}

}  // namespace

std::optional<bool> startsFileNamed(const FormatRegistry& formats, std::string_view name,
                                    std::span<const std::byte> head) {
    const std::vector<const IFileFormat*> named = formatsNamed(formats, name);
    if (named.empty()) {
        return std::nullopt;
    }
    return std::any_of(named.begin(), named.end(), [&](const IFileFormat* format) { return startsFormat(*format, head); });
}

bool endsFileNamed(const FormatRegistry& formats, storage::IStorageSource& source, std::string_view name,
                   std::uint64_t offset, std::uint64_t size) {
    if (size == 0 || offset >= source.size() || size > source.size() - offset) {
        return false;
    }
    const std::uint64_t slack = std::min(kEndCheckSlack, source.size() - offset - size);
    for (const IFileFormat* format : formatsNamed(formats, name)) {
        const FormatDescriptor& descriptor = format->descriptor();
        if (size < descriptor.minimumSize || size > descriptor.maximumSize) {
            continue;
        }
        Result<std::unique_ptr<SourceContentReader>> reader =
            SourceContentReader::open(source, offset, std::min(size + slack, descriptor.maximumSize));
        if (!reader.ok()) {
            continue;
        }
        const Result<std::span<const std::byte>> header =
            (*reader)->read(0, static_cast<std::size_t>(std::min<std::uint64_t>(descriptor.headerSize, size)));
        if (!header.ok() || !startsFormat(*format, *header)) {
            continue;
        }
        const Result<EndDetection> end = format->findEnd(**reader);
        if (end.ok() && end->status == EndStatus::Found && end->length == size) {
            return true;
        }
    }
    return false;
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
