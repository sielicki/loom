// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/fabric.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>

#include <string>
#include <utility>

#include "loom/core/allocator.hpp"
#include "loom/detail/conversions.hpp"

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
struct fabric_info_impl {
    ::fi_info* info{nullptr};  ///< Libfabric info structure

    /**
     * @brief Default constructor.
     */
    fabric_info_impl() = default;

    /**
     * @brief Constructs implementation from libfabric info.
     * @param fi The libfabric info structure.
     */
    explicit fabric_info_impl(::fi_info* fi) : info(fi) {}

    /**
     * @brief Destructor that frees the libfabric info.
     */
    ~fabric_info_impl() {
        if (info) {
            ::fi_freeinfo(info);
        }
    }

    /**
     * @brief Deleted copy constructor.
     */
    fabric_info_impl(const fabric_info_impl&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const fabric_info_impl&) -> fabric_info_impl& = delete;
    /**
     * @brief Deleted move constructor.
     */
    fabric_info_impl(fabric_info_impl&&) = delete;
    /**
     * @brief Deleted move assignment operator.
     */
    auto operator=(fabric_info_impl&&) -> fabric_info_impl& = delete;
};

/**
 * @struct fabric_impl
 * @brief Internal implementation for fabric.
 */
struct fabric_impl {
    ::fid_fabric* fabric{nullptr};      ///< Libfabric fabric handle
    std::string name;                   ///< Fabric name
    std::string provider_name;          ///< Provider name
    std::uint32_t provider_version{0};  ///< Provider version

    /**
     * @brief Default constructor.
     */
    fabric_impl() = default;

    /**
     * @brief Constructs implementation from libfabric fabric and attributes.
     * @param fab The libfabric fabric handle.
     * @param attr The fabric attributes.
     */
    fabric_impl(::fid_fabric* fab, const ::fi_fabric_attr* attr)
        : fabric(fab), name(attr && attr->name ? attr->name : ""),
          provider_name(attr && attr->prov_name ? attr->prov_name : ""),
          provider_version(attr ? attr->prov_version : 0) {}

    /**
     * @brief Destructor that closes the libfabric fabric handle.
     */
    ~fabric_impl() {
        if (fabric) {
            ::fi_close(&fabric->fid);
        }
    }

    /**
     * @brief Deleted copy constructor.
     */
    fabric_impl(const fabric_impl&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const fabric_impl&) -> fabric_impl& = delete;
    /**
     * @brief Deleted move constructor.
     */
    fabric_impl(fabric_impl&&) = delete;
    /**
     * @brief Deleted move assignment operator.
     */
    auto operator=(fabric_impl&&) -> fabric_impl& = delete;
};
}  // namespace detail

fabric_info::fabric_info(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

fabric_info::~fabric_info() = default;

fabric_info::fabric_info(fabric_info&&) noexcept = default;

auto fabric_info::operator=(fabric_info&&) noexcept -> fabric_info& = default;

auto fabric_info::impl_valid() const noexcept -> bool {
    return impl_ && impl_->info != nullptr;
}

auto fabric_info::get_fabric_attr() const -> std::optional<fabric_attributes> {
    if (!impl_ || !impl_->info || !impl_->info->fabric_attr) {
        return std::nullopt;
    }

    auto* attr = impl_->info->fabric_attr;

    return fabric_attributes{
        .name = attr->name ? std::string_view{attr->name} : std::string_view{},
        .provider_name = attr->prov_name ? std::string_view{attr->prov_name} : std::string_view{},
        .provider_version = fabric_version{attr->prov_version},
        .threading = threading_mode::safe,
        .control_progress = progress_mode::manual,
        .data_progress = progress_mode::manual,
    };
}

auto fabric_info::get_domain_attr() const -> std::optional<domain_attributes> {
    if (!impl_ || !impl_->info || !impl_->info->domain_attr) {
        return std::nullopt;
    }

    auto* attr = impl_->info->domain_attr;
    return domain_attributes{
        .name = attr->name ? std::string_view{attr->name} : std::string_view{},
        .threading = detail::from_fi_threading(attr->threading),
        .control_progress = detail::from_fi_progress(attr->control_progress),
        .data_progress = detail::from_fi_progress(attr->data_progress),
        .addr_format = detail::from_fi_addr_format(impl_->info->addr_format),
        .memory_region_mode = detail::from_fi_mr_mode(attr->mr_mode),
        .max_ep_tx_ctx = attr->max_ep_tx_ctx,
        .max_ep_rx_ctx = attr->max_ep_rx_ctx,
    };
}

auto fabric_info::get_endpoint_attr() const -> std::optional<endpoint_attributes> {
    if (!impl_ || !impl_->info || !impl_->info->ep_attr) {
        return std::nullopt;
    }

    auto* attr = impl_->info->ep_attr;

    auto* tx_attr = impl_->info->tx_attr;
    return endpoint_attributes{
        .type = detail::from_fi_ep_type(attr->type),
        .protocol = protocol_version{attr->protocol},
        .max_msg_size = queue_size{attr->max_msg_size},
        .tx_ctx_cnt = queue_size{attr->tx_ctx_cnt},
        .rx_ctx_cnt = queue_size{attr->rx_ctx_cnt},
        .msg_ordering = tx_attr ? detail::from_fi_order(tx_attr->msg_order) : msg_order{0ULL},
        .comp_ordering = tx_attr ? comp_order{tx_attr->comp_order} : comp_order{0ULL},
    };
}

auto fabric_info::get_caps() const -> caps {
    if (!impl_ || !impl_->info) {
        return caps{0ULL};
    }
    return detail::from_fi_caps(impl_->info->caps);
}

auto fabric_info::get_mode() const -> mode {
    if (!impl_ || !impl_->info) {
        return mode{0ULL};
    }
    return detail::from_fi_mode(impl_->info->mode);
}

auto fabric_info::get_address_format() const -> address_format {
    if (!impl_ || !impl_->info) {
        return address_format::unspecified;
    }
    return detail::from_fi_addr_format(impl_->info->addr_format);
}

auto fabric_info::begin() const -> iterator {
    return iterator{impl_ && impl_->info ? this : nullptr};
}

auto fabric_info::end() const -> iterator {
    return iterator{nullptr};
}

auto fabric_info::iterator::operator++() -> iterator& {
    if (current_ && current_->impl_ && current_->impl_->info) {
        current_ = current_->impl_->info->next ? current_ : nullptr;
    } else {
        current_ = nullptr;
    }
    return *this;
}

auto fabric_info::iterator::operator++(int) -> iterator {
    iterator tmp = *this;
    ++(*this);
    return tmp;
}

auto fabric_info::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->info : nullptr;
}

