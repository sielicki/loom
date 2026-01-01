// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <memory>
#include <memory_resource>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/progress_policy.hpp"
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
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct domain_impl
 * @brief Internal implementation for domain.
 */
struct domain_impl;
}  // namespace detail

/**
 * @class domain
 * @brief Represents a fabric domain for resource management.
 *
 * A domain defines the boundary for fabric resources and operations.
 * All endpoints, memory regions, and completion queues are created
 * within a domain context.
 */
class domain : public fabric_object_base<domain> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::domain_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty domain.
     */
    domain() = default;

    /**
     * @brief Constructs a domain from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit domain(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~domain();

    /**
     * @brief Deleted copy constructor.
     */
    domain(const domain&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const domain&) -> domain& = delete;
    /**
     * @brief Move constructor.
     */
    domain(domain&&) noexcept;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(domain&&) noexcept -> domain&;

    [[nodiscard]] static auto create(const fabric& fab,
                                     const fabric_info& info,
                                     memory_resource* resource = nullptr) -> result<domain>;

    [[nodiscard]] auto get_name() const -> std::string_view;
    [[nodiscard]] auto get_threading() const -> threading_mode;
    [[nodiscard]] auto get_control_progress() const -> progress_mode;
    [[nodiscard]] auto get_data_progress() const -> progress_mode;

    [[nodiscard]] auto progress_policy() const noexcept -> runtime_progress_policy;

    [[nodiscard]] auto bind_eq(event_queue& eq, std::uint64_t flags = 0) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class endpoint;
    friend class completion_queue;
    friend class event_queue;
};

}  // namespace loom
