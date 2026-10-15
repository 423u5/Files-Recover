#include "metadata/media_metadata.hpp"

#include "extraction.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace recovery::metadata {

namespace {

struct FormatInfo {
    std::string_view id;
    MediaKind kind;
    std::string_view mediaType;
};

constexpr std::array<FormatInfo, 13> kFormats = {{
    {"jpeg", MediaKind::Image, "image/jpeg"},
    {"png", MediaKind::Image, "image/png"},
    {"gif", MediaKind::Image, "image/gif"},
    {"bmp", MediaKind::Image, "image/bmp"},
    {"webp", MediaKind::Image, "image/webp"},
    {"mp3", MediaKind::Audio, "audio/mpeg"},
    {"wav", MediaKind::Audio, "audio/wav"},
    {"aac", MediaKind::Audio, "audio/aac"},
    {"m4a", MediaKind::Audio, "audio/mp4"},
    {"mp4", MediaKind::Video, "video/mp4"},
    {"zip", MediaKind::Archive, "application/zip"},
    {"pdf", MediaKind::Document, "application/pdf"},
    {"ole2", MediaKind::Document, "application/x-ole-storage"},
}};

// The files of a container format that its extension tells apart.
struct FileInfo {
    std::string_view formatId;
    std::string_view extension;
    MediaKind kind;
    std::string_view mediaType;
};

constexpr std::array<FileInfo, 32> kFiles = {{
    {"zip", "docx", MediaKind::Document, "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"zip", "docm", MediaKind::Document, "application/vnd.ms-word.document.macroEnabled.12"},
    {"zip", "dotx", MediaKind::Document, "application/vnd.openxmlformats-officedocument.wordprocessingml.template"},
    {"zip", "xlsx", MediaKind::Document, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"zip", "xlsm", MediaKind::Document, "application/vnd.ms-excel.sheet.macroEnabled.12"},
    {"zip", "pptx", MediaKind::Document,
     "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"zip", "pptm", MediaKind::Document, "application/vnd.ms-powerpoint.presentation.macroEnabled.12"},
    {"zip", "ppsx", MediaKind::Document, "application/vnd.openxmlformats-officedocument.presentationml.slideshow"},
    {"zip", "vsdx", MediaKind::Document, "application/vnd.ms-visio.drawing"},
    {"zip", "xps", MediaKind::Document, "application/vnd.ms-xpsdocument"},
    {"zip", "oxps", MediaKind::Document, "application/oxps"},
    {"zip", "odt", MediaKind::Document, "application/vnd.oasis.opendocument.text"},
    {"zip", "ods", MediaKind::Document, "application/vnd.oasis.opendocument.spreadsheet"},
    {"zip", "odp", MediaKind::Document, "application/vnd.oasis.opendocument.presentation"},
    {"zip", "odg", MediaKind::Document, "application/vnd.oasis.opendocument.graphics"},
    {"zip", "epub", MediaKind::Document, "application/epub+zip"},
    {"zip", "jar", MediaKind::Archive, "application/java-archive"},
    {"zip", "apk", MediaKind::Archive, "application/vnd.android.package-archive"},
    {"ole2", "doc", MediaKind::Document, "application/msword"},
    {"ole2", "dot", MediaKind::Document, "application/msword"},
    {"ole2", "xls", MediaKind::Document, "application/vnd.ms-excel"},
    {"ole2", "xlt", MediaKind::Document, "application/vnd.ms-excel"},
    {"ole2", "ppt", MediaKind::Document, "application/vnd.ms-powerpoint"},
    {"ole2", "pps", MediaKind::Document, "application/vnd.ms-powerpoint"},
    {"ole2", "pot", MediaKind::Document, "application/vnd.ms-powerpoint"},
    {"ole2", "msg", MediaKind::Document, "application/vnd.ms-outlook"},
    {"ole2", "vsd", MediaKind::Document, "application/vnd.visio"},
    {"ole2", "pub", MediaKind::Document, "application/vnd.ms-publisher"},
    {"ole2", "msi", MediaKind::Archive, "application/x-msi"},
    {"ole2", "msp", MediaKind::Archive, "application/x-ms-patch"},
    {"ole2", "mst", MediaKind::Archive, "application/x-ms-transform"},
    {"ole2", "db", MediaKind::Unknown, "application/x-ole-storage"},
}};

const FormatInfo* formatInfo(std::string_view formatId) noexcept {
    for (const FormatInfo& info : kFormats) {
        if (info.id == formatId) {
            return &info;
        }
    }
    return nullptr;
}

const FileInfo* fileInfo(std::string_view formatId, std::string_view extension) noexcept {
    for (const FileInfo& info : kFiles) {
        if (info.formatId == formatId && info.extension == extension) {
            return &info;
        }
    }
    return nullptr;
}

std::string twoDigits(unsigned value) {
    std::array<char, 8> text{};
    std::snprintf(text.data(), text.size(), "%02u", value % 100);
    return text.data();
}

}  // namespace

std::string_view toString(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::Unknown:
            return "unknown";
        case MediaKind::Image:
            return "image";
        case MediaKind::Audio:
            return "audio";
        case MediaKind::Video:
            return "video";
        case MediaKind::Archive:
            return "archive";
        case MediaKind::Document:
            return "document";
    }
    return "unknown";
}

MediaKind kindOfFormat(std::string_view formatId) noexcept {
    const FormatInfo* info = formatInfo(formatId);
    return info != nullptr ? info->kind : MediaKind::Unknown;
}

std::string_view mediaTypeOfFormat(std::string_view formatId) noexcept {
    const FormatInfo* info = formatInfo(formatId);
    return info != nullptr ? info->mediaType : std::string_view{};
}

MediaKind kindOfFile(std::string_view formatId, std::string_view extension) noexcept {
    const FileInfo* info = fileInfo(formatId, extension);
    return info != nullptr ? info->kind : kindOfFormat(formatId);
}

std::string_view mediaTypeOfFile(std::string_view formatId, std::string_view extension) noexcept {
    const FileInfo* info = fileInfo(formatId, extension);
    return info != nullptr ? info->mediaType : mediaTypeOfFormat(formatId);
}

std::string MediaDateTime::iso8601() const {
    std::array<char, 8> yearText{};
    std::snprintf(yearText.data(), yearText.size(), "%04d", year);
    std::string text = yearText.data();
    if (precision >= Precision::Month) {
        text += "-" + twoDigits(month);
    }
    if (precision >= Precision::Day) {
        text += "-" + twoDigits(day);
    }
    if (precision >= Precision::Hour) {
        text += "T" + twoDigits(hour);
        if (precision >= Precision::Minute) {
            text += ":" + twoDigits(minute);
        }
        if (precision >= Precision::Second) {
            text += ":" + twoDigits(second);
        }
        if (zone == Zone::Utc) {
            text += "Z";
        } else if (zone == Zone::Offset) {
            const int minutes = std::abs(static_cast<int>(offsetMinutes));
            text += (offsetMinutes < 0 ? "-" : "+") + twoDigits(static_cast<unsigned>(minutes / 60)) + ":" +
                    twoDigits(static_cast<unsigned>(minutes % 60));
        }
    }
    return text;
}

std::optional<std::chrono::sys_seconds> MediaDateTime::utc() const {
    using namespace std::chrono;
    if (zone == Zone::Unknown || !detail::validDateTime(*this)) {
        return std::nullopt;
    }
    const sys_days date{year_month_day{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}}};
    sys_seconds instant = date + hours{hour} + minutes{minute} + seconds{second};
    if (zone == Zone::Offset) {
        instant -= minutes{offsetMinutes};
    }
    return instant;
}

