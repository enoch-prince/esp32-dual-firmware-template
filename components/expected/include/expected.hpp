#pragma once
/**
 * @file expected.hpp
 * @brief Minimal C++17-compatible std::expected polyfill for embedded systems.
 * 
 * Zero heap allocation, no exceptions, no RTTI.
 * Drop-in replacement for std::expected<T, E> in C++23.
 */

#include <utility>
#include <type_traits>
#include <new>

namespace tl {

/**
 * @brief Wrapper for unexpected values (errors)
 */
template<typename E>
class unexpected {
public:
    constexpr explicit unexpected(const E& e) : val_(e) {}
    constexpr explicit unexpected(E&& e) noexcept : val_(std::move(e)) {}
    
    constexpr const E& error() const& noexcept { return val_; }
    constexpr E&& error() && noexcept { return std::move(val_); }
    
private:
    E val_;
};

/**
 * @brief Helper to construct unexpected<E>
 */
template<typename E>
constexpr unexpected<typename std::decay<E>::type> 
make_unexpected(E&& e) {
    return unexpected<typename std::decay<E>::type>(std::forward<E>(e));
}

/**
 * @brief Expected<T, E>: represents a value OR an error
 * 
 * Usage:
 *   tl::expected<int, esp_err_t> read_sensor() {
 *       if (ok) return 42;
 *       return tl::make_unexpected(ESP_ERR_TIMEOUT);
 *   }
 *   
 *   auto result = read_sensor();
 *   if (result) {  use *result }
 *   else {  handle result.error()  }
 */
template<typename T, typename E>
class expected {
public:
    using value_type = T;
    using error_type = E;

    // Default constructor (value-initialized T)
    constexpr expected() noexcept(std::is_nothrow_default_constructible<T>::value)
        : has_val_(true) 
    {
        ::new(static_cast<void*>(std::addressof(val_))) T();
    }
    
    // Construct from value
    constexpr expected(const T& val) noexcept(std::is_nothrow_copy_constructible<T>::value)
        : has_val_(true) 
    {
        ::new(static_cast<void*>(std::addressof(val_))) T(val);
    }
    
    constexpr expected(T&& val) noexcept(std::is_nothrow_move_constructible<T>::value)
        : has_val_(true) 
    {
        ::new(static_cast<void*>(std::addressof(val_))) T(std::move(val));
    }
    
    // Construct from error
    constexpr expected(const unexpected<E>& err) noexcept(std::is_nothrow_copy_constructible<E>::value)
        : has_val_(false) 
    {
        ::new(static_cast<void*>(std::addressof(err_))) E(err.error());
    }
    
    constexpr expected(unexpected<E>&& err) noexcept(std::is_nothrow_move_constructible<E>::value)
        : has_val_(false) 
    {
        ::new(static_cast<void*>(std::addressof(err_))) E(std::move(err.error()));
    }
    
    // Destructor
    ~expected() noexcept {
        if (has_val_) {
            val_.~T();
        } else {
            err_.~E();
        }
    }
    
    // Copy/move constructors
    expected(const expected& other) noexcept(
        std::is_nothrow_copy_constructible<T>::value &&
        std::is_nothrow_copy_constructible<E>::value) : has_val_(other.has_val_) 
    {
        if (has_val_) {
            ::new(static_cast<void*>(std::addressof(val_))) T(other.val_);
        } else {
            ::new(static_cast<void*>(std::addressof(err_))) E(other.err_);
        }
    }
    
    expected(expected&& other) noexcept(
        std::is_nothrow_move_constructible<T>::value &&
        std::is_nothrow_move_constructible<E>::value) : has_val_(other.has_val_) 
    {
        if (has_val_) {
            ::new(static_cast<void*>(std::addressof(val_))) T(std::move(other.val_));
        } else {
            ::new(static_cast<void*>(std::addressof(err_))) E(std::move(other.err_));
        }
    }
    
    // Explicit bool conversion: true if has value
    constexpr explicit operator bool() const noexcept { return has_val_; }
    
    // Access value (undefined behavior if has error)
    constexpr const T& operator*() const& noexcept { return val_; }
    constexpr T& operator*() & noexcept { return val_; }
    constexpr T&& operator*() && noexcept { return std::move(val_); }
    constexpr const T&& operator*() const&& noexcept { return std::move(val_); }
    
    // Access error (undefined behavior if has value)
    constexpr const E& error() const& noexcept { return err_; }
    constexpr E& error() & noexcept { return err_; }
    constexpr E&& error() && noexcept { return std::move(err_); }
    constexpr const E&& error() const&& noexcept { return std::move(err_); }
    
    // Value-or-default access
    template<typename U>
    constexpr T value_or(U&& default_val) const& {
        return has_val_ ? val_ : static_cast<T>(std::forward<U>(default_val));
    }
    
private:
    bool has_val_;
    union {
        T val_;
        E err_;
    };
};


/* ── Specialization: expected<void, E> ─────────────────────────────────── */
template<typename E>
class expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    // Constructors
    constexpr expected() noexcept : has_val_(true) {}
    
    constexpr expected(const unexpected<E>& err) noexcept
        : has_val_(false), err_(err.error()) {}
    
    constexpr expected(unexpected<E>&& err) noexcept
        : has_val_(false), err_(std::move(err.error())) {}
    
    // Bool conversion: true if success (no error)
    constexpr explicit operator bool() const noexcept { return has_val_; }
    
    // Access error
    constexpr const E& error() const& noexcept { return err_; }
    constexpr E& error() & noexcept { return err_; }
    constexpr E&& error() && noexcept { return std::move(err_); }
    constexpr const E&& error() const&& noexcept { return std::move(err_); }
    
    // No operator* for void specialization (nothing to return)
    
private:
    bool has_val_;
    E err_;
};

} // namespace tl