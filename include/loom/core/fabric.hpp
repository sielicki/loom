// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

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
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct fabric_info_impl
 * @brief Internal implementation for fabric_info.
 */
struct fabric_info_impl;
/**
 * @struct fabric_impl
 * @brief Internal implementation for fabric.
 */
struct fabric_impl;
}  // namespace detail

/**
 * @struct fabric_attributes
 * @brief Attributes describing a fabric instance.
 */
struct fabric_attributes {
    std::string_view name;                                       ///< Fabric name
    std::string_view provider_name;                              ///< Provider name
    fabric_version provider_version{0U};                         ///< Provider version
    threading_mode threading{threading_mode::unspecified};       ///< Threading model
    progress_mode control_progress{progress_mode::unspecified};  ///< Control progress mode
    progress_mode data_progress{progress_mode::unspecified};     ///< Data progress mode
};

/**
 * @struct domain_attributes
 * @brief Attributes describing a domain instance.
 */
struct domain_attributes {
    std::string_view name;                                       ///< Domain name
    threading_mode threading{threading_mode::unspecified};       ///< Threading model
    progress_mode control_progress{progress_mode::unspecified};  ///< Control progress mode
    progress_mode data_progress{progress_mode::unspecified};     ///< Data progress mode
    address_format addr_format{address_format::unspecified};     ///< Address format
    mr_mode memory_region_mode{0ULL};                            ///< Memory region mode flags
    std::size_t max_ep_tx_ctx{0};  ///< Maximum TX contexts per endpoint
    std::size_t max_ep_rx_ctx{0};  ///< Maximum RX contexts per endpoint
};

/**
 * @struct endpoint_attributes
 * @brief Attributes describing an endpoint instance.
 */
struct endpoint_attributes {
    endpoint_type type{endpoint_types::msg};              ///< Endpoint type
    protocol_version protocol{0U};                        ///< Protocol version
    queue_size max_msg_size{0UL};                         ///< Maximum message size
    queue_size tx_ctx_cnt{0UL};                           ///< Transmit context count
    queue_size rx_ctx_cnt{0UL};                           ///< Receive context count
    msg_order msg_ordering{ordering::none};               ///< Message ordering guarantees
    comp_order comp_ordering{completion_ordering::none};  ///< Completion ordering guarantees
};

/**
 * @class fabric_info
 * @brief Represents fabric information returned from a fabric query.
 *
 * fabric_info contains the results of querying available fabrics and their
 * capabilities. It can be iterated to access multiple matching fabrics.
 */
class fabric_info : public fabric_object_base<fabric_info> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::fabric_info_impl>;

    /**
     * @brief Default constructor creating an empty fabric_info.
     */
    fabric_info() = default;

    /**
     * @brief Constructs a fabric_info from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit fabric_info(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~fabric_info();

    /**
     * @brief Deleted copy constructor.
     */
    fabric_info(const fabric_info&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const fabric_info&) -> fabric_info& = delete;
    /**
     * @brief Move constructor.
     */
    fabric_info(fabric_info&&) noexcept;
    auto operator=(fabric_info&&) noexcept -> fabric_info&;

    [[nodiscard]] auto get_fabric_attr() const -> std::optional<fabric_attributes>;
    [[nodiscard]] auto get_domain_attr() const -> std::optional<domain_attributes>;
    [[nodiscard]] auto get_endpoint_attr() const -> std::optional<endpoint_attributes>;
    [[nodiscard]] auto get_caps() const -> caps;
    [[nodiscard]] auto get_mode() const -> mode;
    [[nodiscard]] auto get_address_format() const -> address_format;

    /**
     * @class iterator
     * @brief Iterator for traversing fabric_info results.
     */
    class iterator {
    public:
        using value_type = const fabric_info;
        using reference = value_type&;
        using pointer = value_type*;

        /**
         * @brief Default constructor creating an end iterator.
         */
        iterator() = default;
        /**
         * @brief Constructs an iterator pointing to the given fabric_info.
         * @param info Pointer to the fabric_info to iterate.
         */
        explicit iterator(const fabric_info* info) noexcept : current_(info) {}

        /**
         * @brief Dereferences the iterator.
         * @return Reference to the current fabric_info.
         */
        auto operator*() const -> reference { return *current_; }
        /**
         * @brief Member access operator.
         * @return Pointer to the current fabric_info.
         */
        auto operator->() const -> pointer { return current_; }
        auto operator++() -> iterator&;
        auto operator++(int) -> iterator;
        /**
         * @brief Equality comparison.
         */
        auto operator==(const iterator&) const -> bool = default;

    private:
        const fabric_info* current_{nullptr};
    };

    [[nodiscard]] auto begin() const -> iterator;
    [[nodiscard]] auto end() const -> iterator;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class fabric;
    friend class domain;
    friend class endpoint;
};

/**
 * @struct fabric_hints
 * @brief Hints for querying available fabrics.
 *
 * Used to filter the results of query_fabric() based on desired capabilities
 * and requirements.
 */
struct fabric_hints {
    caps capabilities{0ULL};                                  ///< Required capabilities
    mode mode_bits{0ULL};                                     ///< Required mode bits
    address_format addr_format{address_format::unspecified};  ///< Preferred address format
    endpoint_type ep_type{endpoint_types::msg};               ///< Preferred endpoint type
    std::optional<std::string_view> fabric_name;              ///< Specific fabric name
    std::optional<std::string_view> domain_name;              ///< Specific domain name
    std::optional<std::string_view> provider_name;            ///< Specific provider name
    std::optional<threading_mode> threading;                  ///< Required threading mode
    std::optional<progress_mode> control_progress;            ///< Required control progress mode
    std::optional<progress_mode> data_progress;               ///< Required data progress mode
    std::optional<mr_mode> memory_region_mode;                ///< Required memory region mode
};

/**
 * @class fabric
 * @brief Represents a fabric instance for communication.
 *
 * A fabric represents a collection of hardware and software resources that
 * provide communication services. It is created from fabric_info and serves
 * as the parent for domains.
 */
class fabric : public fabric_object_base<fabric> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::fabric_impl>;

    /**
     * @brief Default constructor creating an empty fabric.
     */
    fabric() = default;

    /**
     * @brief Constructs a fabric from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit fabric(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~fabric();

    /**
     * @brief Deleted copy constructor.
     */
    fabric(const fabric&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const fabric&) -> fabric& = delete;
    /**
     * @brief Move constructor.
     */
    fabric(fabric&&) noexcept;
    auto operator=(fabric&&) noexcept -> fabric&;

    [[nodiscard]] static auto create(const fabric_info& info, memory_resource* resource = nullptr)
        -> result<fabric>;

    [[nodiscard]] auto get_name() const -> std::string_view;
    [[nodiscard]] auto get_provider_name() const -> std::string_view;
    [[nodiscard]] auto get_provider_version() const -> fabric_version;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class domain;
};

/**
 * @brief Queries available fabrics matching the given hints.
 * @param hints Optional hints to filter results.
 * @param resource Optional memory resource for allocations.
 * @return Result containing fabric_info or an error.
 */
[[nodiscard]] auto query_fabric(const fabric_hints& hints = {}, memory_resource* resource = nullptr)
    -> result<fabric_info>;

/**
 * @brief Gets the libfabric version.
 * @return The libfabric version number.
 */
[[nodiscard]] auto get_fabric_version() noexcept -> fabric_version;

}  // namespace loom
