// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>

#include <array>

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
#include "loom/core/shared_context.hpp"
#include "loom/detail/conversions.hpp"

namespace loom {
class completion_queue;
}  // namespace loom

namespace loom::detail {

/// @brief Internal implementation of the endpoint.
struct endpoint_impl {
    ::fid_ep* ep{nullptr};                    ///< Underlying libfabric endpoint handle.
    endpoint_type type{endpoint_types::msg};  ///< Endpoint type (msg, rdm, dgram).
    address_format addr_format{address_format::unspecified};  ///< Address format.

    /// @brief Default constructor.
    endpoint_impl() = default;

    /// @brief Constructs endpoint_impl with a libfabric endpoint handle.
    /// @param ep_ The libfabric endpoint pointer.
    /// @param type_ The endpoint type.
    /// @param fmt The address format.
    endpoint_impl(::fid_ep* ep_, endpoint_type type_, address_format fmt)
        : ep(ep_), type(type_), addr_format(fmt) {}

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

inline endpoint::endpoint() = default;

inline endpoint::endpoint(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

inline endpoint::~endpoint() = default;

inline endpoint::endpoint(endpoint&&) noexcept = default;

inline auto endpoint::operator=(endpoint&&) noexcept -> endpoint& = default;

inline auto endpoint::impl_valid() const noexcept -> bool {
    return impl_ && impl_->ep;
}

inline auto endpoint::create(const domain& dom, const fabric_info& info, memory_resource* resource)
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

    auto ep_type = detail::from_fi_ep_type(info_ptr->ep_attr->type);
    auto addr_fmt = detail::from_fi_addr_format(info_ptr->addr_format);

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::endpoint_impl>(resource, ep, ep_type, addr_fmt);
        return endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::endpoint_impl>(default_resource, ep, ep_type, addr_fmt);
    return endpoint{std::move(impl)};
}

inline auto endpoint::enable() -> result<void> {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_enable(impl_->ep);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

inline auto endpoint::get_type() const -> result<endpoint_type> {
    if (!impl_) {
        return make_error_result<endpoint_type>(errc::invalid_argument);
    }

    return impl_->type;
}

inline auto endpoint::send(std::span<const std::byte> data, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret =
        ::fi_send(impl_->ep, data.data(), data.size(), nullptr, FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::recv(std::span<std::byte> buffer, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret =
        ::fi_recv(impl_->ep, buffer.data(), buffer.size(), nullptr, FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::sendv(std::span<const iovec> iov, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_sendv(impl_->ep, iov.data(), nullptr, iov.size(), FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::recvv(std::span<const iovec> iov, context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_recvv(impl_->ep, iov.data(), nullptr, iov.size(), FI_ADDR_UNSPEC, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::tagged_send(std::span<const std::byte> data,
                                   std::uint64_t tag,
                                   context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret =
        ::fi_tsend(impl_->ep, data.data(), data.size(), nullptr, FI_ADDR_UNSPEC, tag, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::tagged_recv(std::span<std::byte> buffer,
                                   std::uint64_t tag,
                                   std::uint64_t ignore,
                                   context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_trecv(
        impl_->ep, buffer.data(), buffer.size(), nullptr, FI_ADDR_UNSPEC, tag, ignore, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::inject(std::span<const std::byte> data, fabric_addr dest_addr)
    -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_inject(impl_->ep, data.data(), data.size(), dest_addr.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::inject_data(std::span<const std::byte> data,
                                   std::uint64_t immediate_data,
                                   fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret =
        ::fi_injectdata(impl_->ep, data.data(), data.size(), immediate_data, dest_addr.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::tagged_inject(std::span<const std::byte> data,
                                     std::uint64_t tag,
                                     fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_tinject(impl_->ep, data.data(), data.size(), dest_addr.get(), tag);

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::tagged_inject_data(std::span<const std::byte> data,
                                          std::uint64_t tag,
                                          std::uint64_t immediate_data,
                                          fabric_addr dest_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_tinjectdata(
        impl_->ep, data.data(), data.size(), immediate_data, dest_addr.get(), tag);

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::cancel(context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_cancel(&impl_->ep->fid, ctx.get());

    if (ret < 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

inline auto endpoint::connect(const address& peer_addr) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    const void* addr_ptr = get_address_data(peer_addr);

    int ret = ::fi_connect(impl_->ep, addr_ptr, nullptr, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

inline auto endpoint::accept() -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_accept(impl_->ep, nullptr, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

inline auto endpoint::get_local_address() const -> result<address> {
    if (!impl_ || !impl_->ep) {
        return make_error_result<address>(errc::invalid_argument);
    }

    std::array<std::byte, 256> raw_addr{};
    std::size_t addrlen = raw_addr.size();

    int ret = ::fi_getname(&impl_->ep->fid, raw_addr.data(), &addrlen);

    if (ret != 0) {
        return make_error_result_from_fi_errno<address>(-ret);
    }

    return address_from_raw(raw_addr.data(), addrlen, impl_->addr_format);
}

inline auto endpoint::get_peer_address() const -> result<address> {
    if (!impl_ || !impl_->ep) {
        return make_error_result<address>(errc::invalid_argument);
    }

    std::array<std::byte, 256> raw_addr{};
    std::size_t addrlen = raw_addr.size();

    int ret = ::fi_getpeer(impl_->ep, raw_addr.data(), &addrlen);

    if (ret != 0) {
        return make_error_result_from_fi_errno<address>(-ret);
    }

    return address_from_raw(raw_addr.data(), addrlen, impl_->addr_format);
}

inline auto endpoint::read(std::span<std::byte> local_buffer,
                            rma_addr remote_addr,
                            mr_key key,
                            context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ssize_t ret = ::fi_read(impl_->ep,
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

inline auto endpoint::write(std::span<const std::byte> local_buffer,
                             rma_addr remote_addr,
                             mr_key key,
                             context_ptr<void> ctx) -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
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

inline auto endpoint::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->ep : nullptr;
}

// ============================================================================
// endpoint_bind.cpp implementation
// ============================================================================

inline auto endpoint::bind_cq(completion_queue& cq, cq_bind_flags flags) -> void_result {
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

    return make_success();
}

inline auto endpoint::bind_tx_cq(completion_queue& cq) -> void_result {
    return bind_cq(cq, cq_bind::transmit);
}

inline auto endpoint::bind_rx_cq(completion_queue& cq) -> void_result {
    return bind_cq(cq, cq_bind::recv);
}

inline auto endpoint::bind_eq(event_queue& eq, std::uint64_t flags) -> void_result {
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

inline auto endpoint::bind_av(address_vector& av, std::uint64_t flags) -> void_result {
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

inline auto endpoint::bind_counter(counter& cntr, std::uint64_t flags) -> void_result {
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

inline auto endpoint::bind_stx(shared_tx_context& stx) -> void_result {
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

inline auto endpoint::bind_srx(shared_rx_context& srx) -> void_result {
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
inline auto translate_optname(int optname) -> int {
    switch (optname) {
        case ep_opt::min_multi_recv:
            return FI_OPT_MIN_MULTI_RECV;
        case ep_opt::cm_data_size:
            return FI_OPT_CM_DATA_SIZE;
        case ep_opt::buffered_min:
            return FI_OPT_BUFFERED_MIN;
        case ep_opt::buffered_limit:
            return FI_OPT_BUFFERED_LIMIT;
#ifdef FI_OPT_SHARED_MEMORY_PERMITTED
        case ep_opt::shared_memory_permitted:
            return FI_OPT_SHARED_MEMORY_PERMITTED;
#endif
#ifdef FI_OPT_CUDA_API_PERMITTED
        case ep_opt::cuda_api_permitted:
            return FI_OPT_CUDA_API_PERMITTED;
#endif
#ifdef FI_OPT_EFA_EMULATED_READ
        case ep_opt::efa_emulated_read:
            return FI_OPT_EFA_EMULATED_READ;
#endif
#ifdef FI_OPT_EFA_EMULATED_WRITE
        case ep_opt::efa_emulated_write:
            return FI_OPT_EFA_EMULATED_WRITE;
#endif
#ifdef FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES
        case ep_opt::efa_write_in_order_aligned_128_bytes:
            return FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES;
#endif
        default:
            return optname;
    }
}

}  // namespace

inline auto set_endpoint_option(
    endpoint& ep, int level, int optname, const void* optval, std::size_t optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int fi_level = (level == ep_opt::level_endpoint) ? FI_OPT_ENDPOINT : level;
    int fi_optname = translate_optname(optname);

    int ret = ::fi_setopt(&ep_ptr->fid, fi_level, fi_optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

inline auto get_endpoint_option(
    const endpoint& ep, int level, int optname, void* optval, std::size_t* optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int fi_level = (level == ep_opt::level_endpoint) ? FI_OPT_ENDPOINT : level;
    int fi_optname = translate_optname(optname);

    int ret = ::fi_getopt(&ep_ptr->fid, fi_level, fi_optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
