// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/scalable_endpoint.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>

#include "loom/core/address_vector.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/impl/scalable_endpoint_impl.hpp"

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

    std::size_t tx_ctx_cnt = 0;
    std::size_t rx_ctx_cnt = 0;
    const auto max_msg_size = info_ptr->ep_attr ? info_ptr->ep_attr->max_msg_size : 0;
    const auto inject_size = info_ptr->tx_attr ? info_ptr->tx_attr->inject_size : 0;
    const auto capabilities = detail::from_fi_caps(info_ptr->caps);
    endpoint_type type{endpoint_types::msg};
    const auto format = detail::from_fi_addr_format(info_ptr->addr_format);
    if (info_ptr->ep_attr) {
        tx_ctx_cnt = info_ptr->ep_attr->tx_ctx_cnt;
        rx_ctx_cnt = info_ptr->ep_attr->rx_ctx_cnt;
        type = detail::from_fi_ep_type(info_ptr->ep_attr->type);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(resource,
                                                                            sep,
                                                                            tx_ctx_cnt,
                                                                            rx_ctx_cnt,
                                                                            type,
                                                                            format,
                                                                            max_msg_size,
                                                                            inject_size,
                                                                            capabilities);
        return scalable_endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(default_resource,
                                                                        sep,
                                                                        tx_ctx_cnt,
                                                                        rx_ctx_cnt,
                                                                        type,
                                                                        format,
                                                                        max_msg_size,
                                                                        inject_size,
                                                                        capabilities);
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

auto scalable_endpoint::tx_context(std::size_t index) -> result<endpoint> {
    if (!impl_ || !impl_->sep) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    if (index >= impl_->tx_ctx_cnt) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    ::fid_ep* tx_ep = nullptr;
    int ret = ::fi_tx_context(impl_->sep, static_cast<int>(index), nullptr, &tx_ep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<endpoint>(-ret);
    }

    auto* resource = std::pmr::get_default_resource();
    auto ep_impl = detail::make_pmr_unique<detail::endpoint_impl>(resource,
                                                                  tx_ep,
                                                                  impl_->type,
                                                                  impl_->addr_format,
                                                                  impl_->max_msg_size,
                                                                  impl_->inject_size,
                                                                  impl_->capabilities);
    return endpoint{std::move(ep_impl)};
}

auto scalable_endpoint::rx_context(std::size_t index) -> result<endpoint> {
    if (!impl_ || !impl_->sep) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    if (index >= impl_->rx_ctx_cnt) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    ::fid_ep* rx_ep = nullptr;
    int ret = ::fi_rx_context(impl_->sep, static_cast<int>(index), nullptr, &rx_ep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<endpoint>(-ret);
    }

    auto* resource = std::pmr::get_default_resource();
    auto ep_impl = detail::make_pmr_unique<detail::endpoint_impl>(resource,
                                                                  rx_ep,
                                                                  impl_->type,
                                                                  impl_->addr_format,
                                                                  impl_->max_msg_size,
                                                                  0UZ,
                                                                  impl_->capabilities);
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
