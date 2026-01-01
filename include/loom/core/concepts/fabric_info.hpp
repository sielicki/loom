// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <string_view>

#include "loom/core/concepts/strong_types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept fabric_info_type
 * @brief Concept for fabric information types.
 * @tparam T The fabric info type.
 *
 * Requires get_caps() and get_provider_name() methods.
 */
template <typename T>
concept fabric_info_type = requires(const T& info) {
    { info.get_caps() } noexcept -> strong_type_concept;
    { info.get_provider_name() } -> std::convertible_to<std::string_view>;
};

/**
 * @concept capability_requirement
 * @brief Concept for capability requirement types.
 * @tparam T The requirement type.
 *
 * Requires required_caps and endpoint_type static members.
 */
template <typename T>
concept capability_requirement = requires {
    { T::required_caps };
    { T::endpoint_type };
};

}  // namespace loom
