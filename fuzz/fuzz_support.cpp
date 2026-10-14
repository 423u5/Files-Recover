#include "fuzz_support.hpp"

#include "formats/archive_formats.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "validation/deflate_decoder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recovery::fuzz {

FuzzSource::FuzzSource(std::span<const std::byte> data, std::uint32_t sectorSize)
    : data_(data.begin(), data.end()), sectorSize_(sectorSize) {
    const std::size_t remainder = data_.size() % sectorSize_;
    if (data_.empty() || remainder != 0) {
        data_.resize(data_.size() + (sectorSize_ - remainder), std::byte{0});
    }
}

Status FuzzSource::open() {
    open_ = true;
    return success();
}

void FuzzSource::close() noexcept {
    open_ = false;
}

bool FuzzSource::isOpen() const noexcept {
    return open_;
}

std::uint64_t FuzzSource::size() const noexcept {
    return open_ ? data_.size() : 0;
}

std::uint32_t FuzzSource::sectorSize() const noexcept {
    return open_ ? sectorSize_ : 0;
}

storage::SourceInfo FuzzSource::getInfo() const {
    storage::SourceInfo info;
    info.type = storage::SourceType::Synthetic;
    info.path = "memory://fuzz";
    info.sizeBytes = size();
    info.logicalSectorSize = sectorSize();
    info.physicalSectorSize = sectorSize();
    return info;
}

storage::ReadResult FuzzSource::readValidated(std::uint64_t offset, std::span<std::byte> buffer) {
    // The base class has checked that the range lies inside the source.
    std::memcpy(buffer.data(), data_.data() + offset, buffer.size());
    storage::ReadResult result;
    result.status = storage::ReadStatus::Success;
    result.requestedBytes = buffer.size();
    result.bytesRead = buffer.size();
    result.offset = offset;
    return result;
}

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "fuzz harness error: %s\n", what);
        std::abort();
    }
}

const carving::FormatRegistry& allFormats() {
    static const carving::FormatRegistry registry = [] {
        carving::FormatRegistry formats;
        const bool images = formats::registerImageFormats(formats).ok();
        const bool audio = formats::registerAudioFormats(formats).ok();
        const bool video = formats::registerVideoFormats(formats).ok();
        const bool archives = formats::registerArchiveFormats(formats, validation::deflateDecoder()).ok();
        require(images && audio && video && archives, "registering the formats");
        return formats;
    }();
    return registry;
}

const validation::MediaValidatorRegistry& allMedia() {
    static const validation::MediaValidatorRegistry registry = [] {
        validation::MediaValidatorRegistry media;
        require(validation::registerMediaValidators(media).ok(), "registering the media decoders");
        return media;
    }();
    return registry;
}

}  // namespace recovery::fuzz
