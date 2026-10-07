#include "metadata/media_metadata.hpp"

#include "extraction.hpp"

#include "recovery/candidate_content.hpp"
#include "recovery/checked_math.hpp"

namespace recovery::metadata {

namespace {

// The content itself as the last preview, once its header was read.
void addContentPreview(detail::Extraction& x) {
    MediaMetadata& out = x.out;
    PreviewSource preview;
    preview.kind = PreviewKind::Content;
    preview.formatId = out.formatId;
    preview.mediaType = out.mediaType;
    preview.length = x.content.size();
    if (preview.length == 0) {
        return;
    }
    switch (out.kind) {
        case MediaKind::Image:
            if (!out.image.has_value()) {
                return;
            }
            preview.width = out.image->width;
            preview.height = out.image->height;
            preview.orientation = out.image->orientation;
            break;
        case MediaKind::Audio:
            if (!out.audio.has_value()) {
                return;
            }
            break;
        case MediaKind::Video:
            if (!out.video.has_value()) {
                return;
            }
            preview.width = out.video->width;
            preview.height = out.video->height;
            preview.orientation = orientationForRotation(out.video->rotation);
            break;
        case MediaKind::Unknown:
            return;
    }
    out.previews.push_back(std::move(preview));
}

}  // namespace

Result<MediaMetadata> extractMetadata(carving::IContentReader& content, std::string_view formatId,
                                      const MetadataOptions& options) {
    if (Status valid = validate(options); !valid.ok()) {
        return valid.error();
    }
    MediaMetadata out;
    out.formatId = std::string(formatId);
    out.kind = kindOfFormat(formatId);
    out.mediaType = std::string(mediaTypeOfFormat(formatId));
    if (out.kind == MediaKind::Unknown) {
        return out;
    }
    detail::Extraction x{content, options, out};
    Status read = success();
    if (formatId == "jpeg") {
        read = detail::extractJpeg(x);
    } else if (formatId == "png") {
        read = detail::extractPng(x);
    } else if (formatId == "gif") {
        read = detail::extractGif(x);
    } else if (formatId == "bmp") {
        read = detail::extractBmp(x);
    } else if (formatId == "webp") {
        read = detail::extractWebp(x);
    } else if (formatId == "mp3") {
        read = detail::extractMp3(x);
    } else if (formatId == "aac") {
        read = detail::extractAdts(x);
    } else if (formatId == "wav") {
        read = detail::extractWav(x);
    } else {
        read = detail::extractMp4(x);
    }
    if (!read.ok()) {
        return read.error();
    }
    addContentPreview(x);
    if (out.duration.has_value() && out.duration->count() > 0) {
        out.bitrate = detail::bitrateOf(content.size(), *out.duration);
    }
    return out;
}

Result<MediaMetadata> readMediaMetadata(storage::IStorageSource& source,
                                        const evaluation::EvaluatedCandidate& candidate,
                                        const MetadataOptions& options, const carving::SourceReadOptions& readOptions) {
    if (Status valid = validate(options); !valid.ok()) {
        return valid.error();
    }
    Result<std::unique_ptr<CandidateContentReader>> reader =
        CandidateContentReader::open(source, candidate.data, readOptions);
    if (!reader.ok()) {
        return reader.error();
    }
    return extractMetadata(**reader, candidate.formatId, options);
}

Result<std::vector<std::byte>> readPreview(carving::IContentReader& content, const PreviewSource& preview,
                                           std::uint64_t maxBytes) {
    if (!rangeWithin(preview.offset, preview.length, content.size())) {
        return makeError(ErrorCode::InvalidInput, "the preview does not lie inside the content");
    }
    if (preview.length > maxBytes) {
        return makeError(ErrorCode::InvalidInput, "the preview is larger than " + std::to_string(maxBytes) +
                                                      " bytes");
    }
    return detail::readBlock(content, preview.offset, preview.length);
}

Result<std::unique_ptr<carving::IContentReader>> openPreview(carving::IContentReader& content,
                                                             const PreviewSource& preview) {
    if (!rangeWithin(preview.offset, preview.length, content.size())) {
        return makeError(ErrorCode::InvalidInput, "the preview does not lie inside the content");
    }
    return std::unique_ptr<carving::IContentReader>(
        std::make_unique<detail::WindowReader>(content, preview.offset, preview.length));
}

}  // namespace recovery::metadata
