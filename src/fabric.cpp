// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/fabric.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "loom/core/allocator.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/impl/fabric_impl.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

fabric_info::fabric_info() = default;

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
    current_ = current_ && current_->impl_
                   ? current_->impl_->next_entry(current_->impl_.get_deleter().resource)
                   : nullptr;
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

fabric::fabric() = default;

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
    const std::unique_ptr<::fi_info, decltype(&::fi_freeinfo)> info_hints{::fi_allocinfo(),
                                                                          &::fi_freeinfo};
    if (!info_hints || !info_hints->ep_attr || !info_hints->domain_attr ||
        !info_hints->fabric_attr) {
        return make_error_result<fabric_info>(errc::no_memory);
    }

    info_hints->caps = detail::to_fi_caps(hints.capabilities);
    info_hints->mode = detail::to_fi_mode(hints.mode_bits);
    info_hints->addr_format = detail::to_fi_addr_format(hints.addr_format);
    info_hints->ep_attr->type = detail::to_fi_ep_type(hints.ep_type);

    // fi_freeinfo owns these strings; copy bounded views into C allocations.
    const auto copy_name = [](std::optional<std::string_view> name, char*& output) {
        if (!name) {
            return true;
        }
        output = ::strdup(std::string{*name}.c_str());
        return output != nullptr;
    };
    if (!copy_name(hints.provider_name, info_hints->fabric_attr->prov_name) ||
        !copy_name(hints.fabric_name, info_hints->fabric_attr->name) ||
        !copy_name(hints.domain_name, info_hints->domain_attr->name)) {
        return make_error_result<fabric_info>(errc::no_memory);
    }
    if (hints.threading) {
        info_hints->domain_attr->threading = detail::to_fi_threading(*hints.threading);
    }
    if (hints.memory_region_mode) {
        info_hints->domain_attr->mr_mode = detail::to_fi_mr_mode(*hints.memory_region_mode);
    }
    if (hints.control_progress) {
        info_hints->domain_attr->control_progress = detail::to_fi_progress(*hints.control_progress);
    }
    if (hints.data_progress) {
        info_hints->domain_attr->data_progress = detail::to_fi_progress(*hints.data_progress);
    }

    ::fi_info* result = nullptr;
    int ret = ::fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
                           nullptr,
                           nullptr,
                           0,
                           info_hints.get(),
                           &result);

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
