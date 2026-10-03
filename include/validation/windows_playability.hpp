#pragma once

// Playability on Windows (P14): the platform's own decoders decode the whole
// file, as a viewer or a player would:
//
//   jpeg, png, gif, bmp, webp   the Windows Imaging Component decodes the
//                               pixels of every frame (in bands of rows);
//   mp3, wav, aac, m4a, mp4     Media Foundation's source reader decodes
//                               every sample of every audio and video stream.
//
// The decoders read the content through a read-only IStream over the content
// reader: nothing is written anywhere, and the stream is cut off from the
// reader when check() returns, whatever references the decoders keep.
//
// It is optional and off by default (validation.hpp): the decoders are not
// the engine's, they parse the untrusted content in this process, and what
// they accept depends on the codecs installed (WebP and HEVC come from
// Microsoft Store extensions). A format or a codec without a decoder is
// Unsupported, never Failed. Decoders conceal much damage: Passed means
// they decoded every frame or sample without an error, not that the
// pictures and sound are intact.
//
// Thread safety: check() may be called from any thread, also concurrently
// on different readers. It initializes COM on the calling thread for the
// call (a multithreaded apartment, unless the thread has one already) and
// starts Media Foundation (reference counted).
//
// The implementation and <windows.h> are confined to src/validation/windows/,
// in their own library, recovery_playability.

#include "validation/playability.hpp"

#include <chrono>

namespace recovery::validation {

struct WindowsPlayabilityOptions {
    // Media Foundation: how long the decoders may take to deliver the next
    // sample. A decoder that delivers nothing in that time (some stall on
    // streams they do not support, such as AVC High 4:2:2) makes the result
    // Unsupported: nothing is decided about the file.
    std::chrono::milliseconds sampleTimeout{10'000};
};

// InvalidInput when the timeout is not positive.
[[nodiscard]] Status validate(const WindowsPlayabilityOptions& options);

class WindowsPlayabilityChecker final : public IPlayabilityChecker {
public:
    explicit WindowsPlayabilityChecker(WindowsPlayabilityOptions options = {});

    // Fails with InvalidInput for invalid options, and with the reader's errors.
    [[nodiscard]] Result<LevelResult> check(carving::IContentReader& content, std::string_view formatId,
                                            std::string_view extension) override;

private:
    WindowsPlayabilityOptions options_;
};

}  // namespace recovery::validation
