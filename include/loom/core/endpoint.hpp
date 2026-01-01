// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <memory>
#include <memory_resource>
#include <span>

#include <sys/uio.h>

#include "loom/core/address.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/endpoint_types.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class domain
 * @brief Forward declaration of domain class.
 */
class domain;
/**
 * @class fabric_info
 * @brief Forward declaration of fabric_info class.
 */
class fabric_info;
/**
 * @class completion_queue
 * @brief Forward declaration of completion_queue class.
 */
class completion_queue;
/**
 * @class event_queue
 * @brief Forward declaration of event_queue class.
 */
class event_queue;
/**
 * @class address_vector
 * @brief Forward declaration of address_vector class.
 */
class address_vector;
/**
 * @class counter
 * @brief Forward declaration of counter class.
 */
class counter;
/**
 * @class shared_tx_context
 * @brief Forward declaration of shared_tx_context class.
 */
class shared_tx_context;
/**
 * @class shared_rx_context
 * @brief Forward declaration of shared_rx_context class.
 */
class shared_rx_context;

/**
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct endpoint_impl
 * @brief Internal implementation for endpoint.
 */
struct endpoint_impl;
}  // namespace detail

/**
 * @class endpoint
 * @brief Represents a communication endpoint for fabric operations.
 *
 * An endpoint is the fundamental communication abstraction in libfabric.
 * It provides interfaces for sending and receiving messages, performing
 * RMA operations, and managing connections.
 */
class endpoint : public fabric_object_base<endpoint> {
public:
    using impl_ptr =
        detail::pmr_unique_ptr<detail::endpoint_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty endpoint.
     */
    endpoint() = default;

    /**
     * @brief Constructs an endpoint from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit endpoint(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~endpoint();

    /**
     * @brief Deleted copy constructor.
     */
    endpoint(const endpoint&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const endpoint&) -> endpoint& = delete;
    /**
     * @brief Move constructor.
     */
    endpoint(endpoint&&) noexcept;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(endpoint&&) noexcept -> endpoint&;

    [[nodiscard]] static auto create(const domain& dom,
                                     const fabric_info& info,
                                     memory_resource* resource = nullptr) -> result<endpoint>;

    [[nodiscard]] auto enable() -> void_result;

    [[nodiscard]] auto get_type() const -> result<endpoint_type>;

    [[nodiscard]] auto send(std::span<const std::byte> data, context_ptr<void> ctx = {})
        -> void_result;

    [[nodiscard]] auto recv(std::span<std::byte> buffer, context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto sendv(std::span<const iovec> iov, context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto recvv(std::span<const iovec> iov, context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto tagged_send(std::span<const std::byte> data,
                                   std::uint64_t tag,
                                   context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto tagged_recv(std::span<std::byte> buffer,
                                   std::uint64_t tag,
                                   std::uint64_t ignore = 0,
                                   context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto inject(std::span<const std::byte> data,
                              fabric_addr dest_addr = fabric_addrs::unspecified) -> void_result;

    [[nodiscard]] auto inject_data(std::span<const std::byte> data,
                                   std::uint64_t immediate_data,
                                   fabric_addr dest_addr = fabric_addrs::unspecified)
        -> void_result;

    [[nodiscard]] auto tagged_inject(std::span<const std::byte> data,
                                     std::uint64_t tag,
                                     fabric_addr dest_addr = fabric_addrs::unspecified)
        -> void_result;

    [[nodiscard]] auto tagged_inject_data(std::span<const std::byte> data,
                                          std::uint64_t tag,
                                          std::uint64_t immediate_data,
                                          fabric_addr dest_addr = fabric_addrs::unspecified)
        -> void_result;

    [[nodiscard]] auto cancel(context_ptr<void> ctx) -> void_result;

    [[nodiscard]] auto read(std::span<std::byte> local_buffer,
                            rma_addr remote_addr,
                            mr_key key,
                            context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto write(std::span<const std::byte> local_buffer,
                             rma_addr remote_addr,
                             mr_key key,
                             context_ptr<void> ctx = {}) -> void_result;

    [[nodiscard]] auto connect(const address& peer_addr) -> void_result;
    [[nodiscard]] auto accept() -> void_result;

    [[nodiscard]] auto get_local_address() const -> result<address>;
    [[nodiscard]] auto get_peer_address() const -> result<address>;

    [[nodiscard]] auto bind_cq(completion_queue& cq, cq_bind_flags flags) -> void_result;
    [[nodiscard]] auto bind_tx_cq(completion_queue& cq) -> void_result;
    [[nodiscard]] auto bind_rx_cq(completion_queue& cq) -> void_result;
    [[nodiscard]] auto bind_eq(event_queue& eq, std::uint64_t flags = 0) -> void_result;
    [[nodiscard]] auto bind_av(address_vector& av, std::uint64_t flags = 0) -> void_result;
    [[nodiscard]] auto bind_counter(counter& cntr, std::uint64_t flags = 0) -> void_result;
    [[nodiscard]] auto bind_stx(shared_tx_context& stx) -> void_result;
    [[nodiscard]] auto bind_srx(shared_rx_context& srx) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class completion_queue;
    friend class event_queue;
};

}  // namespace loom
