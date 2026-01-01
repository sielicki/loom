// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept allocator
 * @brief Concept for allocator types.
 * @tparam A The allocator type.
 *
 * Requires value_type alias and allocate/deallocate methods.
 */
template <typename A>
concept allocator = requires(A alloc, std::size_t n) {
    typename A::value_type;
    { alloc.allocate(n) } -> std::same_as<typename A::value_type*>;
    { alloc.deallocate(std::declval<typename A::value_type*>(), n) } -> std::same_as<void>;
};

/**
 * @concept error_policy
 * @brief Concept for error handling policy types.
 * @tparam P The policy type.
 *
 * Requires error_type alias and handle_error() method.
 */
template <typename P>
concept error_policy = requires {
    typename P::error_type;
    { P::handle_error(std::declval<typename P::error_type>()) };
};

/**
 * @concept threading_policy
 * @brief Concept for threading policy types.
 * @tparam P The policy type.
 *
 * Requires mutex_type, lock_guard_type aliases and lock() method.
 */
template <typename P>
concept threading_policy = requires {
    typename P::mutex_type;
    typename P::lock_guard_type;
    {
        P::lock(std::declval<typename P::mutex_type&>())
    } -> std::same_as<typename P::lock_guard_type>;
};

}  // namespace loom
