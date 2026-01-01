// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <memory>
#include <memory_resource>

#include "loom/core/address.hpp"
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
 * @class fabric
 * @brief Forward declaration of fabric class.
 */
class fabric;
/**
 * @class fabric_info
 * @brief Forward declaration of fabric_info class.
 */
class fabric_info;
/**
 * @class event_queue
 * @brief Forward declaration of event_queue class.
 */
class event_queue;

/**
 * @namespace loom::detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct passive_endpoint_impl
 * @brief Internal implementation for passive_endpoint.
 */
struct passive_endpoint_impl;
}  // namespace detail

/**
 * @class passive_endpoint
 * @brief Passive endpoint for accepting incoming connections.
 *
 * Passive endpoints are used by server applications to listen for
 * and accept incoming connection requests.
 */
class passive_endpoint : public fabric_object_base<passive_endpoint> {
public:
    using impl_ptr =
        detail::pmr_unique_ptr<detail::passive_endpoint_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty passive endpoint.
     */
    passive_endpoint() = default;

    /**
     * @brief Constructs from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit passive_endpoint(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~passive_endpoint();

    /**
     * @brief Deleted copy constructor.
     */
    passive_endpoint(const passive_endpoint&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const passive_endpoint&) -> passive_endpoint& = delete;
    /**
     * @brief Move constructor.
     */
    passive_endpoint(passive_endpoint&&) noexcept;
    auto operator=(passive_endpoint&&) noexcept -> passive_endpoint&;

    [[nodiscard]] static auto
    create(const fabric& fab, const fabric_info& info, memory_resource* resource = nullptr)
        -> result<passive_endpoint>;

    [[nodiscard]] auto bind_eq(event_queue& eq, std::uint64_t flags = 0) -> void_result;

    [[nodiscard]] auto listen() -> void_result;

    [[nodiscard]] auto get_local_address() const -> result<address>;

    [[nodiscard]] auto reject(const void* handle) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class event_queue;
};

}  // namespace loom
