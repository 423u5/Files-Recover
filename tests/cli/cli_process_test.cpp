// The program itself (P18): what main.cpp adds to the library, run as a
// process. Its exit codes; arguments in UTF-16, Unicode paths included, read
// back as the library takes them; UTF-8 output to a pipe; the quoting of the
// commands it suggests, read back by the C runtime's rules; and Ctrl+Break,
// which stops a scan cleanly so that it can be resumed.

#include "cli/format.hpp"
#include "cli_test_support.hpp"
#include "recovery/version.hpp"
#include "scan/scan_test_support.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

namespace recovery::cli {
namespace {

using namespace std::chrono_literals;
using test::Bytes;

// RECOVERY_CLI_EXE comes from CMake ($<TARGET_FILE:recovery_cli>).
std::wstring program() {
    const std::string utf8 = RECOVERY_CLI_EXE;
    const Result<std::filesystem::path> path = pathFromUtf8(utf8);
    return path.ok() ? path->native() : std::wstring();
}

std::wstring widen(std::string_view utf8) {
    const Result<std::filesystem::path> path = pathFromUtf8(utf8);
    return path.ok() ? path->native() : std::wstring();
}

// One argument quoted by the C runtime's rules (the reverse of
// CommandLineToArgvW), so that the program reads back exactly `argument`.
std::wstring quoted(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            ::CloseHandle(value);
        }
        value = nullptr;
    }
};

// A process of the program with its standard output and error in pipes,
// read on threads of their own.
class Process {
public:
    Process(const std::vector<std::wstring>& arguments, DWORD flags = 0) {
        std::wstring commandLine = quoted(program());
        for (const std::wstring& argument : arguments) {
            commandLine += L" " + quoted(argument);
        }
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE outRead = nullptr;
        HANDLE outWrite = nullptr;
        HANDLE errRead = nullptr;
        HANDLE errWrite = nullptr;
        if (::CreatePipe(&outRead, &outWrite, &inherit, 0) == 0 ||
            ::CreatePipe(&errRead, &errWrite, &inherit, 0) == 0) {
            ADD_FAILURE() << "CreatePipe failed: " << ::GetLastError();
            return;
        }
        outRead_.value = outRead;
        errRead_.value = errRead;
        Handle outWriteEnd(outWrite);
        Handle errWriteEnd(errWrite);
        (void)::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        (void)::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = outWrite;
        startup.hStdError = errWrite;
        PROCESS_INFORMATION info{};
        if (::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup,
                             &info) == 0) {
            ADD_FAILURE() << "CreateProcessW failed: " << ::GetLastError();
            return;
        }
        process_.value = info.hProcess;
        id_ = info.dwProcessId;
        ::CloseHandle(info.hThread);
        // The child holds the write ends now: reads end when it exits.
        outWriteEnd.reset();
        errWriteEnd.reset();
        outReader_ = std::thread([this] { drain(outRead_.value, out_); });
        errReader_ = std::thread([this] { drain(errRead_.value, err_); });
    }

    ~Process() {
        if (process_.value != nullptr && ::WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) {
            ::TerminateProcess(process_.value, 99);
        }
        if (outReader_.joinable()) {
            outReader_.join();
        }
        if (errReader_.joinable()) {
            errReader_.join();
        }
    }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    [[nodiscard]] DWORD id() const noexcept { return id_; }

    // Waits for the exit (at most `timeout`) and returns its code.
    [[nodiscard]] DWORD wait(std::chrono::milliseconds timeout = 120s) {
        if (process_.value == nullptr) {
            return 0xFFFFFFFF;
        }
        if (::WaitForSingleObject(process_.value, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
            ADD_FAILURE() << "the program did not exit";
            ::TerminateProcess(process_.value, 99);
        }
        DWORD code = 0;
        (void)::GetExitCodeProcess(process_.value, &code);
        outReader_.join();
        errReader_.join();
        return code;
    }

    // The output so far (thread-safe).
    [[nodiscard]] std::string out() const {
        const std::lock_guard lock(mutex_);
        return out_;
    }
    [[nodiscard]] std::string err() const {
        const std::lock_guard lock(mutex_);
        return err_;
    }

private:
    void drain(HANDLE pipe, std::string& into) {
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        while (::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != 0 && read != 0) {
            const std::lock_guard lock(mutex_);
            into.append(buffer.data(), read);
        }
    }

    Handle process_;
    Handle outRead_;
    Handle errRead_;
    DWORD id_ = 0;
    mutable std::mutex mutex_;
    std::string out_;
    std::string err_;
    std::thread outReader_;
    std::thread errReader_;
};

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Ctrl+Break goes to processes that share the sender's console. A test run
// without one (from a service, a pipe, some CI runners) makes its own,
// hidden, for as long as it needs it.
class TestConsole {
public:
    TestConsole() {
        if (!available() && ::AllocConsole() != 0) {
            allocated_ = true;
            if (const HWND window = ::GetConsoleWindow(); window != nullptr) {
                ::ShowWindow(window, SW_HIDE);
            }
        }
    }
    ~TestConsole() {
        if (allocated_) {
            ::FreeConsole();
        }
    }
    TestConsole(const TestConsole&) = delete;
    TestConsole& operator=(const TestConsole&) = delete;

    // Attached to a console (a pseudoconsole has no window, but is one).
    [[nodiscard]] static bool available() {
        std::array<DWORD, 4> processes{};
        return ::GetConsoleProcessList(processes.data(), static_cast<DWORD>(processes.size())) > 0;
    }

private:
    bool allocated_ = false;
};

