// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "loom/core/concepts/data.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class ipv4_address
 * @brief Forward declaration of IPv4 address type.
 */
class ipv4_address;
/**
 * @class ipv6_address
 * @brief Forward declaration of IPv6 address type.
 */
class ipv6_address;
/**
 * @class ib_address
 * @brief Forward declaration of InfiniBand address type.
 */
class ib_address;
/**
 * @class ethernet_address
 * @brief Forward declaration of Ethernet MAC address type.
 */
class ethernet_address;
/**
 * @class unspecified_address
 * @brief Forward declaration of unspecified address type.
 */
class unspecified_address;

/**
 * @concept network_address
 * @brief Concept for network address types with port.
 * @tparam T The address type.
 *
 * Requires port() accessor and trivially serializable.
 */
template <typename T>
concept network_address = requires(const T& addr) {
    { addr.port() } noexcept -> std::convertible_to<std::uint16_t>;
} && trivially_serializable<T>;

/**
 * @concept gid_address
 * @brief Concept for InfiniBand GID-based addresses.
 * @tparam T The address type.
 *
 * Requires gid(), qpn(), and lid() accessors.
 */
template <typename T>
concept gid_address = requires(const T& addr) {
    { addr.gid() } noexcept;
    { addr.qpn() } noexcept -> std::convertible_to<std::uint32_t>;
    { addr.lid() } noexcept -> std::convertible_to<std::uint16_t>;
} && trivially_serializable<T>;

/**
 * @concept mac_address
 * @brief Concept for MAC address types.
 * @tparam T The address type.
 *
 * Requires mac() accessor and trivially serializable.
 */
template <typename T>
concept mac_address = requires(const T& addr) {
    { addr.mac() } noexcept;
} && trivially_serializable<T>;

/**
 * @concept address_type
 * @brief Concept for any supported address type.
 * @tparam T The address type.
 *
 * Union of network, GID, MAC addresses, or empty serializable types.
 */
template <typename T>
concept address_type = network_address<T> || gid_address<T> || mac_address<T> ||
                       (trivially_serializable<T> && std::is_empty_v<T>);

/**
 * @concept address_visitor
 * @brief Concept for address variant visitors.
 * @tparam F The visitor callable type.
 *
 * Must be invocable with all address types.
 */
template <typename F>
concept address_visitor = requires(F&& f) {
    requires std::invocable<F, ipv4_address>;
    requires std::invocable<F, ipv6_address>;
    requires std::invocable<F, ib_address>;
    requires std::invocable<F, ethernet_address>;
    requires std::invocable<F, unspecified_address>;
};

}  // namespace loom
