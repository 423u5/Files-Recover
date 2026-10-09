#pragma once

// Where sessions live and how a command finds one: the sessions folder
// (--sessions-dir, else %LOCALAPPDATA%\RecoveryEngine\Sessions), a session by
// its id, and the command lines that go on with a session.

#include "cli/arguments.hpp"
#include "cli/context.hpp"
#include "recovery/result.hpp"
#include "session/recovery_session.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace recovery::cli {

inline constexpr std::string_view kSessionsDirName = "--sessions-dir";

// The sessions folder: --sessions-dir, else the environment's default.
// Fails when neither is known.
[[nodiscard]] Result<std::filesystem::path> sessionsRoot(const ParsedOptions& options, const Environment& environment);

// Opens session `id` below `root`, alone. Fails, saying why, when there is no
// such session, when it is in use (another command runs it), or when it
// cannot be read. Warns about damage repaired while opening it.
[[nodiscard]] Result<std::unique_ptr<session::RecoverySession>> openSession(const std::filesystem::path& root,
                                                                            std::string_view id, Context& context);

// The arguments that name session `id` again on a command line:
// "--session ID", with --sessions-dir when the command was given one.
[[nodiscard]] std::string sessionArguments(std::string_view id, const ParsedOptions& options);

}  // namespace recovery::cli
