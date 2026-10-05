// Playability on Windows: the Windows Imaging Component for images, Media
// Foundation's source reader for audio and video (windows_playability.hpp).
// <windows.h> and COM stay in this folder.

#include "validation/windows_playability.hpp"

#include "formats/mp4_parser.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/unicode.hpp"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::validation {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxRead = carving::IContentReader::kMaxReadLength;
// Pixels copied out of WIC at a time (32 bits each).
constexpr std::uint64_t kBandBytes = 4ULL * 1024 * 1024;

std::string hresult(HRESULT value) {
    std::array<char, 16> text{};
    static constexpr char kDigits[] = "0123456789ABCDEF";
    const auto bits = static_cast<std::uint32_t>(value);
    text[0] = '0';
    text[1] = 'x';
    for (int i = 0; i < 8; ++i) {
        text[2 + static_cast<std::size_t>(i)] = kDigits[(bits >> (28 - 4 * i)) & 0xFU];
    }
    return std::string(text.data(), 10);
}

std::string narrow(const wchar_t* text, std::size_t length) {
    std::u16string units(length, u'\0');
    for (std::size_t i = 0; i < length; ++i) {
        units[i] = static_cast<char16_t>(text[i]);
    }
    return utf16ToUtf8(units);
}

// A read-only IStream over a content reader. The decoders may keep it after
// check() returns: detach() cuts it off from the reader, and later reads fail.
class ContentStream final : public IStream {
public:
    explicit ContentStream(carving::IContentReader& content) : content_(&content), size_(content.size()) {}

    ContentStream(const ContentStream&) = delete;
    ContentStream& operator=(const ContentStream&) = delete;
    ContentStream(ContentStream&&) = delete;
    ContentStream& operator=(ContentStream&&) = delete;

    void detach() {
        const std::lock_guard lock(mutex_);
        content_ = nullptr;
    }

