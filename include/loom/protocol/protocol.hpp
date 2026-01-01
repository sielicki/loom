// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "loom/core/concepts/capabilities.hpp"
#include "loom/core/concepts/protocol_concepts.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::protocol
 * @brief Protocol abstraction layer for fabric communication.
 */
namespace loom::protocol {

using loom::basic_protocol;
using loom::context_ptr;
using loom::fabric_addr;
using loom::has_runtime_capabilities;
using loom::has_static_capabilities;
using loom::inject_capable;
using loom::mr_key;
using loom::provider_tag;
using loom::rma_addr;
using loom::rma_protocol;
using loom::tagged_protocol;
using loom::void_result;
using namespace loom::fabric_addrs;

/**
 * @struct overloads
 * @brief Helper for creating overload sets from lambdas.
 * @tparam Ts The lambda types to combine.
 */
template <typename... Ts>
struct overloads : Ts... {
    using Ts::operator()...;
};

/**
 * @brief Deduction guide for overloads.
 */
template <typename... Ts>
overloads(Ts...) -> overloads<Ts...>;

/**
 * @brief Visits a variant with multiple visitor lambdas.
 * @tparam Variant The variant type.
 * @tparam Visitors The visitor lambda types.
 * @param var The variant to visit.
 * @param visitors The visitor lambdas.
 * @return The result of the matching visitor.
 */
template <typename Variant, typename... Visitors>
[[nodiscard]] constexpr auto visit(Variant&& var, Visitors&&... visitors) {
    return std::visit(overloads{std::forward<Visitors>(visitors)...}, std::forward<Variant>(var));
}

/**
 * @brief Sends data using a protocol.
 * @tparam Protocol The protocol type (must satisfy basic_protocol).
 * @param proto The protocol instance.
 * @param data The data to send.
 * @param ctx Optional context pointer.
 * @return Success or error result.
 */
template <typename Protocol>
    requires basic_protocol<Protocol>
[[nodiscard]] auto
send(Protocol& proto, std::span<const std::byte> data, context_ptr<void> ctx = {}) -> void_result {
    return proto.send(data, ctx);
}

/**
 * @brief Receives data using a protocol.
 * @tparam Protocol The protocol type (must satisfy basic_protocol).
 * @param proto The protocol instance.
 * @param buffer The buffer to receive into.
 * @param ctx Optional context pointer.
 * @return Success or error result.
 */
template <typename Protocol>
    requires basic_protocol<Protocol>
[[nodiscard]] auto recv(Protocol& proto, std::span<std::byte> buffer, context_ptr<void> ctx = {})
    -> void_result {
    return proto.recv(buffer, ctx);
}

/**
 * @brief Sends tagged data using a protocol.
 * @tparam Protocol The protocol type (must satisfy tagged_protocol).
 * @param proto The protocol instance.
 * @param data The data to send.
 * @param tag The message tag.
 * @param ctx Optional context pointer.
 * @return Success or error result.
 */
template <typename Protocol>
    requires tagged_protocol<Protocol>
[[nodiscard]] auto tagged_send(Protocol& proto,
                               std::span<const std::byte> data,
                               std::uint64_t tag,
                               context_ptr<void> ctx = {}) -> void_result {
    return proto.tagged_send(data, tag, ctx);
}

/**
 * @brief Receives tagged data using a protocol.
 * @tparam Protocol The protocol type (must satisfy tagged_protocol).
 * @param proto The protocol instance.
 * @param buffer The buffer to receive into.
 * @param tag The expected message tag.
 * @param ignore Tag bits to ignore in matching.
 * @param ctx Optional context pointer.
 * @return Success or error result.
 */
template <typename Protocol>
    requires tagged_protocol<Protocol>
[[nodiscard]] auto tagged_recv(Protocol& proto,
                               std::span<std::byte> buffer,
                               std::uint64_t tag,
                               std::uint64_t ignore = 0,
                               context_ptr<void> ctx = {}) -> void_result {
    return proto.tagged_recv(buffer, tag, ignore, ctx);
}

/**
 * @brief Reads from remote memory using a protocol.
 * @tparam Protocol The protocol type (must satisfy rma_protocol).
 * @param proto The protocol instance.
 * @param local Local buffer to read into.
 * @param remote Remote memory address.
 * @param key Remote memory key.
 * @param ctx Optional context pointer.
 * @return Success or error result.
 */
template <typename Protocol>
    requires rma_protocol<Protocol>
[[nodiscard]] auto read(Protocol& proto,
                        std::span<std::byte> local,
                        rma_addr remote,
                        mr_key key,
                        context_ptr<void> ctx = {}) -> void_result {
    return proto.read(local, remote, key, ctx);
}

/**
 * @brief Writes to remote memory using a protocol.
 * @tparam Protocol The protocol type (must satisfy rma_protocol).
 * @param proto The protocol instance.
 * @param local Local buffer to write from.
 * @param remote Remote memory address.
 * @param key Remote memory key.
 * @param ctx Optional context pointer.
 * @return Success or error result.
 */
template <typename Protocol>
    requires rma_protocol<Protocol>
[[nodiscard]] auto write(Protocol& proto,
                         std::span<const std::byte> local,
                         rma_addr remote,
                         mr_key key,
                         context_ptr<void> ctx = {}) -> void_result {
    return proto.write(local, remote, key, ctx);
}

/**
 * @brief Injects a small message using a protocol.
 * @tparam Protocol The protocol type (must satisfy inject_capable).
 * @param proto The protocol instance.
 * @param data The data to inject.
 * @param dest Destination address.
 * @return Success or error result.
 */
template <typename Protocol>
    requires inject_capable<Protocol>
[[nodiscard]] auto inject(Protocol& proto,
                          std::span<const std::byte> data,
                          fabric_addr dest = fabric_addrs::unspecified) -> void_result {
    return proto.inject(data, dest);
}

/**
 * @typedef protocol_variant
 * @brief A variant type for holding different protocol implementations.
 * @tparam Provider The provider tag type.
 * @tparam Protocols The protocol types in the variant.
 */
template <provider_tag Provider, typename... Protocols>
using protocol_variant = std::variant<Protocols...>;

/**
 * @brief Gets the name of a protocol from a variant.
 * @tparam Variant The variant type.
 * @param var The variant containing a protocol.
 * @return The protocol name.
 */
template <typename Variant>
[[nodiscard]] auto get_name(const Variant& var) noexcept -> const char* {
    return std::visit([](const auto& p) { return p.name(); }, var);
}

/**
 * @brief Checks if a protocol variant supports RMA.
 * @tparam Variant The variant type.
 * @param var The variant containing a protocol.
 * @return True if RMA is supported.
 */
template <typename Variant>
[[nodiscard]] auto supports_rma(const Variant& var) noexcept -> bool {
    return std::visit(
        [](const auto& p) {
            if constexpr (has_runtime_capabilities<std::remove_cvref_t<decltype(p)>>) {
                return p.supports_rma();
            } else if constexpr (has_static_capabilities<std::remove_cvref_t<decltype(p)>>) {
                return std::remove_cvref_t<decltype(p)>::supports_rma;
            } else {
                return rma_protocol<std::remove_cvref_t<decltype(p)>>;
            }
        },
        var);
}

/**
 * @brief Checks if a protocol variant supports tagged messaging.
 * @tparam Variant The variant type.
 * @param var The variant containing a protocol.
 * @return True if tagged messaging is supported.
 */
template <typename Variant>
[[nodiscard]] auto supports_tagged(const Variant& var) noexcept -> bool {
    return std::visit(
        [](const auto& p) {
            if constexpr (has_runtime_capabilities<std::remove_cvref_t<decltype(p)>>) {
                return p.supports_tagged();
            } else if constexpr (has_static_capabilities<std::remove_cvref_t<decltype(p)>>) {
                return std::remove_cvref_t<decltype(p)>::supports_tagged;
            } else {
                return tagged_protocol<std::remove_cvref_t<decltype(p)>>;
            }
        },
        var);
}

}  // namespace loom::protocol
