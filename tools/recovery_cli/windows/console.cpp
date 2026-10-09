// Windows console glue of the recovery CLI: the only file of the CLI that
// includes <windows.h>.
//
// Windows assumptions:
//  * Console control handlers run on a thread Windows creates for each
//    event. Returning TRUE for CTRL_C_EVENT or CTRL_BREAK_EVENT keeps the
//    process alive; FALSE passes the event to the next handler, the default
//    one, which ends the process (exit code STATUS_CONTROL_C_EXIT).
//  * For CTRL_CLOSE_EVENT (the console window is closed) Windows ends the
//    process when the handler returns, and after about five seconds anyway.
//  * WriteConsoleW shows any UTF-16 text on a console, whatever its output
//    code page; WriteFile on a console handle would read bytes in that code
//    page. GetConsoleMode succeeds only on console handles.

#include "console.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <streambuf>

namespace recovery::cli::console {

namespace {

// Set while a ConsoleSession lives.
std::atomic<Interrupt*> currentInterrupt{nullptr};

BOOL WINAPI onConsoleEvent(DWORD event) {
    Interrupt* const interrupt = currentInterrupt.load();
    if (interrupt == nullptr) {
        return FALSE;
    }
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        // The first stops the command cleanly; after it, the default handler
        // ends the process.
        return interrupt->request() ? TRUE : FALSE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        (void)interrupt->request();
        (void)interrupt->waitFinished(std::chrono::seconds(4));
        return TRUE;
    default:
        return FALSE;
    }
}

bool isConsole(HANDLE handle) noexcept {
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) != 0;
}

// The bytes of `data` up to an incomplete UTF-8 sequence at its end.
std::size_t completeUtf8(const char* data, std::size_t size) noexcept {
    for (std::size_t back = 1; back <= std::min<std::size_t>(size, 4); ++back) {
        const auto byte = static_cast<unsigned char>(data[size - back]);
        if (byte < 0x80) {
            return size;
        }
        if (byte >= 0xC0) {
            const std::size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;
            return length > back ? size - back : size;
        }
    }
    return size;
}

// Collects UTF-8 bytes and writes them to a console as UTF-16.
class ConsoleBuffer final : public std::streambuf {
public:
    explicit ConsoleBuffer(HANDLE handle) noexcept : handle_(handle) {
        setp(buffer_.data(), buffer_.data() + buffer_.size());
    }

protected:
    int_type overflow(int_type ch) override {
        if (flushBuffer() != 0) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override { return flushBuffer(); }

private:
    int flushBuffer() {
        const auto size = static_cast<std::size_t>(pptr() - pbase());
        const std::size_t complete = completeUtf8(pbase(), size);
        int result = 0;
        if (complete != 0) {
            const int bytes = static_cast<int>(complete);
            const int units = ::MultiByteToWideChar(CP_UTF8, 0, pbase(), bytes, nullptr, 0);
            if (units > 0) {
                std::wstring wide(static_cast<std::size_t>(units), L'\0');
                (void)::MultiByteToWideChar(CP_UTF8, 0, pbase(), bytes, wide.data(), units);
                for (std::size_t done = 0; done < wide.size();) {
                    DWORD written = 0;
                    const auto count = static_cast<DWORD>(wide.size() - done);
                    if (::WriteConsoleW(handle_, wide.data() + done, count, &written, nullptr) == 0 || written == 0) {
                        result = -1;
                        break;
                    }
                    done += written;
                }
            }
        }
        // An incomplete sequence waits for the rest of its bytes.
        const std::size_t kept = size - complete;
        std::memmove(buffer_.data(), pbase() + complete, kept);
        setp(buffer_.data(), buffer_.data() + buffer_.size());
        pbump(static_cast<int>(kept));
        return result;
    }

    HANDLE handle_;
    std::array<char, 4096> buffer_{};
};

}  // namespace

struct ConsoleSession::State {
    std::unique_ptr<ConsoleBuffer> out;
    std::unique_ptr<ConsoleBuffer> err;
    std::streambuf* previousOut = nullptr;
    std::streambuf* previousErr = nullptr;
    std::size_t width = 0;
};

ConsoleSession::ConsoleSession(Interrupt& interrupt) : state_(std::make_unique<State>()) {
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (isConsole(out)) {
        state_->out = std::make_unique<ConsoleBuffer>(out);
        state_->previousOut = std::cout.rdbuf(state_->out.get());
    }
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (isConsole(err)) {
        state_->err = std::make_unique<ConsoleBuffer>(err);
        state_->previousErr = std::cerr.rdbuf(state_->err.get());
        CONSOLE_SCREEN_BUFFER_INFO screen{};
        if (::GetConsoleScreenBufferInfo(err, &screen) != 0 && screen.srWindow.Right >= screen.srWindow.Left) {
            state_->width = static_cast<std::size_t>(screen.srWindow.Right - screen.srWindow.Left + 1);
        }
    }
    currentInterrupt.store(&interrupt);
    (void)::SetConsoleCtrlHandler(&onConsoleEvent, TRUE);
}

ConsoleSession::~ConsoleSession() {
    (void)::SetConsoleCtrlHandler(&onConsoleEvent, FALSE);
    currentInterrupt.store(nullptr);
    std::cout.flush();
    std::cerr.flush();
    if (state_->previousOut != nullptr) {
        std::cout.rdbuf(state_->previousOut);
    }
    if (state_->previousErr != nullptr) {
        std::cerr.rdbuf(state_->previousErr);
    }
}

bool ConsoleSession::errorIsConsole() const noexcept {
    return state_->err != nullptr;
}

std::size_t ConsoleSession::consoleWidth() const noexcept {
    return state_->width;
}

std::vector<std::string> utf8Arguments(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* const argument = argv[i];
        const int units = static_cast<int>(std::wcslen(argument));
        if (units == 0) {
            arguments.emplace_back();
            continue;
        }
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, argument, units, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
        if (bytes > 0) {
            (void)::WideCharToMultiByte(CP_UTF8, 0, argument, units, text.data(), bytes, nullptr, nullptr);
        }
        arguments.push_back(std::move(text));
    }
    return arguments;
}

std::optional<std::filesystem::path> defaultSessionsRoot() {
    // The first call gives the size, terminating null included.
    const DWORD size = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    if (length == 0 || length >= size) {
        return std::nullopt;
    }
    value.resize(length);
    return std::filesystem::path(value) / L"RecoveryEngine" / L"Sessions";
}

}  // namespace recovery::cli::console
