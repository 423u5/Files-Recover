#pragma once

#include "recovery/error.hpp"

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace recovery {

namespace detail {
// Terminates the process. Reading the wrong alternative of a Result is a
// programming error, never an expected runtime condition.
[[noreturn]] void failedResultAccess(const char* what) noexcept;
}  // namespace detail

// Value-or-error return type for expected failures. Exceptions are reserved
// for programming errors and resource exhaustion.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    using ValueType = T;

    template <typename U = T>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Result> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Error>)
    Result(U&& value)  // NOLINT(google-explicit-constructor): implicit by design
        : storage_(std::in_place_index<0>, std::forward<U>(value)) {}

    Result(Error error)  // NOLINT(google-explicit-constructor): implicit by design
        : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return *valuePtr(); }
    [[nodiscard]] const T& value() const& { return *valuePtr(); }
    [[nodiscard]] T&& value() && { return std::move(*valuePtr()); }

    [[nodiscard]] T* operator->() { return valuePtr(); }
    [[nodiscard]] const T* operator->() const { return valuePtr(); }
    [[nodiscard]] T& operator*() & { return *valuePtr(); }
    [[nodiscard]] const T& operator*() const& { return *valuePtr(); }

    [[nodiscard]] const Error& error() const {
        const Error* error = std::get_if<1>(&storage_);
        if (error == nullptr) {
            detail::failedResultAccess("error() called on a successful Result");
        }
        return *error;
    }

private:
    [[nodiscard]] T* valuePtr() {
        T* value = std::get_if<0>(&storage_);
        if (value == nullptr) {
            detail::failedResultAccess("value() called on a failed Result");
        }
        return value;
    }
    [[nodiscard]] const T* valuePtr() const {
        const T* value = std::get_if<0>(&storage_);
        if (value == nullptr) {
            detail::failedResultAccess("value() called on a failed Result");
        }
        return value;
    }

    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() noexcept = default;
    Result(Error error)  // NOLINT(google-explicit-constructor): implicit by design
        : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const {
        if (!error_.has_value()) {
            detail::failedResultAccess("error() called on a successful Status");
        }
        return *error_;
    }

private:
    std::optional<Error> error_;
};

using Status = Result<void>;

[[nodiscard]] inline Status success() noexcept {
    return {};
}

}  // namespace recovery