TEST(CliProcessTest, ExitCodes) {
    {
        Process version({L"--version"});
        EXPECT_EQ(version.wait(), 0u);
        EXPECT_EQ(version.out(), std::string(kEngineName) + " " + std::string(kEngineVersion) + "\r\n");
    }
    {
        Process none({});
        EXPECT_EQ(none.wait(), 1u);
        EXPECT_TRUE(contains(none.err(), "Usage: recovery <command>")) << none.err();
    }
    {
        Process unknown({L"frobnicate"});
        EXPECT_EQ(unknown.wait(), 1u);
    }
    {
        ::recovery::test::TempDir dir;
        Process missing({L"inspect", L"--source", (dir.path() / L"nope.img").native()});
        EXPECT_EQ(missing.wait(), 2u);
        EXPECT_TRUE(contains(missing.err(), "recovery inspect: error: cannot read the source")) << missing.err();
    }
}

TEST(CliProcessTest, UnicodePathsGoInAndComeOut) {
    ::recovery::test::TempDir dir;
    // Cyrillic, Japanese, an emoji (a surrogate pair) and spaces.
    const std::wstring name = L"\u041A\u0430\u0440\u0442\u0430 \u65E5\u672C \U0001F389";
    const std::filesystem::path image = dir.path() / (name + L".img");
    ::recovery::test::writeFile(image, test::card());
    const std::filesystem::path sessions = dir.path() / (name + L" sessions");
    const std::filesystem::path out = dir.path() / (name + L" out");

    Process inspect({L"inspect", L"--source", image.native()});
    ASSERT_EQ(inspect.wait(), 0u) << inspect.err();
    // To a pipe the program writes UTF-8.
    EXPECT_TRUE(contains(inspect.out(), "Source:          disk image " + test::arg(image))) << inspect.out();

    Process scan({L"scan", L"--source", image.native(), L"--sessions-dir", sessions.native(), L"--quiet"});
    ASSERT_EQ(scan.wait(), 0u) << scan.err();
    const std::optional<std::string> id = test::sessionIdOf([&] {
        std::string text = scan.out();
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    }());
    ASSERT_TRUE(id.has_value()) << scan.out();
    EXPECT_TRUE(std::filesystem::exists(sessions / widen(*id) / L"session.journal"));

    Process recover({L"recover", L"--session", widen(*id), L"--sessions-dir", sessions.native(), L"--output",
                     out.native(), L"--quiet"});
    ASSERT_EQ(recover.wait(), 0u) << recover.err();
    EXPECT_EQ(test::filesBelow(out).size(), 10u);
}

TEST(CliProcessTest, SuggestedCommandsAreQuotedAsTheRuntimeReadsThem) {
    for (const std::string_view argument :
         {"plain", "D:\\Recovered", "D:\\My Files", "D:\\My Files\\", "say \"hi\"", "a\\\\\"b c", "",
          "\\\\server\\x y\\", "tab\there"}) {
        const std::wstring line = L"recovery " + widen(quoteArgument(argument));
        int count = 0;
        LPWSTR* parsed = ::CommandLineToArgvW(line.c_str(), &count);
        ASSERT_NE(parsed, nullptr);
        EXPECT_EQ(count, 2) << argument;
        if (count == 2) {
            EXPECT_EQ(std::wstring(parsed[1]), widen(argument)) << argument;
        }
        ::LocalFree(parsed);
    }
}

// Ctrl+Break reaches the program through the console: the scan stops at its
// next consistent point, the program says how to go on and exits with 3,
// and the session resumes to the result of an uninterrupted scan.
TEST(CliProcessTest, CtrlBreakStopsAScanCleanly) {
    const TestConsole console;
    if (!TestConsole::available()) {
        GTEST_SKIP() << "no console to send Ctrl+Break through, and none could be made";
    }
    ::recovery::test::TempDir dir;
    // A card large enough that the scan is still running when Ctrl+Break comes.
    const std::filesystem::path image = dir.path() / L"large.img";
    ::recovery::test::writeFile(image, scan::test::makeCard(32'768));
    const std::filesystem::path sessions = dir.path() / L"sessions";

    Process scan({L"scan", L"--source", image.native(), L"--sessions-dir", sessions.native()},
                 CREATE_NEW_PROCESS_GROUP);
    // Ctrl+Break once the session is created (its id printed).
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!contains(scan.out(), "Session ") && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(contains(scan.out(), "Session ")) << scan.err();
    ASSERT_NE(::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, scan.id()), 0) << ::GetLastError();
    const DWORD code = scan.wait();
    EXPECT_EQ(code, 3u) << scan.out() << scan.err();
    EXPECT_TRUE(contains(scan.err(), "To go on: recovery scan --session ") ||
                contains(scan.err(), "To scan: recovery scan --session "))
        << scan.err();

    std::string out = scan.out();
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    const std::optional<std::string> id = test::sessionIdOf(out);
    ASSERT_TRUE(id.has_value()) << out;
    Process resumed({L"scan", L"--session", widen(*id), L"--sessions-dir", sessions.native(), L"--quiet"});
    EXPECT_EQ(resumed.wait(), 0u) << resumed.err();
    EXPECT_TRUE(contains(resumed.out(), "completed")) << resumed.out();
}

}  // namespace
}  // namespace recovery::cli