    // The reader's first error (Cancelled, a source failure), if a read failed.
    [[nodiscard]] std::optional<Error> error() {
        const std::lock_guard lock(mutex_);
        return error_;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream)) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (left == 0) {
            delete this;
        }
        return left;
    }

    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(_Out_writes_bytes_to_(cb, *pcbRead) void* pv, _In_ ULONG cb,
                                   _Out_opt_ ULONG* pcbRead) override {
        if (pcbRead != nullptr) {
            *pcbRead = 0;
        }
        if (pv == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        const std::lock_guard lock(mutex_);
        if (content_ == nullptr) {
            return E_UNEXPECTED;
        }
        ULONG done = 0;
        while (done < cb && position_ < size_) {
            const std::uint64_t wanted = std::min<std::uint64_t>(cb - done, size_ - position_);
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(wanted, kMaxRead));
            Result<std::span<const std::byte>> bytes = content_->read(position_, chunk);
            if (!bytes.ok()) {
                if (!error_.has_value()) {
                    error_ = bytes.error();
                }
                return STG_E_READFAULT;
            }
            std::memcpy(static_cast<std::byte*>(pv) + done, bytes->data(), chunk);
            done += static_cast<ULONG>(chunk);
            position_ += chunk;
            if (pcbRead != nullptr) {
                *pcbRead = done;
            }
        }
        return done == cb ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(_In_reads_bytes_(cb) const void*, _In_ ULONG cb,
                                    _Out_opt_ ULONG* pcbWritten) override {
        static_cast<void>(cb);
        if (pcbWritten != nullptr) {
            *pcbWritten = 0;
        }
        return STG_E_ACCESSDENIED;
    }

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                                   _Out_opt_ ULARGE_INTEGER* plibNewPosition) override {
        const std::lock_guard lock(mutex_);
        std::int64_t base = 0;
        switch (dwOrigin) {
        case STREAM_SEEK_SET:
            break;
        case STREAM_SEEK_CUR:
            base = static_cast<std::int64_t>(position_);
            break;
        case STREAM_SEEK_END:
            base = static_cast<std::int64_t>(size_);
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t move = dlibMove.QuadPart;
        if ((move > 0 && base > std::numeric_limits<std::int64_t>::max() - move) || base + move < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<std::uint64_t>(base + move);
        if (plibNewPosition != nullptr) {
            plibNewPosition->QuadPart = position_;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(_In_ IStream*, ULARGE_INTEGER, _Out_opt_ ULARGE_INTEGER* pcbRead,
                                     _Out_opt_ ULARGE_INTEGER* pcbWritten) override {
        if (pcbRead != nullptr) {
            pcbRead->QuadPart = 0;
        }
        if (pcbWritten != nullptr) {
            pcbWritten->QuadPart = 0;
        }
        return E_NOTIMPL;
    }
    // Nothing is ever written, so there is nothing to commit or revert.
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(__RPC__out STATSTG* pstatstg, DWORD grfStatFlag) override {
        if (pstatstg == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        *pstatstg = STATSTG{};
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = size_;
        pstatstg->grfMode = STGM_READ;
        if ((grfStatFlag & STATFLAG_NONAME) == 0) {
            static constexpr wchar_t kName[] = L"content";
            auto* name = static_cast<wchar_t*>(CoTaskMemAlloc(sizeof(kName)));
            if (name == nullptr) {
                return E_OUTOFMEMORY;
            }
            std::memcpy(name, kName, sizeof(kName));
            pstatstg->pwcsName = name;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(__RPC__deref_out_opt IStream** ppstm) override {
        if (ppstm != nullptr) {
            *ppstm = nullptr;
        }
        return E_NOTIMPL;
    }

private:
    // Virtual: COM interfaces have no virtual destructor, and Release() deletes.
    virtual ~ContentStream() = default;

    std::mutex mutex_;
    carving::IContentReader* content_;
    const std::uint64_t size_;
    std::uint64_t position_ = 0;
    std::optional<Error> error_;
    LONG references_ = 1;
};

// Owns the one reference ContentStream starts with, and detaches it at the end.
class StreamHandle {
public:
    explicit StreamHandle(carving::IContentReader& content) : stream_(new ContentStream(content)) {}
    StreamHandle(const StreamHandle&) = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;
    StreamHandle(StreamHandle&&) = delete;
    StreamHandle& operator=(StreamHandle&&) = delete;
    ~StreamHandle() {
        stream_->detach();
        stream_->Release();
    }

    [[nodiscard]] ContentStream* get() const noexcept { return stream_; }

private:
    ContentStream* stream_;
};

// COM for the calling thread during one check: a multithreaded apartment,
// or the apartment the thread has already.
class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;
    ~ComScope() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool usable() const noexcept { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

class MediaFoundationScope {
public:
    MediaFoundationScope() : result_(MFStartup(MF_VERSION, MFSTARTUP_LITE)) {}
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;
    MediaFoundationScope(MediaFoundationScope&&) = delete;
    MediaFoundationScope& operator=(MediaFoundationScope&&) = delete;
    ~MediaFoundationScope() {
        if (SUCCEEDED(result_)) {
            MFShutdown();
        }
    }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

// The source reader's asynchronous callback: each ReadSample request is
// answered here, on a Media Foundation thread, and wait() hands the answer
// to the checking thread, or nothing when none came in time (a decoder that
// stalls must not hold the check forever).
class ReadCallback final : public IMFSourceReaderCallback {
public:
    struct Delivery {
        HRESULT status = S_OK;
        DWORD stream = 0;
        DWORD flags = 0;
        bool sample = false;
    };

    ReadCallback() = default;
    ReadCallback(const ReadCallback&) = delete;
    ReadCallback& operator=(const ReadCallback&) = delete;
    ReadCallback(ReadCallback&&) = delete;
    ReadCallback& operator=(ReadCallback&&) = delete;

    [[nodiscard]] std::optional<Delivery> wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!delivered_.wait_for(lock, timeout, [this] { return pending_.has_value(); })) {
            return std::nullopt;
        }
        return std::exchange(pending_, std::nullopt);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
            *object = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (left == 0) {
            delete this;
        }
        return left;
    }

    // IMFSourceReaderCallback
    HRESULT STDMETHODCALLTYPE OnReadSample(_In_ HRESULT hrStatus, _In_ DWORD dwStreamIndex, _In_ DWORD dwStreamFlags,
                                           _In_ LONGLONG, _In_opt_ IMFSample* pSample) override {
        {
            const std::lock_guard lock(mutex_);
            pending_ = Delivery{hrStatus, dwStreamIndex, dwStreamFlags, pSample != nullptr};
        }
        delivered_.notify_one();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(_In_ DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEvent(_In_ DWORD, _In_ IMFMediaEvent*) override { return S_OK; }

private:
    virtual ~ReadCallback() = default;

    std::mutex mutex_;
    std::condition_variable delivered_;
    std::optional<Delivery> pending_;
    LONG references_ = 1;
};

LevelResult level(LevelStatus status, std::string checker, std::string detail) {
    LevelResult result;
    result.status = status;
    result.checker = std::move(checker);
    result.detail = std::move(detail);
    return result;
}

// What a decoder's failure is: the reader's error when a read failed
// (returned as an error, never as a result), a Failed result otherwise.
Result<LevelResult> failure(ContentStream& stream, std::string checker, std::string detail) {
    if (std::optional<Error> error = stream.error()) {
        return *error;
    }
    return level(LevelStatus::Failed, std::move(checker), std::move(detail));
}

// ---------------------------------------------------------------------------
// Known gaps: content the Windows decoders cannot decode although it is
// valid, and on which they fail (or stall) rather than refuse. It is
// Unsupported before any decoder runs, so a failure means something.
// ---------------------------------------------------------------------------

// JPEG: WIC decodes Huffman-coded baseline, extended and progressive frames
// of 8-bit samples. The first frame header before the first scan decides.
Result<std::optional<std::string>> jpegGap(carving::IContentReader& content) {
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(content.size(), 64 * 1024));
    Result<std::span<const std::byte>> read = content.read(0, length);
    if (!read.ok()) {
        return read.error();
    }
    const std::span<const std::byte> bytes = *read;
    for (std::size_t position = 2; position + 4 <= bytes.size();) {
        if (loadU8(bytes, position) != 0xFF) {
            return std::optional<std::string>{};
        }
        const std::uint8_t marker = loadU8(bytes, position + 1);
        if (marker == 0xFF) {
            ++position;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            position += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            return std::optional<std::string>{};
        }
        const std::size_t segment = loadBe16(bytes, position + 2);
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame) {
            const std::uint8_t precision = position + 4 < bytes.size() ? loadU8(bytes, position + 4) : 8;
            if (marker > 0xC2) {
                static constexpr std::array<const char*, 16> kKinds = {
                    "", "", "", "lossless", "", "differential sequential", "differential progressive",
                    "differential lossless", "", "arithmetic-coded sequential", "arithmetic-coded progressive",
                    "arithmetic-coded lossless", "", "differential arithmetic-coded sequential",
                    "differential arithmetic-coded progressive", "differential arithmetic-coded lossless"};
                return std::optional<std::string>{std::string(kKinds[marker & 0x0FU]) + " JPEG (SOF" +
                                                  std::to_string(marker & 0x0FU) +
                                                  "); WIC decodes Huffman-coded baseline, extended and "
                                                  "progressive JPEG"};
            }
            if (precision != 8) {
                return std::optional<std::string>{"a JPEG of " + std::to_string(precision) +
                                                  "-bit samples; WIC decodes 8-bit samples"};
            }
            return std::optional<std::string>{};
        }
        position += 2 + segment;
    }
    return std::optional<std::string>{};
}

// BMP: no JPEG or PNG inside a bitmap, no OS/2 Huffman 1D or RLE24.
Result<std::optional<std::string>> bmpGap(carving::IContentReader& content) {
    if (content.size() < 34) {
        return std::optional<std::string>{};
    }
    Result<std::span<const std::byte>> read = content.read(0, 34);
    if (!read.ok()) {
        return read.error();
    }
    const std::uint32_t header = loadLe32(*read, 14);
    if (header < 40) {
        return std::optional<std::string>{};
    }
    const std::uint32_t bits = loadLe16(*read, 28);
    const std::uint32_t compression = loadLe32(*read, 30);
    if (compression == 4 && bits == 0) {
        return std::optional<std::string>{"a bitmap of JPEG data; WIC does not decode it"};
    }
    if (compression == 5 && bits == 0) {
        return std::optional<std::string>{"a bitmap of PNG data; WIC does not decode it"};
    }
    if (header == 64 && compression == 3 && bits == 1) {
        return std::optional<std::string>{"an OS/2 bitmap of Huffman 1D data; WIC does not decode it"};
    }
    if (header == 64 && compression == 4 && bits == 24) {
        return std::optional<std::string>{"an OS/2 bitmap of RLE24 data; WIC does not decode it"};
    }
    return std::optional<std::string>{};
}

// MP4: Windows' MPEG-4 source reads no compact sample sizes (stz2).
Result<std::optional<std::string>> mp4Gap(carving::IContentReader& content) {
    Result<formats::mp4::Mp4File> parsed = formats::mp4::parseFile(content);
    if (!parsed.ok()) {
        return parsed.error();
    }
    if (parsed->movie.has_value()) {
        for (const formats::mp4::Track& track : parsed->movie->tracks) {
            if (track.samples.compactSampleSizes) {
                return std::optional<std::string>{
                    "compact sample sizes (stz2); Windows' MPEG-4 source does not read them"};
            }
        }
    }
    return std::optional<std::string>{};
}

// ---------------------------------------------------------------------------
// Images: the Windows Imaging Component
// ---------------------------------------------------------------------------

constexpr const char* kImaging = "Windows Imaging Component";

std::string decoderName(IWICBitmapDecoder& decoder) {
    ComPtr<IWICBitmapDecoderInfo> info;
    if (FAILED(decoder.GetDecoderInfo(&info)) || info == nullptr) {
        return "decoder";
    }
    UINT length = 0;
    if (FAILED(info->GetFriendlyName(0, nullptr, &length)) || length == 0) {
        return "decoder";
    }
    std::vector<wchar_t> name(length);
    if (FAILED(info->GetFriendlyName(length, name.data(), &length)) || length == 0) {
        return "decoder";
    }
    return narrow(name.data(), std::min<std::size_t>(length - 1, name.size()));
}

Result<LevelResult> checkImage(carving::IContentReader& content) {
    StreamHandle handle(content);
    ContentStream& stream = *handle.get();
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return level(LevelStatus::Unsupported, kImaging, "the Windows Imaging Component is not available (" +
                                                             hresult(result) + ")");
    }
    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromStream(&stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (result == WINCODEC_ERR_COMPONENTNOTFOUND) {
        if (std::optional<Error> error = stream.error()) {
            return *error;
        }
        return level(LevelStatus::Unsupported, kImaging, "no installed decoder takes the file");
    }
    // A decoder registered for the file that cannot be created (a Store codec such as WebP on Windows Server, or in
    // a session where its package cannot activate) is a missing codec, not damage in the file.
    if (result == WINCODEC_ERR_COMPONENTINITIALIZEFAILURE) {
        if (std::optional<Error> error = stream.error()) {
            return *error;
        }
        return level(LevelStatus::Unsupported, kImaging,
                     "the decoder registered for the file cannot be loaded (" + hresult(result) + ")");
    }
    if (FAILED(result)) {
        return failure(stream, kImaging, "no decoder opens the file (" + hresult(result) + ")");
    }
    const std::string checker = std::string(kImaging) + ", " + decoderName(*decoder.Get());
    UINT frames = 0;
    result = decoder->GetFrameCount(&frames);
    if (FAILED(result) || frames == 0) {
        return failure(stream, checker,
                       FAILED(result) ? "the frames cannot be counted (" + hresult(result) + ")" : "no frames");
    }
    std::vector<BYTE> band;
    std::uint64_t pixels = 0;
    UINT firstWidth = 0;
    UINT firstHeight = 0;
    for (UINT index = 0; index < frames; ++index) {
        const std::string where = "frame " + std::to_string(index + 1) + " of " + std::to_string(frames) + ": ";
        ComPtr<IWICBitmapFrameDecode> frame;
        result = decoder->GetFrame(index, &frame);
        if (FAILED(result)) {
            return failure(stream, checker, where + "the decoder does not open it (" + hresult(result) + ")");
        }
        UINT width = 0;
        UINT height = 0;
        result = frame->GetSize(&width, &height);
        if (FAILED(result) || width == 0 || height == 0) {
            return failure(stream, checker, where + "no size (" + hresult(result) + ")");
        }
        if (index == 0) {
            firstWidth = width;
            firstHeight = height;
        }
        const std::uint64_t stride = std::uint64_t{width} * 4;
        if (width > static_cast<UINT>(std::numeric_limits<INT>::max()) ||
            height > static_cast<UINT>(std::numeric_limits<INT>::max()) || stride > kBandBytes * 16) {
            return level(LevelStatus::Unsupported, checker,
                         where + "a picture of " + std::to_string(width) + "x" + std::to_string(height) +
                             " pixels is not decoded");
        }
        ComPtr<IWICFormatConverter> converter;
        result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) {
            result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                           0.0, WICBitmapPaletteTypeCustom);
        }
        if (FAILED(result)) {
            return failure(stream, checker, where + "its pixels cannot be converted (" + hresult(result) + ")");
        }
        const UINT rows = static_cast<UINT>(std::max<std::uint64_t>(1, kBandBytes / stride));
        band.resize(static_cast<std::size_t>(stride * std::min<UINT>(rows, height)));
        for (UINT y = 0; y < height; y += rows) {
            const UINT count = std::min(rows, height - y);
            const WICRect rect{0, static_cast<INT>(y), static_cast<INT>(width), static_cast<INT>(count)};
            result = converter->CopyPixels(&rect, static_cast<UINT>(stride), static_cast<UINT>(stride * count),
                                           band.data());
            if (FAILED(result)) {
                return failure(stream, checker,
                               where + "the decoder fails in rows " + std::to_string(y) + " to " +
                                   std::to_string(y + count - 1) + " (" + hresult(result) + ")");
            }
        }
        pixels += std::uint64_t{width} * height;
    }
    if (std::optional<Error> error = stream.error()) {
        return *error;
    }
    return level(LevelStatus::Passed, checker,
                 std::to_string(frames) + (frames == 1 ? " frame" : " frames") + " decoded (" +
                     std::to_string(firstWidth) + "x" + std::to_string(firstHeight) +
                     (frames > 1 ? " first" : "") + "), " + std::to_string(pixels) + " pixels");
}

// ---------------------------------------------------------------------------
// Audio and video: Media Foundation
// ---------------------------------------------------------------------------

constexpr const char* kMedia = "Media Foundation";

// The media source and its reader, shut down in that order: a source reader
// released while a request is pending in a stalled decoder waits for it
// forever; once its source is shut down, it does not.
struct Pipeline {
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    // A request is stuck in a decoder: the reader is left unreleased.
    bool stalled = false;

    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;
    ~Pipeline() {
        if (source != nullptr) {
            source->Shutdown();
        }
        if (stalled) {
            static_cast<void>(reader.Detach());
        }
        reader.Reset();
        source.Reset();
    }
};

struct StreamState {
    bool selected = false;
    bool ended = false;
    bool video = false;
    std::uint64_t samples = 0;
};

// Asks the reader for decoded output of stream `index`: PCM for audio, a
// YUV or RGB layout for video. False when no decoder gives one.
bool decodeTo(IMFSourceReader& reader, DWORD index, const GUID& major) {
    static constexpr std::array<const GUID*, 5> kVideo = {&MFVideoFormat_NV12, &MFVideoFormat_P010, &MFVideoFormat_YUY2,
                                                          &MFVideoFormat_I420, &MFVideoFormat_RGB32};
    static constexpr std::array<const GUID*, 2> kAudio = {&MFAudioFormat_PCM, &MFAudioFormat_Float};
    const bool video = major == MFMediaType_Video;
    const std::span<const GUID* const> subtypes = video ? std::span<const GUID* const>(kVideo)
                                                        : std::span<const GUID* const>(kAudio);
    for (const GUID* subtype : subtypes) {
        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type)) || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, major)) ||
            FAILED(type->SetGUID(MF_MT_SUBTYPE, *subtype))) {
            return false;
        }
        if (SUCCEEDED(reader.SetCurrentMediaType(index, nullptr, type.Get()))) {
            return true;
        }
    }
    return false;
}

