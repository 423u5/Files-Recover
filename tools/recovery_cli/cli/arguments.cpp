#include "cli/arguments.hpp"

#include "cli/format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace recovery::cli {

namespace {

Error usage(std::string message) {
    return makeError(ErrorCode::InvalidInput, std::move(message));
}

const OptionSpec* findOption(std::span<const OptionSpec> options, std::string_view text) {
    for (const OptionSpec& option : options) {
        if (option.name == text || (!option.shortName.empty() && option.shortName == text)) {
            return &option;
        }
    }
    return nullptr;
}

std::string inQuotes(std::string_view text) {
    return "'" + printable(text) + "'";
}

// "--source" for an option spec.
std::string spelled(const OptionSpec& option) {
    return std::string(option.name);
}

char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return lower(x) == lower(y);
           });
}

std::string listOf(std::span<const std::string_view> choices) {
    std::string text;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i != 0) {
            text += i + 1 == choices.size() ? " or " : ", ";
        }
        text += choices[i];
    }
    return text;
}

// Digits only, no overflow.
std::optional<std::uint64_t> decimal(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

}  // namespace

bool ParsedOptions::has(std::string_view name) const {
    return values_.find(name) != values_.end();
}

std::optional<std::string_view> ParsedOptions::value(std::string_view name) const {
    const auto found = values_.find(name);
    if (found == values_.end()) {
        return std::nullopt;
    }
    return std::string_view(found->second);
}

std::vector<std::string_view> ParsedOptions::given() const {
    std::vector<std::string_view> names;
    for (const auto& [name, value] : values_) {
        names.emplace_back(name);
    }
    return names;
}

void ParsedOptions::set(std::string name, std::string value) {
    values_[std::move(name)] = std::move(value);
}

Result<ParsedOptions> parseOptions(std::span<const std::string> arguments, std::span<const OptionSpec> options) {
    ParsedOptions parsed;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument.empty() || argument[0] != '-' || argument == "-" || argument == "--") {
            return usage("unexpected argument " + inQuotes(argument) + " (options start with --)");
        }
        std::string_view name = argument;
        std::optional<std::string_view> inlineValue;
        if (argument.starts_with("--")) {
            if (const std::size_t equals = argument.find('='); equals != std::string::npos) {
                name = std::string_view(argument).substr(0, equals);
                inlineValue = std::string_view(argument).substr(equals + 1);
            }
        }
        const OptionSpec* option = findOption(options, name);
        if (option == nullptr) {
            return usage("unknown option " + inQuotes(name));
        }
        if (parsed.has(option->name)) {
            return usage("the option " + spelled(*option) + " is given twice");
        }
        if (option->value.empty()) {
            if (inlineValue.has_value()) {
                return usage("the option " + spelled(*option) + " takes no value");
            }
            parsed.set(std::string(option->name), {});
            continue;
        }
        if (inlineValue.has_value()) {
            parsed.set(std::string(option->name), std::string(*inlineValue));
            continue;
        }
        if (i + 1 >= arguments.size()) {
            return usage("the option " + spelled(*option) + " needs a value (" + std::string(option->value) + ")");
        }
        const std::string& next = arguments[++i];
        if (next.starts_with("--")) {
            return usage("the option " + spelled(*option) + " needs a value (" + std::string(option->value) +
                         "), not " + inQuotes(next));
        }
        parsed.set(std::string(option->name), next);
    }
    return parsed;
}

std::string describeOptions(std::span<const OptionSpec> options) {
    std::vector<std::string> heads;
    std::size_t width = 0;
    for (const OptionSpec& option : options) {
        std::string head = option.shortName.empty() ? "    " : std::string(option.shortName) + ", ";
        head += option.name;
        if (!option.value.empty()) {
            head += ' ';
            head += option.value;
        }
        width = std::max(width, head.size());
        heads.push_back(std::move(head));
    }
    // Help text goes on in the help column, wrapped at word boundaries so that
    // lines stay within kLineWidth.
    constexpr std::size_t kLineWidth = 100;
    const std::size_t column = 2 + width + 2;
    const std::size_t room = kLineWidth > column + 20 ? kLineWidth - column : 20;
    std::string text;
    for (std::size_t i = 0; i < options.size(); ++i) {
        text += "  ";
        text += heads[i];
        text.append(width - heads[i].size() + 2, ' ');
        std::string_view help = options[i].help;
        while (help.size() > room) {
            std::size_t cut = help.rfind(' ', room);
            std::size_t space = 1;
            if (cut == std::string_view::npos || cut == 0) {
                cut = room;
                space = 0;
            }
            text += help.substr(0, cut);
            text += '\n';
            text.append(column, ' ');
            help.remove_prefix(cut + space);
        }
        text += help;
        text += '\n';
    }
    return text;
}

Result<std::uint64_t> parseUnsigned(std::string_view option, std::string_view text, std::uint64_t minimum,
                                    std::uint64_t maximum) {
    const std::optional<std::uint64_t> value = decimal(text);
    if (!value.has_value() || *value < minimum || *value > maximum) {
        return usage(std::string(option) + " must be a whole number from " + std::to_string(minimum) + " to " +
                     std::to_string(maximum) + ", not " + inQuotes(text));
    }
    return *value;
}

