#include "cli/sessions.hpp"

#include "cli/format.hpp"
#include "recovery/text.hpp"

#include <system_error>

namespace recovery::cli {

namespace {

// ERROR_SHARING_VIOLATION: the journal is held by another session object.
constexpr std::uint32_t kSharingViolation = 32;

}  // namespace

Result<std::filesystem::path> sessionsRoot(const ParsedOptions& options, const Environment& environment) {
    if (const std::optional<std::string_view> given = options.value(kSessionsDirName); given.has_value()) {
        if (given->empty()) {
            return makeError(ErrorCode::InvalidInput, std::string(kSessionsDirName) + " needs a folder");
        }
        return pathFromUtf8(*given);
    }
    if (environment.defaultSessionsRoot.has_value()) {
        return *environment.defaultSessionsRoot;
    }
    return makeError(ErrorCode::InvalidInput,
                     "there is no default sessions folder (%LOCALAPPDATA% is not set): give one with " +
                         std::string(kSessionsDirName));
}

Result<std::unique_ptr<session::RecoverySession>> openSession(const std::filesystem::path& root, std::string_view id,
                                                              Context& context) {
    Result<std::filesystem::path> name = pathFromUtf8(id);
    if (!name.ok()) {
        return name.error();
    }
    const std::filesystem::path folder = root / *name;
    std::error_code ec;
    if (!std::filesystem::exists(folder / session::RecoverySession::kJournalName, ec)) {
        return makeError(ErrorCode::InvalidInput, "there is no session '" + printable(id) + "' in '" +
                                                      displayPath(root) + "' (recovery report lists the sessions)");
    }
    const session::SessionTime before = session::sessionNow();
    session::SessionOptions options;
    options.logger = context.logger();
    options.diskResolver = context.diskResolver();
    Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(folder, options);
    if (!opened.ok()) {
        const Error& error = opened.error();
        if (error.code == ErrorCode::DestinationError && error.systemErrorCode == kSharingViolation) {
            return makeError(error.code,
                             "session '" + printable(id) + "' is in use: another recovery command is running it",
                             error.systemErrorCode);
        }
        return makeError(error.code, "cannot open session '" + printable(id) + "': " + describe(error),
                         error.systemErrorCode);
    }
    const session::SessionInfo info = (*opened)->info();
    if (info.tornBytesDropped != 0) {
        context.note("The session's last record was incomplete (the program ended while writing it); it was "
                     "dropped, and the work it held is done again.");
    }
    for (const session::SessionDamage& damage : info.damage) {
        if (damage.time >= before) {
            context.warning("the session's journal was damaged: " + formatBytes(damage.damage.bytesDropped) +
                            " were dropped; a copy is kept as '" + printable(damage.damage.backup) +
                            "', and the work lost is done again");
        }
    }
    return opened;
}

std::string sessionArguments(std::string_view id, const ParsedOptions& options) {
    std::string text = "--session " + quoteArgument(id);
    if (const std::optional<std::string_view> root = options.value(kSessionsDirName); root.has_value()) {
        text += " " + std::string(kSessionsDirName) + " " + quoteArgument(*root);
    }
    return text;
}

}  // namespace recovery::cli
