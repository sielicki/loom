// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/endpoint.hpp"
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
 * @class address_vector
 * @brief Forward declaration of address_vector class.
 */
class address_vector;

/**
 * @namespace loom::detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct scalable_endpoint_impl
 * @brief Internal implementation for scalable_endpoint.
 */
struct scalable_endpoint_impl;
}  // namespace detail

/**
 * @class scalable_endpoint
 * @brief Scalable endpoint for multi-context communication.
 *
 * Scalable endpoints provide multiple transmit and receive contexts
 * for high-performance parallel communication patterns.
 */
class scalable_endpoint : public fabric_object_base<scalable_endpoint> {
public:
    using impl_ptr =
        detail::pmr_unique_ptr<detail::scalable_endpoint_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty scalable endpoint.
     */
    scalable_endpoint() = default;

    /**
     * @brief Constructs from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit scalable_endpoint(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~scalable_endpoint();

    /**
     * @brief Deleted copy constructor.
     */
    scalable_endpoint(const scalable_endpoint&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const scalable_endpoint&) -> scalable_endpoint& = delete;
    /**
     * @brief Move constructor.
     */
    scalable_endpoint(scalable_endpoint&&) noexcept;
    auto operator=(scalable_endpoint&&) noexcept -> scalable_endpoint&;

    [[nodiscard]] static auto
    create(const domain& dom, const fabric_info& info, memory_resource* resource = nullptr)
        -> result<scalable_endpoint>;

    [[nodiscard]] auto bind_av(address_vector& av, std::uint64_t flags = 0) -> void_result;

    [[nodiscard]] auto enable() -> void_result;

    [[nodiscard]] auto tx_context(std::size_t index) -> result<endpoint>;

    [[nodiscard]] auto rx_context(std::size_t index) -> result<endpoint>;

    [[nodiscard]] auto tx_context_count() const noexcept -> std::size_t;

    [[nodiscard]] auto rx_context_count() const noexcept -> std::size_t;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class address_vector;
};

}  // namespace loom
