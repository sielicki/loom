// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept power_of_two
 * @brief Concept checking if a value is a power of two.
 * @tparam N The compile-time value to check.
 */
template <auto N>
concept power_of_two = (N > 0) && ((N & (N - 1)) == 0);

/**
 * @concept valid_alignment
 * @brief Concept checking if an alignment value is valid.
 * @tparam N The alignment value.
 *
 * Must be a power of two and within reasonable bounds.
 */
template <std::size_t N>
concept valid_alignment = power_of_two<N> && (N <= alignof(std::max_align_t) * 16);

}  // namespace loom
