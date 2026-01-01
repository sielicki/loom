// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "loom/core/concepts/capabilities.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept has_static_capabilities
 * @brief Concept for protocols with compile-time capability flags.
 * @tparam P The protocol type.
 *
 * Requires supports_rma, supports_tagged, supports_atomic, supports_inject.
 */
template <typename P>
concept has_static_capabilities = requires {
    { P::supports_rma } -> std::convertible_to<bool>;
    { P::supports_tagged } -> std::convertible_to<bool>;
    { P::supports_atomic } -> std::convertible_to<bool>;
    { P::supports_inject } -> std::convertible_to<bool>;
};

/**
 * @concept has_runtime_capabilities
 * @brief Concept for protocols with runtime capability queries.
 * @tparam P The protocol type.
 *
 * Requires runtime capability query methods.
 */
template <typename P>
concept has_runtime_capabilities = requires(const P& p) {
    { p.supports_rma() } noexcept -> std::same_as<bool>;
    { p.supports_tagged() } noexcept -> std::same_as<bool>;
    { p.supports_atomic() } noexcept -> std::same_as<bool>;
};

/**
 * @concept named_protocol
 * @brief Concept for protocols with a name accessor.
 * @tparam P The protocol type.
 */
template <typename P>
concept named_protocol = requires(const P& p) {
    { p.name() } noexcept -> std::convertible_to<const char*>;
};

/**
 * @concept basic_protocol
 * @brief Concept for basic protocols with messaging.
 * @tparam P The protocol type.
 *
 * Requires name and message capabilities.
 */
template <typename P>
concept basic_protocol = named_protocol<P> && msg_capable<P>;

/**
 * @concept tagged_protocol
 * @brief Concept for protocols with tagged messaging.
 * @tparam P The protocol type.
 */
template <typename P>
concept tagged_protocol = basic_protocol<P> && tagged_capable<P>;

/**
 * @concept rma_protocol
 * @brief Concept for protocols with RMA capabilities.
 * @tparam P The protocol type.
 */
template <typename P>
concept rma_protocol = basic_protocol<P> && rma_capable<P>;

/**
 * @concept full_protocol
 * @brief Concept for protocols with all capabilities.
 * @tparam P The protocol type.
 */
template <typename P>
concept full_protocol = tagged_protocol<P> && rma_protocol<P>;

/**
 * @concept provider_aware_protocol
 * @brief Concept for protocols associated with a provider.
 * @tparam P The protocol type.
 *
 * Requires provider_type alias.
 */
template <typename P>
concept provider_aware_protocol = requires {
    typename P::provider_type;
    requires provider_tag<typename P::provider_type>;
};

/**
 * @concept statically_typed_protocol
 * @brief Concept for statically typed protocols.
 * @tparam P The protocol type.
 *
 * Requires static capabilities and provider awareness.
 */
template <typename P>
concept statically_typed_protocol = has_static_capabilities<P> && provider_aware_protocol<P>;

/**
 * @concept protocol_for_provider
 * @brief Concept for protocols matching a specific provider.
 * @tparam P The protocol type.
 * @tparam Provider The provider type.
 */
template <typename P, typename Provider>
concept protocol_for_provider = provider_tag<Provider> && provider_aware_protocol<P> &&
                                std::same_as<typename P::provider_type, Provider>;

/**
 * @concept rma_enabled_protocol
 * @brief Concept for protocols with RMA enabled.
 * @tparam P The protocol type.
 */
template <typename P>
concept rma_enabled_protocol =
    rma_protocol<P> && has_static_capabilities<P> && requires { requires P::supports_rma; };

/**
 * @concept atomic_enabled_protocol
 * @brief Concept for protocols with atomic operations enabled.
 * @tparam P The protocol type.
 */
template <typename P>
concept atomic_enabled_protocol = has_static_capabilities<P> && requires {
    requires P::supports_atomic;
    requires provider_aware_protocol<P>;
    requires provider_traits<typename P::provider_type>::supports_native_atomics ||
                 provider_traits<typename P::provider_type>::uses_staged_atomics;
};

