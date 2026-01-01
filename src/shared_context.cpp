// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/shared_context.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>

#include "loom/async/completion_queue.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/counter.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
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
 * @struct shared_tx_context_impl
 * @brief Internal implementation for shared_tx_context.
 */
struct shared_tx_context_impl {
    ::fid_stx* stx{nullptr};  ///< Libfabric shared TX context handle

    /**
     * @brief Default constructor.
     */
    shared_tx_context_impl() = default;
    /**
     * @brief Constructs implementation from libfabric STX handle.
     * @param stx_ The libfabric shared TX context handle.
     */
    explicit shared_tx_context_impl(::fid_stx* stx_) : stx(stx_) {}

    /**
     * @brief Destructor that closes the libfabric STX handle.
     */
    ~shared_tx_context_impl() {
        if (stx) {
            ::fi_close(&stx->fid);
        }
    }

    /**
     * @brief Deleted copy constructor.
     */
    shared_tx_context_impl(const shared_tx_context_impl&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const shared_tx_context_impl&) -> shared_tx_context_impl& = delete;
    /**
     * @brief Deleted move constructor.
     */
    shared_tx_context_impl(shared_tx_context_impl&&) = delete;
    /**
     * @brief Deleted move assignment operator.
     */
    auto operator=(shared_tx_context_impl&&) -> shared_tx_context_impl& = delete;
};

/**
 * @struct shared_rx_context_impl
 * @brief Internal implementation for shared_rx_context.
 */
struct shared_rx_context_impl {
    ::fid_ep* srx{nullptr};  ///< Libfabric shared RX context handle

    /**
     * @brief Default constructor.
     */
    shared_rx_context_impl() = default;
    /**
     * @brief Constructs implementation from libfabric SRX handle.
     * @param srx_ The libfabric shared RX context handle.
     */
    explicit shared_rx_context_impl(::fid_ep* srx_) : srx(srx_) {}

    /**
     * @brief Destructor that closes the libfabric SRX handle.
     */
    ~shared_rx_context_impl() {
        if (srx) {
            ::fi_close(&srx->fid);
        }
    }

    /**
     * @brief Deleted copy constructor.
     */
    shared_rx_context_impl(const shared_rx_context_impl&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const shared_rx_context_impl&) -> shared_rx_context_impl& = delete;
    /**
     * @brief Deleted move constructor.
     */
    shared_rx_context_impl(shared_rx_context_impl&&) = delete;
    /**
     * @brief Deleted move assignment operator.
     */
    auto operator=(shared_rx_context_impl&&) -> shared_rx_context_impl& = delete;
};

}  // namespace detail

shared_tx_context::shared_tx_context(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

shared_tx_context::~shared_tx_context() = default;

shared_tx_context::shared_tx_context(shared_tx_context&&) noexcept = default;

auto shared_tx_context::operator=(shared_tx_context&&) noexcept -> shared_tx_context& = default;

auto shared_tx_context::impl_valid() const noexcept -> bool {
    return impl_ && impl_->stx;
}

auto shared_tx_context::create(const domain& dom,
                               const fabric_info& info,
                               memory_resource* resource) -> result<shared_tx_context> {
    if (!dom || !info) {
        return make_error_result<shared_tx_context>(errc::invalid_argument);
    }

    ::fid_stx* stx = nullptr;
    auto* dom_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* info_ptr = static_cast<::fi_info*>(info.internal_ptr());

    ::fi_tx_attr* tx_attr = info_ptr->tx_attr;
    int ret = ::fi_stx_context(dom_ptr, tx_attr, &stx, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<shared_tx_context>(-ret);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::shared_tx_context_impl>(resource, stx);
        return shared_tx_context{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::shared_tx_context_impl>(default_resource, stx);
    return shared_tx_context{std::move(impl)};
}

auto shared_tx_context::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->stx : nullptr;
}

shared_rx_context::shared_rx_context(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

shared_rx_context::~shared_rx_context() = default;

shared_rx_context::shared_rx_context(shared_rx_context&&) noexcept = default;

auto shared_rx_context::operator=(shared_rx_context&&) noexcept -> shared_rx_context& = default;

auto shared_rx_context::impl_valid() const noexcept -> bool {
    return impl_ && impl_->srx;
}

auto shared_rx_context::create(const domain& dom,
                               const fabric_info& info,
                               memory_resource* resource) -> result<shared_rx_context> {
    if (!dom || !info) {
        return make_error_result<shared_rx_context>(errc::invalid_argument);
    }

    ::fid_ep* srx = nullptr;
    auto* dom_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* info_ptr = static_cast<::fi_info*>(info.internal_ptr());

    ::fi_rx_attr* rx_attr = info_ptr->rx_attr;
    int ret = ::fi_srx_context(dom_ptr, rx_attr, &srx, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<shared_rx_context>(-ret);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::shared_rx_context_impl>(resource, srx);
        return shared_rx_context{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::shared_rx_context_impl>(default_resource, srx);
    return shared_rx_context{std::move(impl)};
}

auto shared_rx_context::bind_cq(completion_queue& cq, cq_bind_flags flags) -> void_result {
    if (!impl_ || !impl_->srx) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!cq) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* cq_fid = static_cast<::fid*>(cq.internal_ptr());
    auto fi_flags = detail::to_fi_cq_bind_flags(flags);
    int ret = ::fi_ep_bind(impl_->srx, cq_fid, fi_flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto shared_rx_context::bind_counter(counter& cntr, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->srx) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* cntr_fid = static_cast<::fid*>(cntr.internal_ptr());
    int ret = ::fi_ep_bind(impl_->srx, cntr_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto shared_rx_context::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->srx : nullptr;
}

}  // namespace loom
