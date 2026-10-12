// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/shared_context.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>

#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/unique_fid.hpp"

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
    std::uint64_t rx_op_flags{};
    std::size_t iov_limit{};

    /**
     * @brief Default constructor.
     */
    shared_rx_context_impl() = default;
    /**
     * @brief Constructs implementation from libfabric SRX handle.
     * @param srx_ The libfabric shared RX context handle.
     */
    explicit shared_rx_context_impl(::fid_ep* srx_, std::uint64_t flags = 0, std::size_t limit = 0)
        : srx(srx_), rx_op_flags(flags), iov_limit(limit) {}

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

shared_tx_context::shared_tx_context() = default;

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

    detail::unique_fid<::fid_stx> pending{stx};

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::shared_tx_context_impl>(resource, stx);
        (void)pending.release();
        return shared_tx_context{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::shared_tx_context_impl>(default_resource, stx);
    (void)pending.release();
    return shared_tx_context{std::move(impl)};
}

auto shared_tx_context::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->stx : nullptr;
}

shared_rx_context::shared_rx_context() = default;

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

    if (!FI_CHECK_OP(dom_ptr->ops, fi_ops_domain, srx_ctx))
        return make_error_result<shared_rx_context>(errc::not_supported);
    ::fi_rx_attr* rx_attr = info_ptr->rx_attr;
    int ret = ::fi_srx_context(dom_ptr, rx_attr, &srx, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<shared_rx_context>(-ret);
    }

    detail::unique_fid<::fid_ep> pending{srx};
    const auto rx_flags = rx_attr ? rx_attr->op_flags : 0;
    const auto limit = rx_attr ? rx_attr->iov_limit : 0;

    if (resource) {
        auto impl =
            detail::make_pmr_unique<detail::shared_rx_context_impl>(resource, srx, rx_flags, limit);
        (void)pending.release();
        return shared_rx_context{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::shared_rx_context_impl>(
        default_resource, srx, rx_flags, limit);
    (void)pending.release();
    return shared_rx_context{std::move(impl)};
}

auto shared_rx_context::cancel(context_ptr<void> context) -> void_result {
    if (!impl_ || !impl_->srx || !context)
        return make_error_result<void>(errc::invalid_argument);
    if (!FI_CHECK_OP(impl_->srx->ops, fi_ops_ep, cancel))
        return make_error_result<void>(errc::not_supported);
    const auto status = ::fi_cancel(&impl_->srx->fid, context.get());
    if (status != 0)
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    return make_success();
}

auto shared_rx_context::close() -> void_result {
    if (!impl_ || !impl_->srx)
        return make_success();
    if (!FI_CHECK_OP(impl_->srx->fid.ops, fi_ops, close))
        return make_error_result<void>(errc::not_supported);
    const auto status = ::fi_close(&impl_->srx->fid);
    if (status != 0)
        return make_error_result_from_fi_errno<void>(-status);
    impl_->srx = nullptr;
    return make_success();
}

auto shared_rx_context::iov_limits() const noexcept -> endpoint_iov_limits {
    return {.receive = impl_ && impl_->srx ? impl_->iov_limit : 0};
}

auto shared_rx_context::impl_operation_flags(completion_direction direction) const noexcept
    -> std::uint64_t {
    return impl_ && impl_->srx && direction == completion_direction::receive ? impl_->rx_op_flags
                                                                             : 0;
}

auto shared_rx_context::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->srx : nullptr;
}

}  // namespace loom
