#include "validation/deflate_decoder.hpp"

#include "content_bytes.hpp"
#include "inflate.hpp"
#include "recovery/crc32.hpp"

#include <algorithm>

namespace recovery::validation {

namespace {

using Outcome = formats::IDeflateDecoder::Outcome;

// Takes the CRC-32 and the length of the output; stops before it grows past `limit` bytes.
class CrcSink final : public detail::InflateSink {
public:
    explicit CrcSink(std::uint64_t limit) noexcept : limit_(limit) {}

    bool write(std::span<const std::uint8_t> data) override {
        if (data.size() > limit_ - written_) {
            return false;
        }
        crc_ = crc32Update(crc_, std::as_bytes(data));
        written_ += data.size();
        return true;
    }

    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }
    [[nodiscard]] std::uint32_t crc() const noexcept { return crc_; }

private:
    std::uint64_t limit_;
    std::uint64_t written_ = 0;
    std::uint32_t crc_ = 0;
};

class EngineDeflateDecoder final : public formats::IDeflateDecoder {
public:
    [[nodiscard]] Result<Outcome> decode(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end,
                                         std::uint64_t maxOutput) const override {
        const std::uint64_t stop = std::max(begin, std::min(end, content.size()));
        detail::ContentBytes bytes(content, begin, stop);
        CrcSink sink(maxOutput);
        const detail::InflateOutcome inflated = detail::inflateRaw(bytes, sink);
        Outcome out;
        out.output = sink.written();
        out.crc32 = sink.crc();
        switch (inflated.kind) {
        case detail::InflateOutcome::Kind::Done:
            out.kind = Outcome::Kind::Done;
            out.length = stop - begin - inflated.trailing;
            break;
        case detail::InflateOutcome::Kind::Truncated:
            out.kind = Outcome::Kind::Truncated;
            out.offset = inflated.offset;
            break;
        case detail::InflateOutcome::Kind::Invalid:
            out.kind = Outcome::Kind::Invalid;
            out.offset = inflated.offset;
            out.detail = inflated.detail;
            break;
        case detail::InflateOutcome::Kind::Stopped:
            out.kind = Outcome::Kind::TooLarge;
            out.offset = inflated.offset;
            break;
        case detail::InflateOutcome::Kind::ReadError:
            if (bytes.error().has_value()) {
                return *bytes.error();
            }
            return makeError(ErrorCode::InternalError, "DEFLATE decoding stopped on a read error it did not keep");
        }
        return out;
    }
};

}  // namespace

std::shared_ptr<const formats::IDeflateDecoder> deflateDecoder() {
    static const std::shared_ptr<const formats::IDeflateDecoder> decoder =
        std::make_shared<const EngineDeflateDecoder>();
    return decoder;
}

}  // namespace recovery::validation