std::wstring originName(std::string_view extension) {
    std::wstring name = L"content.";
    for (const char c : extension) {
        name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return name;
}

Result<LevelResult> checkMedia(carving::IContentReader& content, std::string_view extension,
                               std::chrono::milliseconds sampleTimeout) {
    const MediaFoundationScope platform;
    if (FAILED(platform.result())) {
        return level(LevelStatus::Unsupported, kMedia,
                     "Media Foundation is not available (" + hresult(platform.result()) + ")");
    }
    StreamHandle handle(content);
    ContentStream& stream = *handle.get();
    ComPtr<IMFByteStream> bytes;
    HRESULT result = MFCreateMFByteStreamOnStream(&stream, &bytes);
    if (FAILED(result)) {
        return failure(stream, kMedia, "no byte stream over the content (" + hresult(result) + ")");
    }
    // The extension picks the media source; one that does not match the
    // content is not taken as an error.
    ComPtr<IMFSourceResolver> resolver;
    result = MFCreateSourceResolver(&resolver);
    if (FAILED(result)) {
        return level(LevelStatus::Unsupported, kMedia, "no source resolver (" + hresult(result) + ")");
    }
    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    ComPtr<IUnknown> object;
    const std::wstring name = originName(extension);
    constexpr DWORD kResolution = MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ |
                                  MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE;
    result =
        resolver->CreateObjectFromByteStream(bytes.Get(), name.c_str(), kResolution, nullptr, &objectType, &object);
    if (result == MF_E_UNSUPPORTED_BYTESTREAM_TYPE) {
        if (std::optional<Error> error = stream.error()) {
            return *error;
        }
        return level(LevelStatus::Unsupported, kMedia, "no installed media source takes the file");
    }
    if (FAILED(result)) {
        return failure(stream, kMedia, "no media source opens the file (" + hresult(result) + ")");
    }
    Pipeline pipeline;
    result = object.As(&pipeline.source);
    if (FAILED(result)) {
        return failure(stream, kMedia, "the resolver gives no media source (" + hresult(result) + ")");
    }
    IMFMediaSource* const source = pipeline.source.Get();

    ComPtr<ReadCallback> callback;
    callback.Attach(new ReadCallback());
    ComPtr<IMFAttributes> attributes;
    result = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) {
        result = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
    }
    if (FAILED(result)) {
        return level(LevelStatus::Unsupported, kMedia, "no reader attributes (" + hresult(result) + ")");
    }
    result = MFCreateSourceReaderFromMediaSource(source, attributes.Get(), &pipeline.reader);
    if (FAILED(result)) {
        return failure(stream, kMedia, "no source reader for the file (" + hresult(result) + ")");
    }
    IMFSourceReader* const reader = pipeline.reader.Get();
    std::vector<StreamState> streams;
    std::string unsupported;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> native;
        result = reader->GetNativeMediaType(index, 0, &native);
        if (result == MF_E_INVALIDSTREAMNUMBER) {
            break;
        }
        if (FAILED(result)) {
            return failure(stream, kMedia, "stream " + std::to_string(index + 1) + " has no media type (" +
                                               hresult(result) + ")");
        }
        StreamState state;
        GUID major = GUID_NULL;
        GUID subtype = GUID_NULL;
        native->GetGUID(MF_MT_MAJOR_TYPE, &major);
        native->GetGUID(MF_MT_SUBTYPE, &subtype);
        const std::string label = std::string(major == MFMediaType_Video ? "video" : "audio") + " stream " +
                                  std::to_string(index + 1);
        UINT32 profile = 0;
        if (major != MFMediaType_Audio && major != MFMediaType_Video) {
            reader->SetStreamSelection(index, FALSE);
        } else if (subtype == MFVideoFormat_H264 && SUCCEEDED(native->GetUINT32(MF_MT_MPEG2_PROFILE, &profile)) &&
                   profile != 66 && profile != 77 && profile != 100) {
            // Windows' H.264 decoder takes Baseline, Main and High; given
            // another profile (High 10, High 4:2:2, High 4:4:4) it accepts
            // the stream and then delivers nothing.
            reader->SetStreamSelection(index, FALSE);
            unsupported += (unsupported.empty() ? "" : "; ") + label + ": AVC profile " + std::to_string(profile) +
                           " (Windows' H.264 decoder takes Baseline, Main and High)";
        } else {
            state.video = major == MFMediaType_Video;
            reader->SetStreamSelection(index, TRUE);
            state.selected = decodeTo(*reader, index, major);
            if (!state.selected) {
                reader->SetStreamSelection(index, FALSE);
                unsupported += (unsupported.empty() ? "" : "; ") + label + ": no installed decoder";
            }
        }
        streams.push_back(state);
    }
    const auto selected = static_cast<std::size_t>(
        std::count_if(streams.begin(), streams.end(), [](const StreamState& state) { return state.selected; }));
    if (selected == 0) {
        if (std::optional<Error> error = stream.error()) {
            return *error;
        }
        return level(LevelStatus::Unsupported, kMedia,
                     unsupported.empty() ? "the file has no audio or video stream" : "not decoded: " + unsupported);
    }
    // Every sample of every selected stream, to the end. A source that never
    // ends is cut off after more samples than the file has bytes.
    const std::uint64_t limit = content.size() + 1024;
    std::size_t ended = 0;
    for (std::uint64_t reads = 0; ended < selected; ++reads) {
        if (reads > limit) {
            return level(LevelStatus::Unsupported, kMedia, "the source does not reach the end of its streams");
        }
        std::uint64_t decoded = 0;
        for (const StreamState& state : streams) {
            decoded += state.samples;
        }
        result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, nullptr, nullptr, nullptr,
                                    nullptr);
        if (FAILED(result)) {
            return failure(stream, kMedia,
                           "decoding fails after " + std::to_string(decoded) + " samples (" + hresult(result) + ")");
        }
        const std::optional<ReadCallback::Delivery> delivery = callback->wait(sampleTimeout);
        if (!delivery.has_value()) {
            pipeline.stalled = true;
            if (std::optional<Error> error = stream.error()) {
                return *error;
            }
            return level(LevelStatus::Unsupported, kMedia,
                         "the decoders delivered nothing for " + std::to_string(sampleTimeout.count() / 1000) +
                             " seconds after " + std::to_string(decoded) + " samples (stalled; nothing decided)");
        }
        if (FAILED(delivery->status) || (delivery->flags & MF_SOURCE_READERF_ERROR) != 0) {
            return failure(stream, kMedia,
                           "decoding fails after " + std::to_string(decoded) + " samples (" +
                               hresult(delivery->status) + ")");
        }
        if (delivery->stream < streams.size()) {
            StreamState& state = streams[delivery->stream];
            if (delivery->sample) {
                ++state.samples;
            }
            if ((delivery->flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 && state.selected && !state.ended) {
                state.ended = true;
                ++ended;
            }
        }
    }
    std::uint64_t frames = 0;
    std::uint64_t audio = 0;
    for (const StreamState& state : streams) {
        (state.video ? frames : audio) += state.samples;
    }
    std::string detail = "decoded to the end: " + std::to_string(frames) + " video frames and " +
                         std::to_string(audio) + " audio buffers";
    if (!unsupported.empty()) {
        detail += "; not decoded: " + unsupported;
    }
    if (std::optional<Error> error = stream.error()) {
        return *error;
    }
    return level(LevelStatus::Passed, kMedia, std::move(detail));
}

}  // namespace

