// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstring>
#include <variant>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "loom/core/address.hpp"
#include "loom/core/result.hpp"
#include "loom/detail/native_ib_address.hpp"

namespace loom::detail {

using native_address =
    std::variant<sockaddr_in, sockaddr_in6, native_ib_address, std::span<const std::byte>>;

// Socket values own native objects; opaque values borrow their address's bytes.
[[nodiscard]] inline auto to_native_address(const address& addr, address_format format)
    -> result<native_address> {
    const auto actual = get_address_format(addr);
    const bool ip_format = actual == address_format::inet || actual == address_format::inet6;
    const bool socket_format =
        (format == address_format::sockaddr && (ip_format || actual == address_format::ib)) ||
        (format == address_format::sockaddr_ip && ip_format);
    if (format != address_format::unspecified && format != actual && !socket_format) {
        return make_error_result<native_address>(errc::invalid_argument);
    }
    if (const auto* ip = std::get_if<ipv4_address>(&addr)) {
        sockaddr_in native{};
        native.sin_family = AF_INET;
        native.sin_port = htons(ip->port());
        native.sin_addr.s_addr = htonl(ip->to_uint32());
#if defined(__APPLE__) || defined(__FreeBSD__)
        native.sin_len = sizeof(native);
#endif
        return native;
    }
    if (const auto* ip = std::get_if<ipv6_address>(&addr)) {
        sockaddr_in6 native{};
        native.sin6_family = AF_INET6;
        native.sin6_port = htons(ip->port());
        native.sin6_scope_id = ip->scope_id();
        native.sin6_flowinfo = htonl(ip->flow_info());
        for (std::size_t i = 0; i < ip->segments().size(); ++i) {
            const auto segment = htons(ip->segments()[i]);
            std::memcpy(native.sin6_addr.s6_addr + i * sizeof(segment), &segment, sizeof(segment));
        }
#if defined(__APPLE__) || defined(__FreeBSD__)
        native.sin6_len = sizeof(native);
#endif
        return native;
    }
    if (const auto* ib = std::get_if<ib_socket_address>(&addr))
        return to_native_ib_address(*ib);
    if (const auto* opaque = std::get_if<opaque_address>(&addr)) {
        if (opaque->bytes().empty())
            return make_error_result<native_address>(errc::invalid_argument);
        return opaque->bytes();
    }
    // A GID/QPN/LID or a MAC alone does not describe a native socket address.
    return make_error_result<native_address>(errc::not_supported);
}

[[nodiscard]] inline auto native_address_bytes(const native_address& addr) noexcept
    -> std::span<const std::byte> {
    return std::visit(
        [](const auto& value) -> std::span<const std::byte> {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>,
                                       std::span<const std::byte>>)
                return value;
            else
                return std::as_bytes(std::span{&value, 1});
        },
        addr);
}

// Both CM and AV lookup report the required size. AV lookup may return success
// after truncating, whereas CM reports FI_ETOOSMALL.
template <typename Query>
[[nodiscard]] auto query_address(Query&& query, address_format format) -> result<address> {
    std::array<std::byte, 256> initial{};
    std::vector<std::byte> expanded;
    std::span<std::byte> buffer{initial};
    for (;;) {
        auto length = buffer.size();
        const auto ret = query(buffer.data(), &length);
        if (ret != 0 && (ret != -FI_ETOOSMALL || length <= buffer.size()))
            return make_error_result_from_fi_errno<address>(ret);
        if (length > buffer.size()) {
            expanded.resize(length);
            buffer = expanded;
            continue;
        }
        auto decoded = address_from_raw(buffer.data(), length, format);
        if (std::holds_alternative<unspecified_address>(decoded)) {
            const bool known = is_opaque_address_format(format) || format == address_format::inet ||
                               format == address_format::inet6 || format == address_format::ib ||
                               format == address_format::sockaddr ||
                               format == address_format::sockaddr_ip ||
                               format == address_format::ethernet;
            return make_error_result<address>(known ? errc::invalid_argument : errc::not_supported);
        }
        return decoded;
    }
}

}  // namespace loom::detail
