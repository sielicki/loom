// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/scalable_endpoint.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_endpoint.h>

#include <utility>

#include "loom/core/address_vector.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/impl/scalable_endpoint_impl.hpp"
#include "loom/detail/native_address.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom {

scalable_endpoint::scalable_endpoint() = default;

scalable_endpoint::scalable_endpoint(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

scalable_endpoint::~scalable_endpoint() = default;

scalable_endpoint::scalable_endpoint(scalable_endpoint&&) noexcept = default;

auto scalable_endpoint::operator=(scalable_endpoint&&) noexcept -> scalable_endpoint& = default;

auto scalable_endpoint::impl_valid() const noexcept -> bool {
    return impl_ && impl_->sep;
}

auto scalable_endpoint::create(const domain& dom,
                               const fabric_info& info,
                               memory_resource* resource) -> result<scalable_endpoint> {
    if (!dom || !info) {
        return make_error_result<scalable_endpoint>(errc::invalid_argument);
    }

    ::fid_ep* sep = nullptr;
    auto* dom_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* info_ptr = static_cast<::fi_info*>(info.internal_ptr());

    int ret = ::fi_scalable_ep(dom_ptr, info_ptr, &sep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<scalable_endpoint>(-ret);
    }

    detail::unique_fid<::fid_ep> pending{sep};

    std::size_t tx_ctx_cnt = 0;
    std::size_t rx_ctx_cnt = 0;
    const auto max_msg_size = info_ptr->ep_attr ? info_ptr->ep_attr->max_msg_size : 0;
    const auto inject_size = info_ptr->tx_attr ? info_ptr->tx_attr->inject_size : 0;
    const auto capabilities = detail::from_fi_caps(info_ptr->caps);
    const auto tx_flags = info_ptr->tx_attr ? info_ptr->tx_attr->op_flags : 0;
    const auto rx_flags = info_ptr->rx_attr ? info_ptr->rx_attr->op_flags : 0;
    endpoint_type type{endpoint_types::msg};
    const auto format = detail::from_fi_addr_format(info_ptr->addr_format);
    if (info_ptr->ep_attr) {
        tx_ctx_cnt = info_ptr->ep_attr->tx_ctx_cnt;
        rx_ctx_cnt = info_ptr->ep_attr->rx_ctx_cnt;
        type = detail::from_fi_ep_type(info_ptr->ep_attr->type);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(
            resource,
            sep,
            tx_ctx_cnt,
            rx_ctx_cnt,
            type,
            format,
            max_msg_size,
            inject_size,
            capabilities,
            tx_flags,
            rx_flags,
            dom_ptr,
            detail::from_fi_iov_limits(*info_ptr),
            info_ptr->tx_attr,
            info_ptr->rx_attr,
            info_ptr->mode);
        (void)pending.release();
        return scalable_endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(
        default_resource,
        sep,
        tx_ctx_cnt,
        rx_ctx_cnt,
        type,
        format,
        max_msg_size,
        inject_size,
        capabilities,
        tx_flags,
        rx_flags,
        dom_ptr,
        detail::from_fi_iov_limits(*info_ptr),
        info_ptr->tx_attr,
        info_ptr->rx_attr,
        info_ptr->mode);
    (void)pending.release();
    return scalable_endpoint{std::move(impl)};
}

auto scalable_endpoint::bind_av(address_vector& av, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->sep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!av) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* av_fid = static_cast<::fid*>(av.internal_ptr());
    int ret = ::fi_scalable_ep_bind(impl_->sep, av_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto scalable_endpoint::enable() -> void_result {
    if (!impl_ || !impl_->sep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_enable(impl_->sep);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto scalable_endpoint::get_local_address() const -> result<address> {
    if (!impl_valid()) {
        return make_error_result<address>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->sep->cm, fi_ops_cm, getname)) {
        return make_error_result<address>(errc::not_supported);
    }

    return detail::query_address(
        [&](void* data, std::size_t* length) {
            return ::fi_getname(&impl_->sep->fid, data, length);
        },
        impl_->addr_format);
}

auto scalable_endpoint::tx_context_attributes() const -> result<::fi_tx_attr> {
    if (!impl_valid()) {
        return make_error_result<::fi_tx_attr>(errc::invalid_argument);
    }
    if (!impl_->tx_attributes) {
        return make_error_result<::fi_tx_attr>(errc::not_supported);
    }
    return *impl_->tx_attributes;
}

auto scalable_endpoint::rx_context_attributes() const -> result<::fi_rx_attr> {
    if (!impl_valid()) {
        return make_error_result<::fi_rx_attr>(errc::invalid_argument);
    }
    if (!impl_->rx_attributes) {
        return make_error_result<::fi_rx_attr>(errc::not_supported);
    }
    return *impl_->rx_attributes;
}

auto scalable_endpoint::tx_context(std::size_t index) -> result<endpoint> {
    return create_tx_context(index, nullptr);
}

auto scalable_endpoint::rx_context(std::size_t index) -> result<endpoint> {
    return create_rx_context(index, nullptr);
}

auto scalable_endpoint::tx_context(std::size_t index, ::fi_tx_attr& attributes)
    -> result<endpoint> {
    auto native = attributes;
    auto context = create_tx_context(index, &native);
    if (context) {
        attributes = native;
    }
    return context;
}

auto scalable_endpoint::rx_context(std::size_t index, ::fi_rx_attr& attributes)
    -> result<endpoint> {
    auto native = attributes;
    auto context = create_rx_context(index, &native);
    if (context) {
        attributes = native;
    }
    return context;
}

auto scalable_endpoint::create_tx_context(std::size_t index, ::fi_tx_attr* attributes)
    -> result<endpoint> {
    if (!impl_ || !impl_->sep) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    if (!std::in_range<int>(index) || index >= impl_->tx_ctx_cnt) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(impl_->sep->ops, fi_ops_ep, tx_ctx)) {
        return make_error_result<endpoint>(errc::not_supported);
    }

    const auto requested_mode = attributes             ? attributes->mode
                                : impl_->tx_attributes ? impl_->tx_attributes->mode
                                                       : 0ULL;
    if ((impl_->mode | requested_mode) & FI_ASYNC_IOV) {
        return make_error_result<endpoint>(errc::not_supported);
    }

    ::fid_ep* tx_ep = nullptr;
    int ret = ::fi_tx_context(impl_->sep, static_cast<int>(index), attributes, &tx_ep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<endpoint>(-ret);
    }

    detail::unique_fid<::fid_ep> pending{tx_ep};
    if (attributes && (attributes->mode & FI_ASYNC_IOV)) {
        return make_error_result<endpoint>(errc::not_supported);
    }
    auto limits = impl_->iov_limits;
    if (attributes) {
        limits.transmit = attributes->iov_limit;
        limits.rma = attributes->rma_iov_limit;
    }

    auto* resource = std::pmr::get_default_resource();
    auto ep_impl = detail::make_pmr_unique<detail::endpoint_impl>(
        resource,
        tx_ep,
        impl_->type,
        impl_->addr_format,
        impl_->max_msg_size,
        attributes ? attributes->inject_size : impl_->inject_size,
        attributes ? detail::from_fi_caps(attributes->caps) : impl_->capabilities,
        attributes ? attributes->op_flags : impl_->tx_op_flags,
        impl_->rx_op_flags,
        impl_->origin_domain,
        limits);
    (void)pending.release();
    return endpoint{std::move(ep_impl)};
}

auto scalable_endpoint::create_rx_context(std::size_t index, ::fi_rx_attr* attributes)
    -> result<endpoint> {
    if (!impl_ || !impl_->sep) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    if (!std::in_range<int>(index) || index >= impl_->rx_ctx_cnt) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(impl_->sep->ops, fi_ops_ep, rx_ctx)) {
        return make_error_result<endpoint>(errc::not_supported);
    }

    const auto requested_mode = attributes             ? attributes->mode
                                : impl_->rx_attributes ? impl_->rx_attributes->mode
                                                       : 0ULL;
    if ((impl_->mode | requested_mode) & FI_ASYNC_IOV) {
        return make_error_result<endpoint>(errc::not_supported);
    }

    ::fid_ep* rx_ep = nullptr;
    int ret = ::fi_rx_context(impl_->sep, static_cast<int>(index), attributes, &rx_ep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<endpoint>(-ret);
    }

    detail::unique_fid<::fid_ep> pending{rx_ep};
    if (attributes && (attributes->mode & FI_ASYNC_IOV)) {
        return make_error_result<endpoint>(errc::not_supported);
    }
    auto limits = impl_->iov_limits;
    if (attributes) {
        limits.receive = attributes->iov_limit;
    }

    auto* resource = std::pmr::get_default_resource();
    auto ep_impl = detail::make_pmr_unique<detail::endpoint_impl>(
        resource,
        rx_ep,
        impl_->type,
        impl_->addr_format,
        impl_->max_msg_size,
        0UZ,
        attributes ? detail::from_fi_caps(attributes->caps) : impl_->capabilities,
        impl_->tx_op_flags,
        attributes ? attributes->op_flags : impl_->rx_op_flags,
        impl_->origin_domain,
        limits);
    (void)pending.release();
    return endpoint{std::move(ep_impl)};
}

auto scalable_endpoint::tx_context_count() const noexcept -> std::size_t {
    return impl_ ? impl_->tx_ctx_cnt : 0;
}

auto scalable_endpoint::rx_context_count() const noexcept -> std::size_t {
    return impl_ ? impl_->rx_ctx_cnt : 0;
}

auto scalable_endpoint::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->sep : nullptr;
}

}  // namespace loom