/**
 * @concept inject_enabled_protocol
 * @brief Concept for protocols with inject operations enabled.
 * @tparam P The protocol type.
 */
template <typename P>
concept inject_enabled_protocol =
    inject_capable<P> && has_static_capabilities<P> && requires { requires P::supports_inject; };

/**
 * @struct protocol_capabilities
 * @brief Compile-time protocol capability information.
 * @tparam P The protocol type.
 */
template <typename P>
struct protocol_capabilities {
    static constexpr bool has_rma = rma_protocol<P>;        ///< True if RMA capable
    static constexpr bool has_tagged = tagged_protocol<P>;  ///< True if tagged capable
    static constexpr bool has_inject = inject_capable<P>;   ///< True if inject capable

    static constexpr bool static_rma = has_static_capabilities<P> ? P::supports_rma : has_rma;
    static constexpr bool static_tagged =
        has_static_capabilities<P> ? P::supports_tagged : has_tagged;
    static constexpr bool static_atomic = has_static_capabilities<P> ? P::supports_atomic : false;
    static constexpr bool static_inject =
        has_static_capabilities<P> ? P::supports_inject : has_inject;
};

/**
 * @typedef protocol_provider_t
 * @brief Extracts the provider type from a protocol.
 * @tparam P The protocol type.
 */
template <typename P>
    requires provider_aware_protocol<P>
using protocol_provider_t = typename P::provider_type;

/**
 * @typedef protocol_traits_t
 * @brief Gets provider traits for a protocol's provider.
 * @tparam P The protocol type.
 */
template <typename P>
    requires provider_aware_protocol<P>
using protocol_traits_t = provider_traits<protocol_provider_t<P>>;

/**
 * @var protocol_max_inject_size
 * @brief Maximum inject size for a protocol.
 * @tparam P The protocol type.
 */
template <typename P>
    requires provider_aware_protocol<P>
inline constexpr std::size_t protocol_max_inject_size = protocol_traits_t<P>::max_inject_size;

/**
 * @brief Checks if a size can be injected for a protocol.
 * @tparam P The protocol type.
 * @param size The message size.
 * @return True if the size can be injected.
 */
template <typename P>
    requires provider_aware_protocol<P>
[[nodiscard]] constexpr auto protocol_can_inject(std::size_t size) noexcept -> bool {
    return protocol_traits_t<P>::supports_inject && size <= protocol_max_inject_size<P>;
}

/**
 * @concept protocol_with_capabilities
 * @brief Concept for protocols with specific capabilities.
 * @tparam Protocol The protocol type.
 * @tparam RequiredCaps The required capability checkers.
 */
template <typename Protocol, typename... RequiredCaps>
concept protocol_with_capabilities = (... && RequiredCaps::template check<Protocol>);

/**
 * @struct requires_rma
 * @brief Capability checker for RMA requirement.
 */
struct requires_rma {
    /**
     * @var check
     * @brief True if protocol has RMA capability.
     */
    template <typename P>
    static constexpr bool check = rma_protocol<P>;
};

/**
 * @struct requires_tagged
 * @brief Capability checker for tagged messaging requirement.
 */
struct requires_tagged {
    /**
     * @var check
     * @brief True if protocol has tagged capability.
     */
    template <typename P>
    static constexpr bool check = tagged_protocol<P>;
};

/**
 * @struct requires_inject
 * @brief Capability checker for inject requirement.
 */
struct requires_inject {
    /**
     * @var check
     * @brief True if protocol has inject capability.
     */
    template <typename P>
    static constexpr bool check = inject_capable<P>;
};

/**
 * @struct requires_atomic
 * @brief Capability checker for atomic requirement.
 */
struct requires_atomic {
    /**
     * @var check
     * @brief True if protocol has atomic capability.
     */
    template <typename P>
    static constexpr bool check = atomic_enabled_protocol<P>;
};

}  // namespace loom
