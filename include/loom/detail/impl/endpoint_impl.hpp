// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_ext.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>

#include <array>
#include <optional>

#include "loom/async/completion_queue.hpp"
#include "loom/core/address.hpp"
#include "loom/core/address_vector.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/counter.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/endpoint_options.hpp"
#include "loom/core/error.hpp"
#include "loom/core/event_queue.hpp"
#include "loom/core/fabric.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/shared_context.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/impl/completion_queue_impl.hpp"
#include "loom/detail/native_address.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom {
class completion_queue;
}  // namespace loom

namespace loom::detail {

/// @brief Internal implementation of the endpoint.
struct endpoint_impl {
    ::fid_ep* ep{nullptr};                    ///< Underlying libfabric endpoint handle.
    endpoint_type type{endpoint_types::msg};  ///< Endpoint type (msg, rdm, dgram).
    address_format addr_format{address_format::unspecified};  ///< Address format.
    std::size_t max_msg_size{};
    std::size_t inject_size{};
    caps capabilities{};
    std::uint64_t tx_op_flags{};
    std::uint64_t rx_op_flags{};
    ::fid_domain* origin_domain{};
    endpoint_iov_limits iov_limits{};
    completion_queue_binding tx_cq;
    completion_queue_binding rx_cq;

    /// @brief Default constructor.
    endpoint_impl() = default;

    /// @brief Constructs endpoint_impl with a libfabric endpoint handle.
    /// @param ep_ The libfabric endpoint pointer.
    /// @param type_ The endpoint type.
    /// @param fmt The address format.
    /// @param max_msg_size_ The maximum transfer size from the creation attributes.
    /// @param inject_size_ The inject limit from the creation attributes.
    /// @param capabilities_ The capabilities negotiated during creation.
    /// @param tx_op_flags_ Native transmit flags from the creation attributes.
    /// @param rx_op_flags_ Native receive flags from the creation attributes.
    /// @param limits Negotiated vector counts; omit only when no vector I/O is used.
    endpoint_impl(::fid_ep* ep_,
                  endpoint_type type_,
                  address_format fmt,
                  std::size_t max_msg_size_ = 0,
                  std::size_t inject_size_ = 0,
                  caps capabilities_ = {},
                  std::uint64_t tx_op_flags_ = 0,
                  std::uint64_t rx_op_flags_ = 0,
                  ::fid_domain* origin = nullptr,
                  endpoint_iov_limits limits = {})
        : ep(ep_), type(type_), addr_format(fmt), max_msg_size(max_msg_size_),
          inject_size(inject_size_), capabilities(capabilities_), tx_op_flags(tx_op_flags_),
          rx_op_flags(rx_op_flags_), origin_domain(origin), iov_limits(limits) {}

    [[nodiscard]] auto query_size_limit(int option, std::size_t fallback) const
        -> result<std::size_t> {
        if (!ep) {
            return make_error_result<std::size_t>(errc::invalid_argument);
        }
        if (!ep->ops || !FI_CHECK_OP(ep->ops, fi_ops_ep, getopt)) {
            return fallback;
        }
        std::size_t value{};
        auto length = sizeof(value);
        const auto result = ::fi_getopt(&ep->fid, FI_OPT_ENDPOINT, option, &value, &length);
        if (result == -FI_ENOPROTOOPT || result == -FI_ENOSYS) {
            return fallback;
        }
        if (result != 0) {
            return make_error_result_from_fi_errno<std::size_t>(-result);
        }
        if (length != sizeof(value)) {
            return make_error_result<std::size_t>(errc::io_error);
        }
        return value;
    }

