// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>

#include <string>
#include <utility>

#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/event_queue.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/mr_key.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom::detail {

/// @brief Internal implementation of the domain class.
struct domain_impl {
    ::fid_domain* domain{nullptr};                          ///< Underlying libfabric domain handle.
    threading_mode threading{threading_mode::unspecified};  ///< Threading model for the domain.
    progress_mode control_progress{progress_mode::unspecified};  ///< Control progress mode.
    progress_mode data_progress{progress_mode::unspecified};     ///< Data progress mode.

    address_format addr_format{address_format::unspecified};
    std::string name;
    int mr_mode{};
    std::shared_ptr<mr_key_pool> mr_keys;
    std::size_t max_error_data{};  ///< Negotiated maximum CQ/EQ diagnostic size.
    std::uint64_t max_counter_value{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_error_counter_value{std::numeric_limits<std::uint64_t>::max()};

    /// @brief Default constructor.
    domain_impl() = default;

    /// @brief Constructs domain_impl with a libfabric domain handle and modes.
    /// @param dom The libfabric domain pointer.
    /// @param thread The threading mode.
    /// @param ctrl_prog The control progress mode.
    /// @param data_prog The data progress mode.
    explicit domain_impl(::fid_domain* dom,
                         threading_mode thread = threading_mode::unspecified,
                         progress_mode ctrl_prog = progress_mode::unspecified,
                         progress_mode data_prog = progress_mode::unspecified,
                         address_format format = address_format::unspecified,
                         std::string domain_name = {},
                         int registration_mode = 0,
                         std::size_t key_bytes = sizeof(std::uint64_t),
                         memory_resource* resource = get_default_resource())
        : domain(dom), threading(thread), control_progress(ctrl_prog), data_progress(data_prog),
          addr_format(format), name(std::move(domain_name)), mr_mode(registration_mode),
          mr_keys((registration_mode & FI_MR_PROV_KEY) || registration_mode == FI_MR_BASIC
                      ? nullptr
                      : std::allocate_shared<mr_key_pool>(
                            pmr_allocator<mr_key_pool>{resource}, key_bytes, resource)) {}

    /// @brief Destructor that closes the libfabric domain.
    ~domain_impl() {
        if (domain) {
            ::fi_close(&domain->fid);
        }
    }

    domain_impl(const domain_impl&) = delete;
    auto operator=(const domain_impl&) -> domain_impl& = delete;
    domain_impl(domain_impl&&) = delete;
    auto operator=(domain_impl&&) -> domain_impl& = delete;
};

}  // namespace loom::detail

#ifdef LOOM_IMPLEMENTATION

namespace loom {

domain::domain() = default;

domain::domain(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

domain::~domain() = default;

domain::domain(domain&&) noexcept = default;

auto domain::operator=(domain&&) noexcept -> domain& = default;

auto domain::impl_valid() const noexcept -> bool {
    return impl_ && impl_->domain != nullptr;
}

auto domain::create(const fabric& fab, const fabric_info& info, memory_resource* resource)
    -> result<domain> {
    if (!fab || !info) {
        return make_error_result<domain>(errc::invalid_argument);
    }

    ::fid_domain* dom = nullptr;
    auto* fab_ptr = static_cast<::fid_fabric*>(fab.internal_ptr());
    auto* info_ptr = static_cast<::fi_info*>(info.internal_ptr());

    int ret = ::fi_domain(fab_ptr, info_ptr, &dom, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<domain>(-ret);
    }

    detail::unique_fid<::fid_domain> pending{dom};

    auto threading = threading_mode::unspecified;
    auto control_prog = progress_mode::unspecified;
    auto data_prog = progress_mode::unspecified;

    if (info_ptr->domain_attr != nullptr) {
        threading = detail::from_fi_threading(info_ptr->domain_attr->threading);
        control_prog = detail::from_fi_progress(info_ptr->domain_attr->control_progress);
        data_prog = detail::from_fi_progress(info_ptr->domain_attr->data_progress);
    }

    const auto registration_mode = info_ptr->domain_attr ? info_ptr->domain_attr->mr_mode : 0;
    const auto key_bytes =
        info_ptr->domain_attr ? info_ptr->domain_attr->mr_key_size : sizeof(std::uint64_t);
    const auto format = detail::from_fi_addr_format(info_ptr->addr_format);
    const std::string domain_name =
        info_ptr->domain_attr && info_ptr->domain_attr->name ? info_ptr->domain_attr->name : "";

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::domain_impl>(resource,
                                                                 dom,
                                                                 threading,
                                                                 control_prog,
                                                                 data_prog,
                                                                 format,
                                                                 domain_name,
                                                                 registration_mode,
                                                                 key_bytes,
                                                                 resource);
        impl->max_error_data = info_ptr->domain_attr ? info_ptr->domain_attr->max_err_data : 0;
        impl->max_counter_value = detail::from_fi_counter_limit(
            info_ptr->domain_attr ? info_ptr->domain_attr->max_cntr_value : 0);
        impl->max_error_counter_value = detail::from_fi_counter_limit(
            info_ptr->domain_attr ? info_ptr->domain_attr->max_err_cntr_value : 0);
        (void)pending.release();
        return domain{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::domain_impl>(default_resource,
                                                             dom,
                                                             threading,
                                                             control_prog,
                                                             data_prog,
                                                             format,
                                                             domain_name,
                                                             registration_mode,
                                                             key_bytes,
                                                             default_resource);
    impl->max_error_data = info_ptr->domain_attr ? info_ptr->domain_attr->max_err_data : 0;
    impl->max_counter_value = detail::from_fi_counter_limit(
        info_ptr->domain_attr ? info_ptr->domain_attr->max_cntr_value : 0);
    impl->max_error_counter_value = detail::from_fi_counter_limit(
        info_ptr->domain_attr ? info_ptr->domain_attr->max_err_cntr_value : 0);
    (void)pending.release();
    return domain{std::move(impl)};
}

auto domain::get_name() const -> std::string_view {
    return impl_ ? std::string_view{impl_->name} : std::string_view{};
}

auto domain::get_address_format() const noexcept -> address_format {
    return impl_ ? impl_->addr_format : address_format::unspecified;
}

auto domain::get_memory_region_mode() const noexcept -> mr_mode {
    return impl_ && impl_->domain ? detail::from_fi_mr_mode(impl_->mr_mode) : mr_mode{};
}

auto domain::get_threading() const -> threading_mode {
    if (!impl_) {
        return threading_mode::unspecified;
    }
    return impl_->threading;
}

auto domain::get_control_progress() const -> progress_mode {
    if (!impl_) {
        return progress_mode::unspecified;
    }
    return impl_->control_progress;
}

auto domain::get_data_progress() const -> progress_mode {
    if (!impl_) {
        return progress_mode::unspecified;
    }
    return impl_->data_progress;
}

auto domain::progress_policy() const noexcept -> runtime_progress_policy {
    if (!impl_) {
        return runtime_progress_policy{};
    }
    return runtime_progress_policy{impl_->control_progress, impl_->data_progress};
}

auto domain::bind_eq(event_queue& eq, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->domain) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!eq) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* eq_fid = static_cast<::fid*>(eq.internal_ptr());
    int ret = ::fi_domain_bind(impl_->domain, eq_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto domain::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->domain : nullptr;
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
