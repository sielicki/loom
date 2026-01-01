// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "loom/core/crtp/crtp_detection.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept registrable_memory
 * @brief Concept for memory that can be registered with fabric.
 * @tparam T The memory container type.
 *
 * Requires contiguous memory with data() and size() accessors.
 */
template <typename T>
concept registrable_memory = requires(T& mem) {
    { std::data(mem) } -> std::convertible_to<void*>;
    { std::size(mem) } -> std::convertible_to<std::size_t>;
    requires std::contiguous_iterator<decltype(std::begin(mem))>;
};

/**
 * @concept remote_memory_descriptor
 * @brief Concept for remote memory descriptors via CRTP detection.
 * @tparam T The descriptor type.
 *
 * Requires CRTP-tagged remote memory with addr, key, and length members.
 */
template <typename T>
concept remote_memory_descriptor = detail::is_remote_memory_v<T> && requires(const T& rm) {
    { rm.addr } -> std::convertible_to<std::uint64_t>;
    { rm.key } -> std::convertible_to<std::uint64_t>;
    { rm.length } -> std::convertible_to<std::size_t>;
};

/**
 * @concept remote_memory_descriptor_duck
 * @brief Concept for remote memory descriptors via duck typing.
 * @tparam T The descriptor type.
 *
 * Checks for remote memory interface without requiring CRTP inheritance.
 */
template <typename T>
concept remote_memory_descriptor_duck = requires(const T& rm) {
    { rm.addr } -> std::convertible_to<std::uint64_t>;
    { rm.key } -> std::convertible_to<std::uint64_t>;
    { rm.length } -> std::convertible_to<std::size_t>;
};

/**
 * @concept memory_region_type
 * @brief Concept for local memory region types.
 * @tparam T The memory region type.
 *
 * Requires descriptor, key, address, and length accessors.
 */
template <typename T>
concept memory_region_type = requires(const T& mr) {
    { mr.descriptor() } noexcept -> std::convertible_to<void*>;
    { mr.key() } noexcept -> std::convertible_to<std::uint64_t>;
    { mr.address() } noexcept -> std::convertible_to<void*>;
    { mr.length() } noexcept -> std::convertible_to<std::size_t>;
};

}  // namespace loom
