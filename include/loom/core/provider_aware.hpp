// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/crtp/remote_memory_base.hpp"
#include "loom/core/endpoint.hpp"
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
    static auto from_mr(const memory_region& mr) -> provider_remote_memory {
        return provider_remote_memory{
            reinterpret_cast<std::uint64_t>(mr.address()), mr.key(), mr.length()};
    }

    /**
     * @brief Converts to a generic remote_memory.
     * @return The generic remote memory descriptor.
     */
    [[nodiscard]] constexpr auto to_generic() const noexcept -> remote_memory {
        return remote_memory{addr, key, length};
    }
};

/**
 * @class provider_endpoint
 * @brief Provider-specific endpoint wrapper with compile-time provider traits.
 * @tparam Provider The provider tag type.
 */
template <provider_tag Provider>
class provider_endpoint {
public:
    using provider_type = Provider;                               ///< Provider tag type
    using traits_type = provider_traits<Provider>;                ///< Provider traits
    using remote_memory_type = provider_remote_memory<Provider>;  ///< Remote memory type

    /**
     * @brief Constructs from an endpoint.
     * @param ep The underlying endpoint.
     */
    explicit provider_endpoint(endpoint ep) noexcept : ep_(std::move(ep)) {}

    /**
     * @brief Default constructor.
     */
    provider_endpoint() = default;
    /**
     * @brief Destructor.
     */
    ~provider_endpoint() = default;
    /**
     * @brief Move constructor.
     */
    provider_endpoint(provider_endpoint&&) noexcept = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(provider_endpoint&&) noexcept -> provider_endpoint& = default;
    /**
     * @brief Deleted copy constructor.
     */
    provider_endpoint(const provider_endpoint&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const provider_endpoint&) -> provider_endpoint& = delete;

    /**
     * @brief Enables the endpoint for communication.
     * @return Success or error result.
     */
    [[nodiscard]] auto enable() -> void_result { return ep_.enable(); }

    /**
     * @brief Sends data on the endpoint.
     * @param data Data to send.
     * @param ctx Optional context pointer.
     * @return Success or error result.
     */
    [[nodiscard]] auto send(std::span<const std::byte> data, context_ptr<void> ctx = {})
        -> void_result {
        return ep_.send(data, ctx);
    }

    /**
     * @brief Receives data on the endpoint.
     * @param buffer Buffer to receive into.
     * @param ctx Optional context pointer.
     * @return Success or error result.
     */
    [[nodiscard]] auto recv(std::span<std::byte> buffer, context_ptr<void> ctx = {})
        -> void_result {
        return ep_.recv(buffer, ctx);
    }

    /**
     * @brief Injects a small message.
     * @param data Data to inject.
     * @param dest_addr Destination address.
     * @return Success or error result.
     */
    [[nodiscard]] auto inject(std::span<const std::byte> data,
                              fabric_addr dest_addr = fabric_addrs::unspecified) -> void_result
        requires inject_capable_provider<Provider>
    {
        if constexpr (traits_type::max_inject_size > 0) {
            if (data.size() > traits_type::max_inject_size) {
                return make_error_result<void>(errc::message_too_long);
            }
        }
        return ep_.inject(data, dest_addr);
    }

    /**
     * @brief Checks if a message size can be injected.
     * @param size Size to check.
     * @return True if the size can be injected.
     */
    [[nodiscard]] static constexpr auto can_inject(std::size_t size) noexcept -> bool {
        return loom::can_inject<Provider>(size);
    }

    /**
     * @brief Gets the maximum inject size for this provider.
     * @return Maximum inject size in bytes.
     */
    [[nodiscard]] static constexpr auto max_inject_size() noexcept -> std::size_t {
        return traits_type::max_inject_size;
    }

    /**
     * @brief Checks if the provider supports native atomics.
     * @return True if native atomics are supported.
     */
    [[nodiscard]] static constexpr auto supports_native_atomics() noexcept -> bool {
        return traits_type::supports_native_atomics;
    }

