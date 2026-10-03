#include "recovery/result.hpp"

#include <cstdio>
#include <cstdlib>

namespace recovery::detail {

void failedResultAccess(const char* what) noexcept {
    std::fputs("RecoveryEngine: invalid Result access: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

}  // namespace recovery::detail
