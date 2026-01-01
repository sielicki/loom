// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <system_error>
#include <utility>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept completion_handler
 * @brief Concept for completion callback handlers.
 * @tparam F The callable type.
 *
 * Requires the callable to accept an error_code parameter.
 */
template <typename F>
concept completion_handler = requires(F&& f, std::error_code ec) {
    { std::forward<F>(f)(ec) };
};

/**
 * @concept result_handler
 * @brief Concept for result callback handlers with a value.
 * @tparam F The callable type.
 * @tparam T The result value type.
 *
 * Requires the callable to accept an error_code and result value.
 */
template <typename F, typename T>
concept result_handler = requires(F&& f, std::error_code ec, T value) {
    { std::forward<F>(f)(ec, value) };
};

}  // namespace loom
