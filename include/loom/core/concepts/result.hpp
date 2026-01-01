// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <system_error>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept result_type
 * @brief Concept for result types with value and error.
 * @tparam T The result type.
 *
 * Requires value_type, error_type aliases, has_value(), and error() methods.
 */
template <typename T>
concept result_type = requires(const T& r) {
    typename T::value_type;
    typename T::error_type;
    { r.has_value() } -> std::convertible_to<bool>;
    { r.error() } -> std::convertible_to<std::error_code>;
};

/**
 * @concept fallible_operation
 * @brief Concept for operations that may fail.
 * @tparam T The operation type.
 *
 * Requires the operation to return a result_type when invoked.
 */
template <typename T>
concept fallible_operation = requires(T op) {
    { op() } -> result_type;
};

}  // namespace loom