fabric::fabric(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

fabric::~fabric() = default;

fabric::fabric(fabric&&) noexcept = default;

auto fabric::operator=(fabric&&) noexcept -> fabric& = default;

auto fabric::impl_valid() const noexcept -> bool {
    return impl_ && impl_->fabric != nullptr;
}

auto fabric::create(const fabric_info& info, memory_resource* resource) -> result<fabric> {
    if (!info) {
        return make_error_result<fabric>(errc::invalid_argument);
    }

    auto* fi_info = static_cast<::fi_info*>(info.internal_ptr());
    if (!fi_info || !fi_info->fabric_attr) {
        return make_error_result<fabric>(errc::invalid_argument);
    }

    ::fid_fabric* fab = nullptr;
    int ret = ::fi_fabric(fi_info->fabric_attr, &fab, nullptr);

    if (ret != 0) {
        return make_error_result<fabric>(static_cast<errc>(ret));
    }

    if (resource) {
        auto impl =
            detail::make_pmr_unique<detail::fabric_impl>(resource, fab, fi_info->fabric_attr);
        return fabric{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::fabric_impl>(default_resource, fab, fi_info->fabric_attr);
    return fabric{std::move(impl)};
}

auto fabric::get_name() const -> std::string_view {
    if (!impl_) {
        return {};
    }
    return impl_->name;
}

auto fabric::get_provider_name() const -> std::string_view {
    if (!impl_) {
        return {};
    }
    return impl_->provider_name;
}

auto fabric::get_provider_version() const -> fabric_version {
    if (!impl_) {
        return fabric_version{0U};
    }
    return fabric_version{impl_->provider_version};
}

auto fabric::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->fabric : nullptr;
}

auto query_fabric(const fabric_hints& hints, memory_resource* resource) -> result<fabric_info> {
    ::fi_info* info_hints = ::fi_allocinfo();
    if (!info_hints) {
        return make_error_result<fabric_info>(errc::no_memory);
    }

    if (hints.capabilities) {
        info_hints->caps = detail::to_fi_caps(hints.capabilities);
    }
    if (hints.mode_bits) {
        info_hints->mode = detail::to_fi_mode(hints.mode_bits);
    }

    info_hints->addr_format = detail::to_fi_addr_format(hints.addr_format);

    if (info_hints->ep_attr) {
        info_hints->ep_attr->type = detail::to_fi_ep_type(hints.ep_type);
    }

    if (hints.threading || hints.memory_region_mode) {
        if (!info_hints->domain_attr) {
            info_hints->domain_attr = new ::fi_domain_attr{};
        }
        if (hints.threading) {
            info_hints->domain_attr->threading = detail::to_fi_threading(*hints.threading);
        }
        if (hints.memory_region_mode) {
            info_hints->domain_attr->mr_mode = detail::to_fi_mr_mode(*hints.memory_region_mode);
        }
    }

    ::fi_info* result = nullptr;
    int ret = ::fi_getinfo(
        FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), nullptr, nullptr, 0, info_hints, &result);

    ::fi_freeinfo(info_hints);

    if (ret != 0) {
        return make_error_result<fabric_info>(static_cast<errc>(ret));
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::fabric_info_impl>(resource, result);
        return fabric_info{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::fabric_info_impl>(default_resource, result);
    return fabric_info{std::move(impl)};
}

auto get_fabric_version() noexcept -> fabric_version {
    return fabric_version{::fi_version()};
}

}  // namespace loom
