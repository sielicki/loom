// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>

#include "loom/async/completion_queue.hpp"
#include "loom/core/address_vector.hpp"
#include "loom/core/counter.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/event_queue.hpp"
#include "loom/core/shared_context.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/impl/endpoint_impl.hpp"

namespace loom {

auto completion_registration::bind(endpoint& ep,
                                   completion_direction direction,
                                   context_ptr<void> context,
                                   void* object,
                                   callback complete) noexcept -> void_result {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    detail::completion_queue_impl* queue{};
    switch (direction) {
        case completion_direction::transmit:
            queue = ep.impl_->tx_cq.owner;
            break;
        case completion_direction::receive:
            queue = ep.impl_->rx_cq.owner;
            break;
        default:
            return make_error_result<void>(errc::invalid_argument);
    }
    if (!queue) {
        return make_error_result<void>(errc::invalid_argument);
    }
    return bind_to(*queue, context, object, complete);
}

auto endpoint::bind_cq(const completion_queue& cq, cq_bind_flags flags) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!cq) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* cq_fid = static_cast<::fid*>(cq.internal_ptr());
    auto fi_flags = detail::to_fi_cq_bind_flags(flags);
    int ret = ::fi_ep_bind(impl_->ep, cq_fid, fi_flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    if (flags.has(cq_bind::transmit)) {
        impl_->tx_cq.bind(*cq.impl_);
    }
    if (flags.has(cq_bind::recv)) {
        impl_->rx_cq.bind(*cq.impl_);
    }
    return make_success();
}

auto endpoint::bind_tx_cq(completion_queue& cq) -> void_result {
    return bind_cq(cq, cq_bind::transmit);
}

auto endpoint::bind_rx_cq(completion_queue& cq) -> void_result {
    return bind_cq(cq, cq_bind::recv);
}

auto endpoint::bind_eq(event_queue& eq, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!eq) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* eq_fid = static_cast<::fid*>(eq.internal_ptr());
    int ret = ::fi_ep_bind(impl_->ep, eq_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::bind_av(const address_vector& av, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!av) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* av_fid = static_cast<::fid*>(av.internal_ptr());
    int ret = ::fi_ep_bind(impl_->ep, av_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::bind_counter(counter& cntr, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* cntr_fid = static_cast<::fid*>(cntr.internal_ptr());
    int ret = ::fi_ep_bind(impl_->ep, cntr_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::bind_stx(shared_tx_context& stx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!stx) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* stx_ptr = static_cast<::fid_stx*>(stx.internal_ptr());
    int ret = ::fi_ep_bind(impl_->ep, &stx_ptr->fid, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::bind_srx(shared_rx_context& srx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!srx) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* srx_ptr = static_cast<::fid_ep*>(srx.internal_ptr());
    int ret = ::fi_ep_bind(impl_->ep, &srx_ptr->fid, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

}  // namespace loom