Result<std::uint64_t> parseSize(std::string_view option, std::string_view text, std::uint64_t minimum,
                                std::uint64_t maximum) {
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        ++digits;
    }
    const std::string_view suffix = text.substr(digits);
    std::uint64_t unit = 1;
    if (suffix.empty() || equalsIgnoringCase(suffix, "b")) {
        unit = 1;
    } else if (equalsIgnoringCase(suffix, "k") || equalsIgnoringCase(suffix, "kb") ||
               equalsIgnoringCase(suffix, "kib")) {
        unit = 1024;
    } else if (equalsIgnoringCase(suffix, "m") || equalsIgnoringCase(suffix, "mb") ||
               equalsIgnoringCase(suffix, "mib")) {
        unit = 1024 * 1024;
    } else if (equalsIgnoringCase(suffix, "g") || equalsIgnoringCase(suffix, "gb") ||
               equalsIgnoringCase(suffix, "gib")) {
        unit = 1024 * 1024 * 1024;
    } else {
        unit = 0;
    }
    const std::optional<std::uint64_t> count = decimal(text.substr(0, digits));
    const bool fits = count.has_value() && unit != 0 && *count <= std::numeric_limits<std::uint64_t>::max() / unit;
    if (!fits || *count * unit < minimum || *count * unit > maximum) {
        return usage(std::string(option) + " must be a size from " + formatSize(minimum) + " to " +
                     formatSize(maximum) + " (a number of bytes, or with K, M or G), not " + inQuotes(text));
    }
    return *count * unit;
}

Result<std::string> parseChoice(std::string_view option, std::string_view text,
                                std::span<const std::string_view> choices) {
    for (const std::string_view choice : choices) {
        if (text == choice) {
            return std::string(choice);
        }
    }
    return usage(std::string(option) + " must be " + listOf(choices) + ", not " + inQuotes(text));
}

Result<std::vector<std::string>> parseChoices(std::string_view option, std::string_view text,
                                              std::span<const std::string_view> choices) {
    std::vector<std::string> chosen;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = text.find(',', start);
        const std::string_view item = text.substr(start, comma == std::string_view::npos ? text.npos : comma - start);
        Result<std::string> one = parseChoice(option, item, choices);
        if (!one.ok()) {
            return usage(std::string(option) + " takes a comma-separated list of " + listOf(choices) +
                         "; " + inQuotes(item) + " is none of them");
        }
        if (std::find(chosen.begin(), chosen.end(), *one) != chosen.end()) {
            return usage(std::string(option) + " names " + inQuotes(item) + " twice");
        }
        chosen.push_back(std::move(one).value());
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return chosen;
}

bool IdSelection::contains(std::uint64_t id) const noexcept {
    const auto after = std::upper_bound(ranges_.begin(), ranges_.end(), id,
                                        [](std::uint64_t value, const auto& range) { return value < range.first; });
    return after != ranges_.begin() && id <= std::prev(after)->second;
}

std::uint64_t IdSelection::last() const noexcept {
    return ranges_.empty() ? 0 : ranges_.back().second;
}

void IdSelection::add(std::uint64_t first, std::uint64_t last) {
    ranges_.emplace_back(first, last);
    std::sort(ranges_.begin(), ranges_.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (const auto& range : ranges_) {
        if (!merged.empty() && range.first <= merged.back().second + 1) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    ranges_ = std::move(merged);
}

Result<IdSelection> parseIds(std::string_view option, std::string_view text) {
    // Bounded: a list of ranges, not of every id in them.
    constexpr std::size_t kMaxItems = 10'000;
    const auto bad = [&](std::string_view item) {
        return usage(std::string(option) + " takes candidate ids and ranges such as 4 or 1,4-9,12; " +
                     inQuotes(item) + " is not one");
    };
    IdSelection selection;
    std::size_t start = 0;
    for (std::size_t items = 0;; ++items) {
        if (items == kMaxItems) {
            return usage(std::string(option) + " has more than " + std::to_string(kMaxItems) + " items");
        }
        const std::size_t comma = text.find(',', start);
        const std::string_view item = text.substr(start, comma == std::string_view::npos ? text.npos : comma - start);
        const std::size_t dash = item.find('-');
        const std::optional<std::uint64_t> first = decimal(item.substr(0, dash));
        const std::optional<std::uint64_t> last =
            dash == std::string_view::npos ? first : decimal(item.substr(dash + 1));
        if (!first.has_value() || !last.has_value() || *first == 0 || *last < *first) {
            return bad(item);
        }
        selection.add(*first, *last);
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return selection;
}

Result<std::string> parseSessionId(std::string_view option, std::string_view text) {
    constexpr std::string_view kForbidden = "\\/:*?\"<>|";
    const bool control = std::any_of(text.begin(), text.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
    });
    if (text.empty() || text == "." || text == ".." || text.size() > 255 || control ||
        text.find_first_of(kForbidden) != std::string_view::npos || text.back() == '.' || text.back() == ' ') {
        return usage(std::string(option) + " takes a session id such as 20261008-101530-3fa94c2e (the name of " +
                     "its folder below the sessions folder), not " + inQuotes(text));
    }
    return std::string(text);
}

}  // namespace recovery::cli