    [[nodiscard]] auto size_limit(int option, std::size_t fallback) const -> std::size_t {
        return query_size_limit(option, fallback).value_or(0);
    }

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

}  // namespace loom::detail

#ifdef LOOM_IMPLEMENTATION

namespace loom {

// ============================================================================
// endpoint.cpp implementation
// ============================================================================

endpoint::endpoint() = default;

endpoint::endpoint(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

endpoint::~endpoint() = default;

auto endpoint::close() noexcept -> void_result {
    if (impl_ && impl_->ep) {
        const auto status = ::fi_close(&impl_->ep->fid);
        if (status != 0) {
            return make_error_result_from_fi_errno<void>(-status);
        }
        impl_->ep = nullptr;
    }
    impl_.reset();
    return make_success();
}

endpoint::endpoint(endpoint&&) noexcept = default;

auto endpoint::operator=(endpoint&&) noexcept -> endpoint& = default;

auto endpoint::impl_operation_flags(completion_direction direction) const noexcept
    -> std::uint64_t {
    if (!impl_) {
        return 0;
    }
    switch (direction) {
        case completion_direction::transmit:
            return impl_->tx_op_flags;
        case completion_direction::receive:
            return impl_->rx_op_flags;
    }
    return 0;
}

auto endpoint::iov_limits() const noexcept -> endpoint_iov_limits {
    return impl_ && impl_->ep ? impl_->iov_limits : endpoint_iov_limits{};
}

auto endpoint::belongs_to(const domain& dom) const noexcept -> bool {
    return impl_ && impl_->ep && dom && impl_->origin_domain == dom.internal_ptr();
}

auto endpoint::impl_valid() const noexcept -> bool {
    return impl_ && impl_->ep;
}

auto endpoint::create(const domain& dom, const fabric_info& info, memory_resource* resource)
    -> result<endpoint> {
    if (!dom || !info) {
        return make_error_result<endpoint>(errc::invalid_argument);
    }

    ::fid_ep* ep = nullptr;
    auto* dom_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* info_ptr = static_cast<::fi_info*>(info.internal_ptr());

    int ret = ::fi_endpoint(dom_ptr, info_ptr, &ep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<endpoint>(-ret);
    }

    detail::unique_fid<::fid_ep> pending{ep};

    auto ep_type = detail::from_fi_ep_type(info_ptr->ep_attr->type);
    auto addr_fmt = detail::from_fi_addr_format(info_ptr->addr_format);
    const auto max_msg_size = info_ptr->ep_attr->max_msg_size;
    const auto inject_size = info_ptr->tx_attr ? info_ptr->tx_attr->inject_size : 0;
    const auto capabilities = detail::from_fi_caps(info_ptr->caps);
    const auto tx_flags = info_ptr->tx_attr ? info_ptr->tx_attr->op_flags : 0;
    const auto rx_flags = info_ptr->rx_attr ? info_ptr->rx_attr->op_flags : 0;

    if (resource) {
        auto impl =
            detail::make_pmr_unique<detail::endpoint_impl>(resource,
                                                           ep,
                                                           ep_type,
                                                           addr_fmt,
                                                           max_msg_size,
                                                           inject_size,
                                                           capabilities,
                                                           tx_flags,
                                                           rx_flags,
                                                           dom_ptr,
                                                           detail::from_fi_iov_limits(*info_ptr));
        (void)pending.release();
        return endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::endpoint_impl>(default_resource,
                                                       ep,
                                                       ep_type,
                                                       addr_fmt,
                                                       max_msg_size,
                                                       inject_size,
                                                       capabilities,
                                                       tx_flags,
                                                       rx_flags,
                                                       dom_ptr,
                                                       detail::from_fi_iov_limits(*info_ptr));
    (void)pending.release();
    return endpoint{std::move(impl)};
}

auto endpoint::enable() -> result<void> {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_enable(impl_->ep);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::get_type() const -> result<endpoint_type> {
    if (!impl_) {
        return make_error_result<endpoint_type>(errc::invalid_argument);
    }

    return impl_->type;
}

auto endpoint::message_inject_size() const -> result<std::size_t> {
    if (!impl_) {
        return make_error_result<std::size_t>(errc::invalid_argument);
    }
    return impl_->query_size_limit(FI_OPT_INJECT_MSG_SIZE, impl_->inject_size);
}

auto endpoint::send(std::span<const std::byte> data, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->msg, fi_ops_msg, send)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret =
        ::fi_send(impl_->ep, data.data(), data.size(), nullptr, FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::recv(std::span<std::byte> buffer, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->msg, fi_ops_msg, recv)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret =
        ::fi_recv(impl_->ep, buffer.data(), buffer.size(), nullptr, FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::sendv(std::span<const iovec> iov, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->msg, fi_ops_msg, sendv)) {
        return make_error_result<void>(errc::not_supported);
    }

    if (iov.size() > impl_->iov_limits.transmit)
        return make_error_result<void>(errc::invalid_argument);

    ssize_t ret = ::fi_sendv(impl_->ep, iov.data(), nullptr, iov.size(), FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::recvv(std::span<const iovec> iov, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->msg, fi_ops_msg, recvv)) {
        return make_error_result<void>(errc::not_supported);
    }

    if (iov.size() > impl_->iov_limits.receive)
        return make_error_result<void>(errc::invalid_argument);

    ssize_t ret = ::fi_recvv(impl_->ep, iov.data(), nullptr, iov.size(), FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::tagged_send(std::span<const std::byte> data,
                           std::uint64_t tag,
                           context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->tagged, fi_ops_tagged, send)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret =
        ::fi_tsend(impl_->ep, data.data(), data.size(), nullptr, FI_ADDR_UNSPEC, tag, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::tagged_recv(std::span<std::byte> buffer,
                           std::uint64_t tag,
                           std::uint64_t ignore,
                           context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->tagged, fi_ops_tagged, recv)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret = ::fi_trecv(
        impl_->ep, buffer.data(), buffer.size(), nullptr, FI_ADDR_UNSPEC, tag, ignore, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::inject(std::span<const std::byte> data, fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->msg, fi_ops_msg, inject)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret = ::fi_inject(impl_->ep, data.data(), data.size(), dest_addr.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::inject_data(std::span<const std::byte> data,
                           std::uint64_t immediate_data,
                           fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->msg, fi_ops_msg, injectdata)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret =
        ::fi_injectdata(impl_->ep, data.data(), data.size(), immediate_data, dest_addr.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::tagged_inject(std::span<const std::byte> data,
                             std::uint64_t tag,
                             fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->tagged, fi_ops_tagged, inject)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret = ::fi_tinject(impl_->ep, data.data(), data.size(), dest_addr.get(), tag);

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::tagged_inject_data(std::span<const std::byte> data,
                                  std::uint64_t tag,
                                  std::uint64_t immediate_data,
                                  fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->tagged, fi_ops_tagged, injectdata)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret =
        ::fi_tinjectdata(impl_->ep, data.data(), data.size(), immediate_data, dest_addr.get(), tag);

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::cancel(context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->ops, fi_ops_ep, cancel)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret = ::fi_cancel(&impl_->ep->fid, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::connect(const address& peer_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(impl_->ep->cm, fi_ops_cm, connect)) {
        return make_error_result<void>(errc::not_supported);
    }

    const auto native = detail::to_native_address(peer_addr, impl_->addr_format);
    if (!native) {
        return std::unexpected(native.error());
    }
    const int ret =
        ::fi_connect(impl_->ep, detail::native_address_bytes(*native).data(), nullptr, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::accept() -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(impl_->ep->cm, fi_ops_cm, accept)) {
        return make_error_result<void>(errc::not_supported);
    }

    int ret = ::fi_accept(impl_->ep, nullptr, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::get_local_address() const -> result<address> {
    if (!impl_ || !impl_->ep) {
        return make_error_result<address>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(impl_->ep->cm, fi_ops_cm, getname)) {
        return make_error_result<address>(errc::not_supported);
    }

    return detail::query_address(
        [&](void* data, std::size_t* length) {
            return ::fi_getname(&impl_->ep->fid, data, length);
        },
        impl_->addr_format);
}

auto endpoint::get_peer_address() const -> result<address> {
    if (!impl_ || !impl_->ep) {
        return make_error_result<address>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(impl_->ep->cm, fi_ops_cm, getpeer)) {
        return make_error_result<address>(errc::not_supported);
    }

    return detail::query_address(
        [&](void* data, std::size_t* length) { return ::fi_getpeer(impl_->ep, data, length); },
        impl_->addr_format);
}

auto endpoint::read(std::span<std::byte> local_buffer,
                    rma_addr remote_addr,
                    mr_key key,
                    context_ptr<void> ctx) -> void_result {
    return rma::read_to(*this,
                        local_buffer,
                        mr_descriptor{},
                        fabric_addrs::unspecified,
                        remote_memory{remote_addr.get(), key.get(), local_buffer.size()},
                        ctx.get());
}

auto endpoint::write(std::span<const std::byte> local_buffer,
                     rma_addr remote_addr,
                     mr_key key,
                     context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(impl_->ep->rma, fi_ops_rma, write)) {
        return make_error_result<void>(errc::not_supported);
    }

    ssize_t ret = ::fi_write(impl_->ep,
                             local_buffer.data(),
                             local_buffer.size(),
                             nullptr,
                             FI_ADDR_UNSPEC,
                             static_cast<std::uint64_t>(remote_addr),
                             static_cast<std::uint64_t>(key),
                             ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto endpoint::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->ep : nullptr;
}

// ============================================================================
// endpoint_bind.cpp implementation
// ============================================================================

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

auto endpoint::bound_cq(completion_direction direction) const noexcept -> const completion_queue* {
    if (!impl_ || !impl_->ep)
        return nullptr;
    const detail::completion_queue_impl* queue{};
    switch (direction) {
        case completion_direction::transmit:
            queue = impl_->tx_cq.owner;
            break;
        case completion_direction::receive:
            queue = impl_->rx_cq.owner;
            break;
        default:
            return nullptr;
    }
    return queue ? queue->wrapper : nullptr;
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

// ============================================================================
// endpoint_options.cpp implementation
// ============================================================================

namespace {

/// @brief Translates loom endpoint option names to libfabric option names.
constexpr auto translate_optname(ep_opt::option optname) -> std::optional<int> {
    switch (optname) {
        case ep_opt::min_multi_recv:
            return FI_OPT_MIN_MULTI_RECV;
        case ep_opt::cm_data_size:
            return FI_OPT_CM_DATA_SIZE;
        case ep_opt::buffered_min:
            return FI_OPT_BUFFERED_MIN;
        case ep_opt::buffered_limit:
            return FI_OPT_BUFFERED_LIMIT;
        case ep_opt::shared_memory_permitted:
            return FI_OPT_SHARED_MEMORY_PERMITTED;
        case ep_opt::cuda_api_permitted:
            return FI_OPT_CUDA_API_PERMITTED;
        case ep_opt::efa_emulated_read:
            return FI_OPT_EFA_EMULATED_READ;
        case ep_opt::efa_emulated_write:
            return FI_OPT_EFA_EMULATED_WRITE;
        case ep_opt::efa_write_in_order_aligned_128_bytes:
            return FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES;
    }
    return std::nullopt;
}

}  // namespace

auto set_endpoint_option(endpoint& ep,
                         ep_opt::level level,
                         ep_opt::option optname,
                         const void* optval,
                         std::size_t optlen) -> result<void> {
    const auto native_option = translate_optname(optname);
    if (level != ep_opt::level_endpoint || !native_option)
        return make_error_result<void>(errc::invalid_argument);
    return set_native_endpoint_option(ep, FI_OPT_ENDPOINT, *native_option, optval, optlen);
}

auto get_endpoint_option(const endpoint& ep,
                         ep_opt::level level,
                         ep_opt::option optname,
                         void* optval,
                         std::size_t* optlen) -> result<void> {
    const auto native_option = translate_optname(optname);
    if (level != ep_opt::level_endpoint || !native_option)
        return make_error_result<void>(errc::invalid_argument);
    return get_native_endpoint_option(ep, FI_OPT_ENDPOINT, *native_option, optval, optlen);
}

auto set_native_endpoint_option(
    endpoint& ep, int level, int optname, const void* optval, std::size_t optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(ep_ptr->ops, fi_ops_ep, setopt)) {
        return make_error_result<void>(errc::not_supported);
    }

    int ret = ::fi_setopt(&ep_ptr->fid, level, optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto get_native_endpoint_option(
    const endpoint& ep, int level, int optname, void* optval, std::size_t* optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(ep_ptr->ops, fi_ops_ep, getopt)) {
        return make_error_result<void>(errc::not_supported);
    }

    int ret = ::fi_getopt(&ep_ptr->fid, level, optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
