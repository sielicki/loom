// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/address.hpp"

#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace loom {

auto address_from_raw(const void* data, std::size_t len, address_format format) noexcept
    -> address {
    if (data == nullptr || len == 0) {
        return unspecified_address{};
    }

    switch (format) {
        case address_format::inet: {
            if (len < sizeof(::sockaddr_in)) {
                return unspecified_address{};
            }
            const auto* sa = static_cast<const ::sockaddr_in*>(data);
            auto addr_be = sa->sin_addr.s_addr;
            auto a = static_cast<std::uint8_t>((addr_be >> 0) & 0xFF);
            auto b = static_cast<std::uint8_t>((addr_be >> 8) & 0xFF);
            auto c = static_cast<std::uint8_t>((addr_be >> 16) & 0xFF);
            auto d = static_cast<std::uint8_t>((addr_be >> 24) & 0xFF);
            auto port = ntohs(sa->sin_port);
            return ipv4_address{a, b, c, d, port};
        }

        case address_format::inet6: {
            if (len < sizeof(::sockaddr_in6)) {
                return unspecified_address{};
            }
            const auto* sa6 = static_cast<const ::sockaddr_in6*>(data);
            std::array<std::uint16_t, 8> segments{};
            for (std::size_t i = 0; i < 8; ++i) {
                auto hi = static_cast<std::uint16_t>(sa6->sin6_addr.s6_addr[i * 2]);
                auto lo = static_cast<std::uint16_t>(sa6->sin6_addr.s6_addr[i * 2 + 1]);
                segments[i] = static_cast<std::uint16_t>((hi << 8) | lo);
            }
            auto port = ntohs(sa6->sin6_port);
            return ipv6_address{std::span<const std::uint16_t, 8>{segments}, port};
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
