// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>

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

class domain;       ///< Forward declaration
class fabric_info;  ///< Forward declaration

/**
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct shared_tx_context_impl
 * @brief Internal implementation for shared_tx_context.
 */
struct shared_tx_context_impl;
/**
 * @struct shared_rx_context_impl
 * @brief Internal implementation for shared_rx_context.
 */
struct shared_rx_context_impl;
}  // namespace detail

/**
 * @class shared_tx_context
 * @brief Shared transmit context for multiple endpoints.
 *
 * A shared transmit context (STX) allows multiple endpoints to share
 * transmit resources, reducing memory usage when many endpoints are needed.
 */
class shared_tx_context : public fabric_object_base<shared_tx_context> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::shared_tx_context_impl>;

    /**
     * @brief Default constructor creating an empty shared_tx_context.
     */
    shared_tx_context();

    /**
     * @brief Constructs a shared_tx_context from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit shared_tx_context(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~shared_tx_context();

    /**
     * @brief Deleted copy constructor.
     */
    shared_tx_context(const shared_tx_context&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const shared_tx_context&) -> shared_tx_context& = delete;
    /**
     * @brief Move constructor.
     */
    shared_tx_context(shared_tx_context&&) noexcept;
    auto operator=(shared_tx_context&&) noexcept -> shared_tx_context&;

    [[nodiscard]] static auto
    create(const domain& dom, const fabric_info& info, memory_resource* resource = nullptr)
        -> result<shared_tx_context>;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
};

/**
 * @class shared_rx_context
 * @brief Shared receive context for multiple endpoints.
 *
 * A shared receive context (SRX) allows multiple endpoints to share
 * receive resources, reducing memory usage when many endpoints are needed.
 * Submit receives through the core messaging helpers. Completions belong to
 * the endpoint that receives the message, using that endpoint's CQ/counters.
 * All bound connectionless endpoints must share one address vector.
 * Close bound endpoints before closing this context; keep receive buffers,
 * registrations, and provider context storage alive until their operations
 * retire or the context closes successfully. The domain must outlive it.
 */
class shared_rx_context : public fabric_object_base<shared_rx_context> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::shared_rx_context_impl>;

    /**
     * @brief Default constructor creating an empty shared_rx_context.
     */
    shared_rx_context();

    /**
     * @brief Constructs a shared_rx_context from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit shared_rx_context(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~shared_rx_context();

    /**
     * @brief Deleted copy constructor.
     */
    shared_rx_context(const shared_rx_context&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const shared_rx_context&) -> shared_rx_context& = delete;
    /**
     * @brief Move constructor.
     */
    shared_rx_context(shared_rx_context&&) noexcept;
    auto operator=(shared_rx_context&&) noexcept -> shared_rx_context&;

    [[nodiscard]] static auto
    create(const domain& dom, const fabric_info& info, memory_resource* resource = nullptr)
        -> result<shared_rx_context>;

    /// Request cancellation; success alone does not retire the receive storage.
    [[nodiscard]] auto cancel(context_ptr<void> context) -> void_result;
    /// Close after bound endpoints; failure retains the native handle for retry.
    [[nodiscard]] auto close() -> void_result;

    /// Negotiated receive vector count; other directions and missing metadata are zero.
    [[nodiscard]] auto iov_limits() const noexcept -> endpoint_iov_limits;

    /// Native receive defaults captured at creation; transmit defaults are zero.
    [[nodiscard]] auto impl_operation_flags(completion_direction direction) const noexcept
        -> std::uint64_t;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
};

}  // namespace loom