Status validate(const WindowsPlayabilityOptions& options) {
    if (options.sampleTimeout.count() <= 0) {
        return makeError(ErrorCode::InvalidInput, "Windows playability: the sample timeout must be positive");
    }
    return success();
}

WindowsPlayabilityChecker::WindowsPlayabilityChecker(WindowsPlayabilityOptions options) : options_(options) {}

Result<LevelResult> WindowsPlayabilityChecker::check(carving::IContentReader& content, std::string_view formatId,
                                                     std::string_view extension) {
    if (Status valid = validate(options_); !valid.ok()) {
        return valid.error();
    }
    const bool image = formatId == "jpeg" || formatId == "png" || formatId == "gif" || formatId == "bmp" ||
                       formatId == "webp";
    const bool media = formatId == "mp3" || formatId == "wav" || formatId == "aac" || formatId == "m4a" ||
                       formatId == "mp4";
    if (!image && !media) {
        return level(LevelStatus::Unsupported, "Windows decoders",
                     "no Windows decoder is used for " + std::string(formatId) + " files");
    }
    const ComScope com;
    if (!com.usable()) {
        return level(LevelStatus::Unsupported, image ? kImaging : kMedia,
                     "COM cannot be initialized on this thread (" + hresult(com.result()) + ")");
    }
    Result<std::optional<std::string>> gap = formatId == "jpeg"                       ? jpegGap(content)
                                             : formatId == "bmp"                      ? bmpGap(content)
                                             : formatId == "mp4" || formatId == "m4a" ? mp4Gap(content)
                                                                                      : std::optional<std::string>{};
    if (!gap.ok()) {
        return gap.error();
    }
    if (gap->has_value()) {
        return level(LevelStatus::Unsupported, image ? kImaging : kMedia, "not decoded: " + **gap);
    }
    return image ? checkImage(content) : checkMedia(content, extension, options_.sampleTimeout);
}

}  // namespace recovery::validation
