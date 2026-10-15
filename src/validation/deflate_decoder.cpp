#include "validation/deflate_decoder.hpp"

#include "content_bytes.hpp"
#include "inflate.hpp"
#include "recovery/crc32.hpp"

#include <algorithm>

namespace recovery::validation {

namespace {

using Outcome = formats::IDeflateDecoder::Outcome;

// Counts the output (and takes its CRC-32, or keeps it); stops before it grows past `limit` bytes.
class OutputSink final : public detail::InflateSink {
public:
    OutputSink(std::uint64_t limit, bool crc, std::vector<std::byte>* kept) noexcept
        : limit_(limit), crc_(crc), kept_(kept) {}

    bool write(std::span<const std::uint8_t> data) override {
        if (data.size() > limit_ - written_) {
            return false;
        }
        if (crc_) {
            value_ = crc32Update(value_, std::as_bytes(data));
        }
        if (kept_ != nullptr) {
            const std::span<const std::byte> bytes = std::as_bytes(data);
            kept_->insert(kept_->end(), bytes.begin(), bytes.end());
        }
        written_ += data.size();
        return true;
    }

    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }
    [[nodiscard]] std::uint32_t crc() const noexcept { return value_; }

private:
    std::uint64_t limit_;
    bool crc_;
    std::vector<std::byte>* kept_;
    std::uint64_t written_ = 0;
    std::uint32_t value_ = 0;
};

Result<Outcome> outcomeOf(const detail::InflateOutcome& inflated, const detail::ContentBytes& bytes,
                          const OutputSink& sink, std::uint64_t begin, std::uint64_t stop) {
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

class EngineDeflateDecoder final : public formats::IDeflateDecoder {
public:
    [[nodiscard]] Result<Outcome> decode(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end,
                                         std::uint64_t maxOutput) const override {
        const std::uint64_t stop = std::max(begin, std::min(end, content.size()));
        detail::ContentBytes bytes(content, begin, stop);
        OutputSink sink(maxOutput, true, nullptr);
        const detail::InflateOutcome inflated = detail::inflateRaw(bytes, sink);
        return outcomeOf(inflated, bytes, sink, begin, stop);
    }

    [[nodiscard]] Result<Outcome> decodeZlib(carving::IContentReader& content, std::uint64_t begin,
                                             std::uint64_t end, std::uint64_t maxOutput,
                                             std::vector<std::byte>* kept) const override {
        const std::uint64_t stop = std::max(begin, std::min(end, content.size()));
        detail::ContentBytes bytes(content, begin, stop);
        OutputSink sink(maxOutput, false, kept);
        const detail::InflateOutcome inflated = detail::inflateZlib(bytes, sink);
        return outcomeOf(inflated, bytes, sink, begin, stop);
    }
};

}  // namespace

std::shared_ptr<const formats::IDeflateDecoder> deflateDecoder() {
    static const std::shared_ptr<const formats::IDeflateDecoder> decoder =
        std::make_shared<const EngineDeflateDecoder>();
    return decoder;
}

}  // namespace recovery::validation
