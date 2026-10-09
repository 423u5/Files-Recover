#include "cli/format.hpp"

namespace recovery::cli {

std::string quoteArgument(std::string_view argument) {
    const bool needsQuotes =
        argument.empty() || argument.find_first_of(" \t\"") != std::string_view::npos;
    if (!needsQuotes) {
        return std::string(argument);
    }
    // Backslashes are literal except before a quote, where 2n of them read
    // as n and an odd one escapes the quote.
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

}  // namespace recovery::cli
