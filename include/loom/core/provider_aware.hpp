// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>

#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/crtp/remote_memory_base.hpp"
#include "loom/core/endpoint_contract.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @struct provider_remote_memory
 * @brief Provider-specific remote memory descriptor.
 * @tparam Provider The provider tag type.
 */
template <provider_tag Provider>
struct provider_remote_memory : provider_aware_remote_memory_base<provider_remote_memory<Provider>,
                                                                  provider_traits<Provider>> {
    std::uint64_t addr{0};  ///< Remote address
    std::uint64_t key{0};   ///< Remote memory key
    std::size_t length{0};  ///< Memory region length

    /**
     * @brief Default constructor.
     */
    constexpr provider_remote_memory() noexcept = default;

    /**
     * @brief Constructs from address, key, and length.
     * @param addr_ Remote address.
     * @param key_ Remote memory key.
     * @param len_ Region length.
     */
    constexpr provider_remote_memory(std::uint64_t addr_,
                                     std::uint64_t key_,
                                     std::size_t len_) noexcept
        : addr(addr_), key(key_), length(len_) {}

    /**
     * @brief Constructs from a generic remote_memory.
     * @param rm The generic remote memory descriptor.
     */
    explicit provider_remote_memory(const remote_memory& rm) noexcept
        : addr(rm.addr), key(rm.key), length(rm.length) {}

    /**
     * @brief Creates a provider_remote_memory from a memory region.
     * @param mr The memory region.
     * @return A provider-specific remote memory descriptor.
     */
    static auto from_mr(const memory_region& mr) -> result<provider_remote_memory> {
        return remote_memory::from_mr(mr).transform(
            [](const remote_memory& remote) { return provider_remote_memory{remote}; });
    }

    /**
     * @brief Converts to a generic remote_memory.
     * @return The generic remote memory descriptor.
     */
    [[nodiscard]] constexpr auto to_generic() const noexcept -> remote_memory {
        return remote_memory{addr, key, length};
    }
};

/// Compatibility aliases: the provider tag chooses only a preferred bound.
/// No provider identity, native atomic support, or MR policy is inferred.
/// Use contract_endpoint<Profile> to state requirements directly.
template <provider_tag Provider,
          std::size_t InjectBound = provider_traits<Provider>::max_inject_size,
          endpoint_tag Endpoint = rdm_endpoint_tag>
using provider_endpoint = contract_endpoint<message_endpoint_profile<Endpoint, InjectBound>>;

template <provider_tag Provider,
          std::size_t InjectBound = provider_traits<Provider>::max_inject_size,
          endpoint_tag Endpoint = rdm_endpoint_tag>
[[nodiscard]] auto create_provider_endpoint(const domain& dom,
                                            const fabric_info& info,
                                            memory_resource* resource = nullptr)
    -> result<provider_endpoint<Provider, InjectBound, Endpoint>> {
    return provider_endpoint<Provider, InjectBound, Endpoint>::create(dom, info, resource);
}

using verbs_endpoint = provider_endpoint<provider::verbs_tag>;
using efa_endpoint = provider_endpoint<provider::efa_tag>;
using slingshot_endpoint = provider_endpoint<provider::slingshot_tag>;
using shm_endpoint = provider_endpoint<provider::shm_tag>;
using tcp_endpoint = provider_endpoint<provider::tcp_tag>;

using verbs_remote_memory = provider_remote_memory<provider::verbs_tag>;
using efa_remote_memory = provider_remote_memory<provider::efa_tag>;
using slingshot_remote_memory = provider_remote_memory<provider::slingshot_tag>;
using shm_remote_memory = provider_remote_memory<provider::shm_tag>;
using tcp_remote_memory = provider_remote_memory<provider::tcp_tag>;

}  // namespace loom
