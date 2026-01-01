// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "loom/core/concepts/data.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept atomic_compatible
 * @brief Concept for types compatible with fabric atomic operations.
 * @tparam T The type to check.
 *
 * Requires trivially copyable, 1/2/4/8 byte size, and integral or floating point.
 */
template <typename T>
concept atomic_compatible =
    std::is_trivially_copyable_v<T> &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) &&
    (std::is_integral_v<T> || std::is_floating_point_v<T>);

namespace detail {

template <typename T>
struct atomic_datatype_map;

template <>
struct atomic_datatype_map<std::int8_t> {
    static constexpr atomic_datatype value = atomic_datatype::int8;
};
template <>
struct atomic_datatype_map<std::uint8_t> {
    static constexpr atomic_datatype value = atomic_datatype::uint8;
};
template <>
struct atomic_datatype_map<std::int16_t> {
    static constexpr atomic_datatype value = atomic_datatype::int16;
};
template <>
struct atomic_datatype_map<std::uint16_t> {
    static constexpr atomic_datatype value = atomic_datatype::uint16;
};
template <>
struct atomic_datatype_map<std::int32_t> {
    static constexpr atomic_datatype value = atomic_datatype::int32;
};
template <>
struct atomic_datatype_map<std::uint32_t> {
    static constexpr atomic_datatype value = atomic_datatype::uint32;
};
template <>
struct atomic_datatype_map<std::int64_t> {
    static constexpr atomic_datatype value = atomic_datatype::int64;
};
template <>
struct atomic_datatype_map<std::uint64_t> {
    static constexpr atomic_datatype value = atomic_datatype::uint64;
};
template <>
struct atomic_datatype_map<float> {
    static constexpr atomic_datatype value = atomic_datatype::float32;
};
template <>
struct atomic_datatype_map<double> {
    static constexpr atomic_datatype value = atomic_datatype::float64;
};

template <typename T>
inline constexpr bool has_atomic_datatype_v = requires { atomic_datatype_map<T>::value; };

}  // namespace detail

/**
 * @brief Maps a C++ type to its corresponding libfabric atomic_datatype.
 * @tparam T The C++ type to map.
 */
template <typename T>
    requires detail::has_atomic_datatype_v<T>
inline constexpr atomic_datatype atomic_datatype_v = detail::atomic_datatype_map<T>::value;

/**
 * @concept fi_atomic_type
 * @brief Concept for types that have a direct mapping to libfabric atomic datatypes.
 * @tparam T The type to check.
 *
 * Requires atomic_compatible and a valid atomic_datatype mapping.
 */
template <typename T>
concept fi_atomic_type = atomic_compatible<T> && detail::has_atomic_datatype_v<T>;

namespace detail {

template <atomic_op Op, typename T>
consteval auto is_valid_atomic_op() -> bool {
    if constexpr (Op == atomic_op::min || Op == atomic_op::max || Op == atomic_op::sum ||
                  Op == atomic_op::prod) {
        return std::is_integral_v<T> || std::is_floating_point_v<T>;
    } else if constexpr (Op == atomic_op::lor || Op == atomic_op::land || Op == atomic_op::lxor) {
        return std::is_integral_v<T>;
    } else if constexpr (Op == atomic_op::bor || Op == atomic_op::band || Op == atomic_op::bxor) {
        return std::is_integral_v<T>;
    } else if constexpr (Op == atomic_op::read || Op == atomic_op::write) {
        return true;
    } else if constexpr (Op == atomic_op::cswap || Op == atomic_op::cswap_ne ||
                         Op == atomic_op::cswap_le || Op == atomic_op::cswap_lt ||
                         Op == atomic_op::cswap_ge || Op == atomic_op::cswap_gt ||
                         Op == atomic_op::mswap) {
        return true;
    } else {
        return false;
    }
}

}  // namespace detail

/**
 * @concept valid_atomic_op
 * @brief Concept verifying an atomic operation is valid for a given type.
 * @tparam T The data type.
 * @tparam Op The atomic operation.
 *
 * Ensures the operation is semantically valid for the type (e.g., bitwise ops require integral).
 */
template <typename T, atomic_op Op>
concept valid_atomic_op = fi_atomic_type<T> && detail::is_valid_atomic_op<Op, T>();

/**
 * @concept fetch_addable
 * @brief Concept for types supporting fetch-add atomic operations.
 * @tparam T The type to check.
 *
 * Must be atomic_compatible and integral or floating point.
 */
template <typename T>
concept fetch_addable =
    atomic_compatible<T> && (std::is_integral_v<T> || std::is_floating_point_v<T>);

/**
 * @concept cas_compatible
 * @brief Concept for types supporting compare-and-swap operations.
 * @tparam T The type to check.
 *
 * Alias for atomic_compatible.
 */
template <typename T>
concept cas_compatible = atomic_compatible<T>;

/**
 * @concept collective_element
 * @brief Concept for types usable in collective operations.
 * @tparam T The type to check.
 *
 * Must be trivially serializable and atomic compatible.
 */
template <typename T>
concept collective_element = trivially_serializable<T> && atomic_compatible<T>;

/**
 * @concept reduction_element
 * @brief Concept for types usable in reduction collective operations.
 * @tparam T The type to check.
 *
 * Must be a collective element that supports arithmetic reduction (sum, prod, min, max).
 */
template <typename T>
concept reduction_element =
    collective_element<T> && (std::is_integral_v<T> || std::is_floating_point_v<T>);

/**
 * @concept bitwise_reduction_element
 * @brief Concept for types usable in bitwise reduction operations.
 * @tparam T The type to check.
 *
 * Must be an integral collective element for bitwise OR/AND/XOR reductions.
 */
template <typename T>
concept bitwise_reduction_element = collective_element<T> && std::is_integral_v<T>;

}  // namespace loom
