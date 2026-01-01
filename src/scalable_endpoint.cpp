// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/scalable_endpoint.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>

#include "loom/core/address_vector.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"

namespace loom {

namespace detail {

/// @brief Internal implementation of the scalable endpoint.
struct scalable_endpoint_impl {
    ::fid_ep* sep{nullptr};     ///< Underlying libfabric scalable endpoint handle.
    std::size_t tx_ctx_cnt{0};  ///< Number of transmit contexts.
    std::size_t rx_ctx_cnt{0};  ///< Number of receive contexts.

    /// @brief Default constructor.
    scalable_endpoint_impl() = default;

    /// @brief Constructs scalable_endpoint_impl with a libfabric SEP handle and context counts.
    /// @param sep_ The libfabric scalable endpoint pointer.
    /// @param tx_cnt The number of transmit contexts.
    /// @param rx_cnt The number of receive contexts.
    scalable_endpoint_impl(::fid_ep* sep_, std::size_t tx_cnt, std::size_t rx_cnt)
        : sep(sep_), tx_ctx_cnt(tx_cnt), rx_ctx_cnt(rx_cnt) {}

    /// @brief Destructor that closes the libfabric scalable endpoint.
    ~scalable_endpoint_impl() {
        if (sep) {
            ::fi_close(&sep->fid);
        }
    }

    scalable_endpoint_impl(const scalable_endpoint_impl&) = delete;
    auto operator=(const scalable_endpoint_impl&) -> scalable_endpoint_impl& = delete;
    scalable_endpoint_impl(scalable_endpoint_impl&&) = delete;
    auto operator=(scalable_endpoint_impl&&) -> scalable_endpoint_impl& = delete;
};

/// @brief Internal implementation of the endpoint (for tx/rx contexts).
struct endpoint_impl {
    ::fid_ep* ep{nullptr};                    ///< Underlying libfabric endpoint handle.
    endpoint_type type{endpoint_types::msg};  ///< Endpoint type.
    address_format addr_format{address_format::unspecified};  ///< Address format.

    /// @brief Default constructor.
    endpoint_impl() = default;

    /// @brief Constructs endpoint_impl with a libfabric endpoint handle.
    /// @param ep_ The libfabric endpoint pointer.
    explicit endpoint_impl(::fid_ep* ep_) : ep(ep_) {}

    /// @brief Destructor that closes the libfabric endpoint.
    ~endpoint_impl() {
        if (ep) {
            ::fi_close(&ep->fid);
        }
    }

    endpoint_impl(const endpoint_impl&) = delete;
    auto operator=(const endpoint_impl&) -> endpoint_impl& = delete;
    endpoint_impl(endpoint_impl&&) = delete;
    auto operator=(endpoint_impl&&) -> endpoint_impl& = delete;
};
}  // namespace detail

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
    if (info_ptr->ep_attr) {
        tx_ctx_cnt = info_ptr->ep_attr->tx_ctx_cnt;
        rx_ctx_cnt = info_ptr->ep_attr->rx_ctx_cnt;
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(
            resource, sep, tx_ctx_cnt, rx_ctx_cnt);
        return scalable_endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(
        default_resource, sep, tx_ctx_cnt, rx_ctx_cnt);
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
    auto ep_impl = detail::make_pmr_unique<detail::endpoint_impl>(resource, tx_ep);
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
    auto ep_impl = detail::make_pmr_unique<detail::endpoint_impl>(resource, rx_ep);
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
