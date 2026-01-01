// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <type_traits>

#include "loom/core/crtp/crtp_tags.hpp"

/**
 * @namespace loom::detail
 * @brief Internal implementation details.
 */
namespace loom::detail {

/**
 * @struct has_crtp_tag
 * @brief Detects if a type has a crtp_tag type alias.
 * @tparam T The type to check.
 *
 * Primary template returns false_type.
 */
template <typename T, typename = void>
struct has_crtp_tag : std::false_type {};

/**
 * @struct has_crtp_tag<T, std::void_t<typename T::crtp_tag>>
 * @brief Specialization for types with crtp_tag.
 * @tparam T The type to check.
 */
template <typename T>
struct has_crtp_tag<T, std::void_t<typename T::crtp_tag>> : std::true_type {};

/**
 * @struct has_specific_crtp_tag
 * @brief Checks if a type has a specific CRTP tag.
 * @tparam T The type to check.
 * @tparam Tag The expected tag type.
 *
 * Primary template returns false_type.
 */
template <typename T, typename Tag>
struct has_specific_crtp_tag : std::false_type {};

/**
 * @struct has_specific_crtp_tag<T, Tag>
 * @brief Specialization for types with crtp_tag.
 * @tparam T The type to check.
 * @tparam Tag The expected tag type.
 */
template <typename T, typename Tag>
    requires has_crtp_tag<T>::value
struct has_specific_crtp_tag<T, Tag> : std::is_same<typename T::crtp_tag, Tag> {};

/**
 * @var is_fabric_object_v
 * @brief True if T is a fabric object type.
 * @tparam T The type to check.
 */
template <typename T>
inline constexpr bool is_fabric_object_v = has_specific_crtp_tag<T, crtp::fabric_object_tag>::value;

/**
 * @var is_event_v
 * @brief True if T is an event type.
 * @tparam T The type to check.
 */
template <typename T>
inline constexpr bool is_event_v = has_specific_crtp_tag<T, crtp::event_tag>::value;

/**
 * @var is_remote_memory_v
 * @brief True if T is a remote memory type.
 * @tparam T The type to check.
 */
template <typename T>
inline constexpr bool is_remote_memory_v = has_specific_crtp_tag<T, crtp::remote_memory_tag>::value;

}  // namespace loom::detail
