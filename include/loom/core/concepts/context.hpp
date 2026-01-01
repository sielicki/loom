// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <type_traits>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept context_pointer
 * @brief Concept for user context pointer types.
 * @tparam T The pointer type.
 *
 * Allows nullptr, void*, or smart pointer types with get() accessor.
 */
template <typename T>
concept context_pointer = std::is_null_pointer_v<T> || std::is_same_v<T, void*> || requires(T ptr) {
    { ptr.get() } noexcept -> std::convertible_to<void*>;
};

}  // namespace loom
