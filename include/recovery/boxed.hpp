#pragma once

// Optional values held on the heap (after P20, L212): the size of a pointer
// when empty, for the large members most objects leave empty (the carve of
// a file the filesystem found, MP4 and fragment evidence), and for large
// results several owners keep unchanged (a volume's candidates, in a scan's
// update and in each checkpoint that applies it).
//
// Both read like std::optional (has_value(), *, ->, value(), emplace(),
// reset(), assignment from a value or std::nullopt), so they replace one in a
// struct without changing the code that uses the member.
//
//  * Boxed<T>: a copy copies the value.
//  * SharedBox<T>: the value is never changed once made: copies share it.
//    Only the reference emplace() returns, before the box is copied, may
//    change it.

#include <concepts>
#include <memory>
#include <optional>
#include <utility>

namespace recovery {

template <typename T>
class Boxed {
public:
    using value_type = T;

    Boxed() noexcept = default;
    // Implicit, as std::optional's: a member assigned or initialised from a
    // value, an optional or nullopt reads the same as before.
    Boxed(std::nullopt_t) noexcept {}
    Boxed(const T& value) : value_(std::make_unique<T>(value)) {}
    Boxed(T&& value) : value_(std::make_unique<T>(std::move(value))) {}
    Boxed(const std::optional<T>& value)
        : value_(value.has_value() ? std::make_unique<T>(*value) : nullptr) {}
    Boxed(std::optional<T>&& value)
        : value_(value.has_value() ? std::make_unique<T>(std::move(*value)) : nullptr) {}
    Boxed(const Boxed& other) : value_(other.value_ != nullptr ? std::make_unique<T>(*other.value_) : nullptr) {}
    Boxed(Boxed&& other) noexcept = default;
    ~Boxed() = default;

    Boxed& operator=(const Boxed& other) {
        if (this != &other) {
            value_ = other.value_ != nullptr ? std::make_unique<T>(*other.value_) : nullptr;
        }
        return *this;
    }
    Boxed& operator=(Boxed&& other) noexcept = default;
    Boxed& operator=(std::nullopt_t) noexcept {
        value_.reset();
        return *this;
    }
    Boxed& operator=(const T& value) {
        value_ = std::make_unique<T>(value);
        return *this;
    }
    Boxed& operator=(T&& value) {
        value_ = std::make_unique<T>(std::move(value));
        return *this;
    }
    Boxed& operator=(const std::optional<T>& value) {
        value_ = value.has_value() ? std::make_unique<T>(*value) : nullptr;
        return *this;
    }
    Boxed& operator=(std::optional<T>&& value) {
        value_ = value.has_value() ? std::make_unique<T>(std::move(*value)) : nullptr;
        return *this;
    }

    [[nodiscard]] bool has_value() const noexcept { return value_ != nullptr; }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] T& operator*() noexcept { return *value_; }
    [[nodiscard]] const T& operator*() const noexcept { return *value_; }
    [[nodiscard]] T* operator->() noexcept { return value_.get(); }
    [[nodiscard]] const T* operator->() const noexcept { return value_.get(); }
    [[nodiscard]] T& value() {
        if (value_ == nullptr) {
            throw std::bad_optional_access();
        }
        return *value_;
    }
    [[nodiscard]] const T& value() const {
        if (value_ == nullptr) {
            throw std::bad_optional_access();
        }
        return *value_;
    }
    template <typename... Args>
    T& emplace(Args&&... args) {
        value_ = std::make_unique<T>(std::forward<Args>(args)...);
        return *value_;
    }
    void reset() noexcept { value_.reset(); }

    friend bool operator==(const Boxed& a, std::nullopt_t) noexcept { return !a.has_value(); }
    friend bool operator==(const Boxed& a, const Boxed& b)
        requires std::equality_comparable<T>
    {
        return a.has_value() == b.has_value() && (!a.has_value() || *a == *b);
    }

private:
    std::unique_ptr<T> value_;
};

template <typename T>
class SharedBox {
public:
    using value_type = T;

    SharedBox() noexcept = default;
    SharedBox(std::nullopt_t) noexcept {}
    SharedBox(const T& value) : value_(std::make_shared<const T>(value)) {}
    SharedBox(T&& value) : value_(std::make_shared<const T>(std::move(value))) {}

    SharedBox& operator=(std::nullopt_t) noexcept {
        value_.reset();
        return *this;
    }
    SharedBox& operator=(const T& value) {
        value_ = std::make_shared<const T>(value);
        return *this;
    }
    SharedBox& operator=(T&& value) {
        value_ = std::make_shared<const T>(std::move(value));
        return *this;
    }

    [[nodiscard]] bool has_value() const noexcept { return value_ != nullptr; }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const T& operator*() const noexcept { return *value_; }
    [[nodiscard]] const T* operator->() const noexcept { return value_.get(); }
    [[nodiscard]] const T& value() const {
        if (value_ == nullptr) {
            throw std::bad_optional_access();
        }
        return *value_;
    }
    // A new value, for its maker to fill in before the box is copied.
    template <typename... Args>
    T& emplace(Args&&... args) {
        auto made = std::make_shared<T>(std::forward<Args>(args)...);
        T& filled = *made;
        value_ = std::move(made);
        return filled;
    }
    void reset() noexcept { value_.reset(); }

    friend bool operator==(const SharedBox& a, std::nullopt_t) noexcept { return !a.has_value(); }

private:
    std::shared_ptr<const T> value_;
};

}  // namespace recovery
