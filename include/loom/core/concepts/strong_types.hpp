// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_trigger.h>

#include <concepts>
#include <cstddef>
#include <type_traits>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @struct async_context
 * @brief CRTP base for async operation contexts.
 * @tparam Derived The derived context type.
 *
 * Wraps fi_context2 for async operations with user data.
 */
template <typename Derived>
struct async_context {
    ::fi_context2 fi_ctx{};  ///< Libfabric context structure

    /**
     * @brief Returns raw pointer to derived context.
     * @return Void pointer to this context.
     */
    [[nodiscard]] constexpr auto raw() noexcept -> void* {
        return static_cast<void*>(static_cast<Derived*>(this));
    }

    /**
     * @brief Reconstructs derived pointer from raw pointer.
     * @param ptr Raw void pointer.
     * @return Pointer to derived type.
     */
    [[nodiscard]] static constexpr auto from_raw(void* ptr) noexcept -> Derived* {
        return static_cast<Derived*>(ptr);
    }
};

/**
 * @concept structured_context
 * @brief Concept for structured async contexts.
 * @tparam T The context type.
 *
 * Requires fi_ctx at offset 0 and raw() method.
 */
template <typename T>
concept structured_context = requires(T t) {
    requires std::same_as<std::remove_reference_t<decltype(t.fi_ctx)>, ::fi_context2>;
    { t.raw() } noexcept -> std::same_as<void*>;
    requires(offsetof(T, fi_ctx) == 0);
};

/**
 * @struct triggered_context
 * @brief CRTP base for triggered operation contexts.
 * @tparam Derived The derived context type.
 *
 * Wraps fi_triggered_context2 for deferred operations.
 */
template <typename Derived>
struct triggered_context {
    ::fi_triggered_context2 trig_ctx{};  ///< Libfabric triggered context

    /**
     * @brief Returns raw pointer to derived context.
     * @return Void pointer to this context.
     */
    [[nodiscard]] constexpr auto raw() noexcept -> void* {
        return static_cast<void*>(static_cast<Derived*>(this));
    }

    /**
     * @brief Reconstructs derived pointer from raw pointer.
     * @param ptr Raw void pointer.
     * @return Pointer to derived type.
     */
    [[nodiscard]] static constexpr auto from_raw(void* ptr) noexcept -> Derived* {
        return static_cast<Derived*>(ptr);
    }

    /**
     * @brief Returns pointer to triggered context structure.
     * @return Pointer to fi_triggered_context2.
     */
    [[nodiscard]] constexpr auto fi_context_ptr() noexcept -> ::fi_triggered_context2* {
        return &trig_ctx;
    }

    /**
     * @brief Configures threshold trigger.
     * @param cntr Counter to watch.
     * @param threshold Threshold value to trigger at.
     */
    auto set_threshold_trigger(::fid_cntr* cntr, std::size_t threshold) noexcept -> void {
        trig_ctx.event_type = FI_TRIGGER_THRESHOLD;
        trig_ctx.trigger.threshold.cntr = cntr;
        trig_ctx.trigger.threshold.threshold = threshold;
    }
};

/**
 * @concept structured_triggered_context
 * @brief Concept for structured triggered contexts.
 * @tparam T The context type.
 *
 * Requires trig_ctx at offset 0 and context accessor methods.
 */
template <typename T>
concept structured_triggered_context = requires(T t) {
    requires std::same_as<std::remove_reference_t<decltype(t.trig_ctx)>, ::fi_triggered_context2>;
    { t.raw() } noexcept -> std::same_as<void*>;
    { t.fi_context_ptr() } noexcept -> std::same_as<::fi_triggered_context2*>;
    requires(offsetof(T, trig_ctx) == 0);
};

/**
 * @concept bitwise_flags
 * @brief Concept for bitwise flag types.
 * @tparam T The flags type.
 *
 * Requires bitwise operators and has/has_any methods.
 */
