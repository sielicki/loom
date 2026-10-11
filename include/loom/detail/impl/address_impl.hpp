// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "loom/core/address.hpp"

#ifdef LOOM_IMPLEMENTATION

namespace loom {

auto address_from_raw(const void* data, std::size_t len, address_format format) noexcept
    -> address {
    if (data == nullptr || len == 0) {
        return unspecified_address{};
    }

    switch (format) {
        case address_format::sockaddr:
        case address_format::sockaddr_ip: {
            if (len < sizeof(::sockaddr)) {
                return unspecified_address{};
            }
            ::sockaddr sa{};
            std::memcpy(&sa, data, sizeof(sa));
            switch (sa.sa_family) {
                case AF_INET:
                    return address_from_raw(data, len, address_format::inet);
                case AF_INET6:
                    return address_from_raw(data, len, address_format::inet6);
                default:
                    return unspecified_address{};
            }
        }
        case address_format::inet: {
            if (len < sizeof(::sockaddr_in)) {
                return unspecified_address{};
            }
            ::sockaddr_in sa{};
            std::memcpy(&sa, data, sizeof(sa));
            if (sa.sin_family != AF_INET) {
                return unspecified_address{};
            }
            return ipv4_address{ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)};
        }

        case address_format::inet6: {
            if (len < sizeof(::sockaddr_in6)) {
                return unspecified_address{};
            }
            ::sockaddr_in6 sa6{};
            std::memcpy(&sa6, data, sizeof(sa6));
            if (sa6.sin6_family != AF_INET6) {
                return unspecified_address{};
            }
            std::array<std::uint16_t, 8> segments{};
            for (std::size_t i = 0; i < 8; ++i) {
                auto hi = static_cast<std::uint16_t>(sa6.sin6_addr.s6_addr[i * 2]);
                auto lo = static_cast<std::uint16_t>(sa6.sin6_addr.s6_addr[i * 2 + 1]);
                segments[i] = static_cast<std::uint16_t>((hi << 8) | lo);
            }
            auto port = ntohs(sa6.sin6_port);
            return ipv6_address{std::span<const std::uint16_t, 8>{segments},
                                port,
                                sa6.sin6_scope_id,
                                ntohl(sa6.sin6_flowinfo)};
        }

        case address_format::ib: {
            if (len < ib_address::gid_size) {
                return unspecified_address{};
            }
            const auto* raw = static_cast<const std::uint8_t*>(data);
            std::array<std::uint8_t, 16> gid{};
            std::memcpy(gid.data(), raw, 16);
            return ib_address{std::span<const std::uint8_t, 16>{gid}, 0, 0};
        }

        case address_format::ethernet: {
            if (len < ethernet_address::mac_size) {
                return unspecified_address{};
            }
            const auto* raw = static_cast<const std::uint8_t*>(data);
            std::array<std::uint8_t, 6> mac{};
            std::memcpy(mac.data(), raw, 6);
            return ethernet_address{std::span<const std::uint8_t, 6>{mac}};
        }

        case address_format::unspecified:
        default:
            return unspecified_address{};
    }
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
