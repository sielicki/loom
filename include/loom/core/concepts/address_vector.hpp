// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstdint>

#include "loom/core/concepts/address.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept av_insertable
 * @brief Concept for types that can be inserted into address vectors.
 * @tparam T The address type.
 *
 * Alias for address_type concept.
 */
template <typename T>
concept av_insertable = address_type<T>;

/**
 * @concept av_handle_type
 * @brief Concept for address vector handle types.
 * @tparam T The handle type.
 *
 * Requires valid() method and value member.
 */
template <typename T>
concept av_handle_type = requires(const T& h) {
    { h.valid() } noexcept -> std::convertible_to<bool>;
    { h.value } -> std::convertible_to<std::uint64_t>;
};

}  // namespace loom