template <typename T>
concept bitwise_flags = requires(T a, T b) {
    { a | b } noexcept -> std::same_as<T>;
    { a & b } noexcept -> std::same_as<T>;
    { a ^ b } noexcept -> std::same_as<T>;
    { ~a } noexcept -> std::same_as<T>;
    { a |= b } noexcept -> std::same_as<T&>;
    { a &= b } noexcept -> std::same_as<T&>;
    { a ^= b } noexcept -> std::same_as<T&>;
    { a.has(b) } noexcept -> std::convertible_to<bool>;
    { a.has_any(b) } noexcept -> std::convertible_to<bool>;
};

/**
 * @concept strong_type_concept
 * @brief Concept for strong type wrappers.
 * @tparam T The strong type.
 *
 * Requires value_type, tag_type aliases and get() accessor.
 */
template <typename T>
concept strong_type_concept = requires(const T& t) {
    typename T::value_type;
    typename T::tag_type;
    { t.get() } noexcept -> std::same_as<const typename T::value_type&>;
};

/**
 * @concept unsigned_strong_type
 * @brief Concept for unsigned strong types.
 * @tparam T The strong type.
 *
 * Requires strong_type_concept with unsigned integral value.
 */
template <typename T>
concept unsigned_strong_type =
    strong_type_concept<T> && std::unsigned_integral<typename T::value_type>;

/**
 * @namespace loom::detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct is_strong_type
 * @brief Type trait to detect strong types.
 * @tparam T The type to check.
 *
 * Primary template returns false_type.
 */
template <typename T>
struct is_strong_type : std::false_type {};

/**
 * @concept has_value_type
 * @brief Concept for types with value_type alias.
 * @tparam T The type to check.
 */
template <typename T>
concept has_value_type = requires { typename T::value_type; };

/**
 * @struct is_strong_type<T>
 * @brief Specialization for types with value_type and get().
 * @tparam T The type to check.
 */
template <has_value_type T>
    struct is_strong_type<T> : std::bool_constant < requires(T t) {
    t.get();
}>{};
}  // namespace detail

/**
 * @var is_strong_type_v
 * @brief True if T is a strong type.
 * @tparam T The type to check.
 */
template <typename T>
inline constexpr bool is_strong_type_v = detail::is_strong_type<T>::value;

/**
 * @typedef underlying_type_t
 * @brief Extracts the underlying value type from a strong type.
 * @tparam T The strong type.
 */
template <strong_type_concept T>
using underlying_type_t = typename T::value_type;

/**
 * @var is_flags_type_v
 * @brief True if T is a flags type.
 * @tparam T The type to check.
 */
template <typename T>
inline constexpr bool is_flags_type_v = unsigned_strong_type<T>;

/**
 * @concept opaque_context
 * @brief Concept for opaque context types.
 * @tparam T The context type.
 *
 * Requires raw() method returning void*.
 */
template <typename T>
concept opaque_context = requires(T t) {
    { t.raw() } noexcept -> std::same_as<void*>;
};

/**
 * @concept context_like
 * @brief Concept for context-compatible types.
 * @tparam T The type to check.
 *
 * Allows pointers, nullptr, or structured contexts.
 */
template <typename T>
concept context_like = std::is_pointer_v<T> || std::is_null_pointer_v<T> ||
                       structured_context<std::remove_pointer_t<T>>;

/**
 * @brief Converts a context to raw void pointer.
 * @tparam T The context type.
 * @param ctx The context to convert.
 * @return Raw void pointer for libfabric operations.
 */
template <context_like T>
[[nodiscard]] constexpr auto to_raw_context(T ctx) noexcept -> void* {
    if constexpr (std::is_null_pointer_v<T>) {
        return nullptr;
    } else if constexpr (std::is_pointer_v<T>) {
        if constexpr (structured_context<std::remove_pointer_t<T>>) {
            return ctx ? ctx->raw() : nullptr;
        } else {
            return static_cast<void*>(ctx);
        }
    } else {
        return ctx.raw();
    }
}

}  // namespace loom
