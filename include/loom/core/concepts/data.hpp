// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <type_traits>

#include <sys/uio.h>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class mr_descriptor
 * @brief Forward declaration of memory region descriptor.
 */
class mr_descriptor;

/**
 * @concept trivially_serializable
 * @brief Concept for types that can be serialized as raw bytes.
 * @tparam T The type to check.
 *
 * Must be trivially copyable, standard layout, and not a pointer.
 */
template <typename T>
concept trivially_serializable =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && !std::is_pointer_v<T>;

/**
 * @concept contiguous_byte_range
 * @brief Concept for contiguous ranges of bytes.
 * @tparam T The range type.
 *
 * Requires data(), size(), and contiguous iterators.
 */
template <typename T>
concept contiguous_byte_range = requires(T&& t) {
    { std::data(t) } -> std::convertible_to<const std::byte*>;
    { std::size(t) } -> std::convertible_to<std::size_t>;
    requires std::contiguous_iterator<decltype(std::begin(t))>;
};

/**
 * @concept mutable_byte_range
 * @brief Concept for mutable contiguous ranges of bytes.
 * @tparam T The range type.
 *
 * Extends contiguous_byte_range with mutable data access.
 */
template <typename T>
concept mutable_byte_range = contiguous_byte_range<T> && requires(T&& t) {
    { std::data(t) } -> std::convertible_to<std::byte*>;
};

/**
 * @concept buffer_sequence
 * @brief Concept for sequences of contiguous byte ranges.
 * @tparam T The sequence type.
 *
 * Requires iterable elements that are each contiguous_byte_range.
 */
template <typename T>
concept buffer_sequence = requires(T&& seq) {
    { std::begin(seq) } -> std::input_iterator;
    { std::end(seq) } -> std::sentinel_for<decltype(std::begin(seq))>;
    requires contiguous_byte_range<decltype(*std::begin(seq))>;
};

/**
 * @concept iov_range
 * @brief Concept for contiguous ranges of iovec structures.
 * @tparam T The range type.
 *
 * For scatter-gather I/O operations.
 */
template <typename T>
concept iov_range = std::ranges::contiguous_range<T> &&
                    std::same_as<std::ranges::range_value_t<std::remove_cvref_t<T>>, ::iovec>;

/**
 * @concept descriptor_range
 * @brief Concept for contiguous ranges of memory region descriptors.
 * @tparam T The range type.
 */
template <typename T>
concept descriptor_range =
    std::ranges::contiguous_range<T> &&
    std::same_as<std::ranges::range_value_t<std::remove_cvref_t<T>>, mr_descriptor>;

namespace detail {

template <typename T>
struct static_size : std::integral_constant<std::size_t, 0> {
    static constexpr bool has_static_size = false;
};

template <typename T, std::size_t N>
struct static_size<T[N]> : std::integral_constant<std::size_t, N * sizeof(T)> {
    static constexpr bool has_static_size = true;
};

template <typename T, std::size_t N>
struct static_size<std::array<T, N>> : std::integral_constant<std::size_t, N * sizeof(T)> {
    static constexpr bool has_static_size = true;
};

template <typename T>
inline constexpr bool has_static_size_v = static_size<std::remove_cvref_t<T>>::has_static_size;

template <typename T>
inline constexpr std::size_t static_size_v = static_size<std::remove_cvref_t<T>>::value;

}  // namespace detail

/**
 * @concept injectable_buffer
 * @brief Concept for buffers suitable for inject operations.
 * @tparam T The buffer type.
 * @tparam MaxInjectSize The maximum inject size supported by the endpoint.
 *
 * For statically-sized buffers, validates at compile time that the buffer fits.
 * For dynamically-sized buffers, only checks that it's a valid byte range.
 * Runtime validation is still required for dynamic buffers.
 */
template <typename T, std::size_t MaxInjectSize>
concept injectable_buffer = contiguous_byte_range<T> && (!detail::has_static_size_v<T> ||
                                                         detail::static_size_v<T> <= MaxInjectSize);

/**
 * @concept statically_injectable_buffer
 * @brief Concept for buffers with compile-time size validation for inject.
 * @tparam T The buffer type.
 * @tparam MaxInjectSize The maximum inject size.
 *
 * Only satisfied by fixed-size types (arrays, std::array) that fit within MaxInjectSize.
 */
template <typename T, std::size_t MaxInjectSize>
concept statically_injectable_buffer = contiguous_byte_range<T> && detail::has_static_size_v<T> &&
                                       detail::static_size_v<T> <= MaxInjectSize;

/**
 * @concept bounded_iov_range
 * @brief Concept for IOV ranges with a maximum element count.
 * @tparam T The range type.
 * @tparam MaxIOV The maximum number of IOV entries allowed.
 *
 * For statically-sized arrays, validates at compile time.
 * For dynamic containers, runtime validation is still required.
 */
template <typename T, std::size_t MaxIOV>
concept bounded_iov_range = iov_range<T> && std::ranges::sized_range<T>;

/**
 * @concept statically_bounded_iov_range
 * @brief Concept for IOV arrays with compile-time size validation.
 * @tparam T The range type.
 * @tparam MaxIOV The maximum number of IOV entries allowed.
 *
 * Only satisfied by fixed-size IOV arrays that don't exceed MaxIOV.
 */
template <typename T, std::size_t MaxIOV>
concept statically_bounded_iov_range = iov_range<T> && detail::has_static_size_v<T> &&
                                       (detail::static_size_v<T> / sizeof(::iovec)) <= MaxIOV;

}  // namespace loom
