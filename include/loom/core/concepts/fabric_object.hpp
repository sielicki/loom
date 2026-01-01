// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>

#include "loom/core/crtp/crtp_detection.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept fabric_object
 * @brief Concept for types derived from fabric_object_base.
 * @tparam T The type to check.
 *
 * Requires the type to be detected as a fabric object via CRTP tags
 * and provide internal_ptr() and bool conversion.
 */
template <typename T>
concept fabric_object = detail::is_fabric_object_v<T> && requires(const T& obj) {
    { obj.internal_ptr() } noexcept -> std::convertible_to<void*>;
    { static_cast<bool>(obj) } noexcept -> std::same_as<bool>;
};

/**
 * @concept fabric_object_like
 * @brief Concept for types with fabric object interface via duck typing.
 * @tparam T The type to check.
 *
 * Checks for the fabric object interface without requiring CRTP inheritance.
 */
template <typename T>
concept fabric_object_like = requires(const T& obj) {
    { obj.internal_ptr() } noexcept -> std::convertible_to<void*>;
    { static_cast<bool>(obj) } noexcept -> std::same_as<bool>;
};

/**
 * @concept enableable
 * @brief Concept for fabric objects that can be enabled.
 * @tparam T The type to check.
 *
 * Requires fabric_object_like interface plus an enable() method.
 */
template <typename T>
concept enableable = fabric_object_like<T> && requires(T& obj) {
    { obj.enable() };
};

/**
 * @concept bindable_to
 * @brief Concept for fabric objects that can be bound to other fabric objects.
 * @tparam Bindable The source type to bind.
 * @tparam Target The target type to bind to.
 */
template <typename Bindable, typename Target>
concept bindable_to = fabric_object_like<Bindable> && fabric_object_like<Target>;

}  // namespace loom