std::string_view toString(Orientation orientation) noexcept {
    switch (orientation) {
        case Orientation::Normal:
            return "normal";
        case Orientation::Mirror:
            return "mirror";
        case Orientation::Rotate180:
            return "rotate 180";
        case Orientation::MirrorRotate180:
            return "mirror, rotate 180";
        case Orientation::MirrorRotate270:
            return "mirror, rotate 270";
        case Orientation::Rotate90:
            return "rotate 90";
        case Orientation::MirrorRotate90:
            return "mirror, rotate 90";
        case Orientation::Rotate270:
            return "rotate 270";
    }
    return "normal";
}

std::uint16_t rotationOf(Orientation orientation) noexcept {
    switch (orientation) {
        case Orientation::Rotate180:
        case Orientation::MirrorRotate180:
            return 180;
        case Orientation::Rotate90:
        case Orientation::MirrorRotate90:
            return 90;
        case Orientation::Rotate270:
        case Orientation::MirrorRotate270:
            return 270;
        case Orientation::Normal:
        case Orientation::Mirror:
            break;
    }
    return 0;
}

bool mirrored(Orientation orientation) noexcept {
    return orientation == Orientation::Mirror || orientation == Orientation::MirrorRotate180 ||
           orientation == Orientation::MirrorRotate270 || orientation == Orientation::MirrorRotate90;
}

std::optional<Orientation> orientationForRotation(std::uint32_t degrees) noexcept {
    switch (degrees) {
        case 0:
            return Orientation::Normal;
        case 90:
            return Orientation::Rotate90;
        case 180:
            return Orientation::Rotate180;
        case 270:
            return Orientation::Rotate270;
        default:
            return std::nullopt;
    }
}

std::string_view toString(ColorModel model) noexcept {
    switch (model) {
        case ColorModel::Unknown:
            return "unknown";
        case ColorModel::Grayscale:
            return "grayscale";
        case ColorModel::Rgb:
            return "RGB";
        case ColorModel::Indexed:
            return "indexed";
        case ColorModel::YCbCr:
            return "YCbCr";
        case ColorModel::Cmyk:
            return "CMYK";
        case ColorModel::Ycck:
            return "YCCK";
    }
    return "unknown";
}

bool MediaTags::empty() const noexcept {
    return title.empty() && artist.empty() && album.empty() && genre.empty() && !date.has_value() &&
           !track.has_value() && !trackTotal.has_value();
}

std::string_view toString(PreviewKind kind) noexcept {
    switch (kind) {
        case PreviewKind::Content:
            return "content";
        case PreviewKind::Thumbnail:
            return "thumbnail";
        case PreviewKind::CoverArt:
            return "cover art";
    }
    return "content";
}

Status validate(const MetadataOptions& options) {
    if (options.maxScanBytes == 0 || options.maxTagBytes == 0 || options.maxTextLength == 0) {
        return makeError(ErrorCode::InvalidInput, "metadata limits must not be 0");
    }
    return formats::mp4::validate(options.mp4);
}

}  // namespace recovery::metadata
