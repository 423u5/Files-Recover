#pragma once

// The command line of one command: its options, their values, and what is
// wrong with it. Every error here is a usage error (exit code 1): the
// command line itself is wrong, whatever the disks hold. Errors are
// Result<T> errors with code InvalidInput and a message for the user.

#include "recovery/result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recovery::cli {

struct OptionSpec {
    // "--source"
    std::string_view name;
    // "-h", or empty.
    std::string_view shortName;
    // The value's name in the help ("PATH"); empty for a flag.
    std::string_view value;
    std::string_view help;
};

class ParsedOptions {
public:
    [[nodiscard]] bool has(std::string_view name) const;
    // The option's value; nullopt when the option was not given.
    [[nodiscard]] std::optional<std::string_view> value(std::string_view name) const;
    // The options given, by name.
    [[nodiscard]] std::vector<std::string_view> given() const;

    void set(std::string name, std::string value);

private:
    std::map<std::string, std::string, std::less<>> values_;
};

// Parses `arguments` against `options`: "--name value", "--name=value", and
// short flags ("-h"). Each option at most once; no positional arguments.
[[nodiscard]] Result<ParsedOptions> parseOptions(std::span<const std::string> arguments,
                                                 std::span<const OptionSpec> options);

// The options' help, one aligned line each.
[[nodiscard]] std::string describeOptions(std::span<const OptionSpec> options);

// A decimal number within [minimum, maximum]: digits only.
[[nodiscard]] Result<std::uint64_t> parseUnsigned(std::string_view option, std::string_view text,
                                                  std::uint64_t minimum, std::uint64_t maximum);
// A size in bytes: a number with an optional binary suffix (K or KiB, M or
// MiB, G or GiB; KB, MB and GB mean the same), within [minimum, maximum].
[[nodiscard]] Result<std::uint64_t> parseSize(std::string_view option, std::string_view text, std::uint64_t minimum,
                                              std::uint64_t maximum);
// One of `choices`, exactly.
[[nodiscard]] Result<std::string> parseChoice(std::string_view option, std::string_view text,
                                              std::span<const std::string_view> choices);
// A comma-separated list of `choices`, each at most once, in the order given.
[[nodiscard]] Result<std::vector<std::string>> parseChoices(std::string_view option, std::string_view text,
                                                            std::span<const std::string_view> choices);

// Candidate ids: "4", "1,4-9,12". Ranges are inclusive; ids start at 1.
class IdSelection {
public:
    [[nodiscard]] bool contains(std::uint64_t id) const noexcept;
    // The largest id named.
    [[nodiscard]] std::uint64_t last() const noexcept;
    // Merged, sorted, inclusive ranges.
    [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges() const noexcept {
        return ranges_;
    }

    void add(std::uint64_t first, std::uint64_t last);

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_;
};

[[nodiscard]] Result<IdSelection> parseIds(std::string_view option, std::string_view text);

// A session id: the name of a folder below the sessions folder (no path
// separators, no "." or "..", none of the characters Windows forbids).
[[nodiscard]] Result<std::string> parseSessionId(std::string_view option, std::string_view text);

}  // namespace recovery::cli
