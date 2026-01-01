// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

class domain;            ///< Forward declaration
class fabric_info;       ///< Forward declaration
class completion_queue;  ///< Forward declaration
class counter;           ///< Forward declaration

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
    shared_tx_context() = default;

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
 */
class shared_rx_context : public fabric_object_base<shared_rx_context> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::shared_rx_context_impl>;

    /**
     * @brief Default constructor creating an empty shared_rx_context.
     */
    shared_rx_context() = default;

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

    [[nodiscard]] auto bind_cq(completion_queue& cq, cq_bind_flags flags) -> void_result;
    [[nodiscard]] auto bind_counter(counter& cntr, std::uint64_t flags = 0) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
};

}  // namespace loom
