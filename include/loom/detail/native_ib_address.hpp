// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "loom/core/address.hpp"

namespace loom::detail {

// FI_SOCKADDR_IB uses the Linux sockaddr_ib ABI. Libfabric's ofi_sockaddr_ib
// uses the same portable layout, including AF_IB = 27 on other platforms.
// Keep the transport layout independent of optional rdma-core headers.
inline constexpr std::uint16_t ib_socket_family = 27;
struct native_ib_address {
    std::uint16_t family{};
    std::uint16_t partition_key{};
    std::uint32_t flow_info{};
    std::array<std::uint8_t, 16> gid{};
    std::uint64_t service_id{};
    std::uint64_t service_id_mask{};
    std::uint64_t scope_id{};
};

static_assert(sizeof(native_ib_address) == 48);
static_assert(offsetof(native_ib_address, gid) == 8);
static_assert(offsetof(native_ib_address, service_id) == 24);
static_assert(offsetof(native_ib_address, scope_id) == 40);

template <std::unsigned_integral T>
constexpr auto ib_network_order(T value) noexcept -> T {
    static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big);
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(value);
    return value;
}

constexpr auto to_native_ib_address(const ib_socket_address& address) noexcept
    -> native_ib_address {
    return {.family = ib_socket_family,
            .partition_key = ib_network_order(address.partition_key),
            .flow_info = ib_network_order(address.flow_info),
            .gid = address.gid,
            .service_id = ib_network_order(address.service_id),
            .service_id_mask = ib_network_order(address.service_id_mask),
            .scope_id = address.scope_id};
}

constexpr auto from_native_ib_address(const native_ib_address& address) noexcept
    -> ib_socket_address {
    return {.gid = address.gid,
            .partition_key = ib_network_order(address.partition_key),
            .flow_info = ib_network_order(address.flow_info),
            .service_id = ib_network_order(address.service_id),
            .service_id_mask = ib_network_order(address.service_id_mask),
            .scope_id = address.scope_id};
}

}  // namespace loom::detail
