// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <expected>
#include <system_error>

#include "loom/core/error.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @typedef result
 * @brief Result type for operations that may fail.
 * @tparam T The success value type.
 */
template <typename T>
using result = std::expected<T, std::error_code>;

/**
 * @typedef void_result
 * @brief Result type for operations with no return value.
 */
using void_result = result<void>;

/**
 * @brief Creates an error result from a loom error code.
 * @tparam T The expected success type.
 * @param e The error code.
 * @return An error result.
 */
template <typename T>
[[nodiscard]] inline auto make_error_result(errc e) noexcept -> result<T> {
    return std::unexpected(make_error_code(e));
}

/**
 * @brief Creates an error result from a std::error_code.
 * @tparam T The expected success type.
 * @param ec The error code.
 * @return An error result.
 */
template <typename T>
[[nodiscard]] inline auto make_error_result(std::error_code ec) noexcept -> result<T> {
    return std::unexpected(ec);
}

/**
 * @brief Creates an error result from a libfabric errno.
 * @tparam T The expected success type.
 * @param fi_errno The libfabric error number.
 * @return An error result.
 */
template <typename T>
[[nodiscard]] inline auto make_error_result_from_fi_errno(int fi_errno) noexcept -> result<T> {
    return std::unexpected(make_error_code_from_fi_errno(fi_errno));
}

/**
 * @brief Creates a success result with a value.
 * @tparam T The value type.
 * @param value The success value.
 * @return A success result containing the value.
 */
template <typename T>
[[nodiscard]] inline auto make_success(T&& value) -> result<std::decay_t<T>> {
    return result<std::decay_t<T>>(std::forward<T>(value));
}

/**
 * @brief Creates a success result with no value.
 * @return A void success result.
 */
[[nodiscard]] inline auto make_success() noexcept -> void_result {
    return void_result();
}

}  // namespace loom