    /**
     * @brief Checks if the provider requires staged atomics.
     * @return True if staged atomics are required.
     */
    [[nodiscard]] static constexpr auto requires_staged_atomics() noexcept -> bool {
        return traits_type::uses_staged_atomics;
    }

    /**
     * @brief Gets the provider name.
     * @return The provider name string.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* {
        return traits_type::provider_name();
    }

    /**
     * @brief Cancels an operation.
     * @param ctx Context of the operation to cancel.
     * @return Success or error result.
     */
    [[nodiscard]] auto cancel(context_ptr<void> ctx) -> void_result { return ep_.cancel(ctx); }

    /**
     * @brief Gets the local address of the endpoint.
     * @return The local address or error.
     */
    [[nodiscard]] auto get_local_address() const -> result<address> {
        return ep_.get_local_address();
    }

    /**
     * @brief Gets the peer address of the endpoint.
     * @return The peer address or error.
     */
    [[nodiscard]] auto get_peer_address() const -> result<address> {
        return ep_.get_peer_address();
    }

    /**
     * @brief Binds a completion queue to the endpoint.
     * @param cq The completion queue.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_cq(completion_queue& cq, cq_bind_flags flags) -> void_result {
        return ep_.bind_cq(cq, flags);
    }

    /**
     * @brief Binds a transmit completion queue.
     * @param cq The completion queue.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_tx_cq(completion_queue& cq) -> void_result {
        return ep_.bind_tx_cq(cq);
    }

    /**
     * @brief Binds a receive completion queue.
     * @param cq The completion queue.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_rx_cq(completion_queue& cq) -> void_result {
        return ep_.bind_rx_cq(cq);
    }

    /**
     * @brief Binds an event queue to the endpoint.
     * @param eq The event queue.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_eq(event_queue& eq, std::uint64_t flags = 0) -> void_result {
        return ep_.bind_eq(eq, flags);
    }

    /**
     * @brief Binds an address vector to the endpoint.
     * @param av The address vector.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_av(address_vector& av, std::uint64_t flags = 0) -> void_result {
        return ep_.bind_av(av, flags);
    }

    /**
     * @brief Binds a counter to the endpoint.
     * @param cntr The counter.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_counter(counter& cntr, std::uint64_t flags = 0) -> void_result {
        return ep_.bind_counter(cntr, flags);
    }

    /**
     * @brief Gets the underlying endpoint.
     * @return Reference to the underlying endpoint.
     */
    [[nodiscard]] auto underlying() noexcept -> endpoint& { return ep_; }
    /**
     * @brief Gets the underlying endpoint (const).
     * @return Const reference to the underlying endpoint.
     */
    [[nodiscard]] auto underlying() const noexcept -> const endpoint& { return ep_; }

    /**
     * @brief Checks if the endpoint is valid.
     * @return True if valid.
     */
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(ep_); }

private:
    endpoint ep_;  ///< Underlying endpoint
};

/**
 * @brief Creates a provider-specific endpoint from a generic endpoint.
 * @tparam Provider The provider tag type.
 * @param ep The generic endpoint to wrap.
 * @return A provider-specific endpoint wrapper.
 */
template <provider_tag Provider>
[[nodiscard]] auto make_provider_endpoint(endpoint ep) -> provider_endpoint<Provider> {
    return provider_endpoint<Provider>{std::move(ep)};
}

/**
 * @brief Creates a provider-specific endpoint from domain and fabric info.
 * @tparam Provider The provider tag type.
 * @param dom The fabric domain.
 * @param info The fabric info for endpoint creation.
 * @param resource Optional memory resource for allocation.
 * @return A provider-specific endpoint or error.
 */
template <provider_tag Provider>
[[nodiscard]] auto create_provider_endpoint(const domain& dom,
                                            const fabric_info& info,
                                            memory_resource* resource = nullptr)
    -> result<provider_endpoint<Provider>> {
    auto ep_result = endpoint::create(dom, info, resource);
    if (!ep_result) {
        return make_error_result<provider_endpoint<Provider>>(ep_result.error());
    }
    return provider_endpoint<Provider>{std::move(*ep_result)};
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
