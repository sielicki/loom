// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/endpoint.hpp"

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
#include "loom/core/error.hpp"
#include "loom/core/event_queue.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/impl/endpoint_impl.hpp"
#include "loom/detail/native_address.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom {
class completion_queue;
}  // namespace loom

namespace loom {

endpoint::endpoint() = default;

endpoint::endpoint(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

endpoint::~endpoint() = default;

endpoint::endpoint(endpoint&&) noexcept = default;

auto endpoint::operator=(endpoint&&) noexcept -> endpoint& = default;

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

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::endpoint_impl>(
            resource, ep, ep_type, addr_fmt, max_msg_size, inject_size, capabilities);
        (void)pending.release();
        return endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::endpoint_impl>(
        default_resource, ep, ep_type, addr_fmt, max_msg_size, inject_size, capabilities);
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

    const auto native = detail::to_native_address(peer_addr, impl_->addr_format);
    if (!native) {
        return std::unexpected(native.error());
    }
    const int ret = std::visit(
        [&](const auto& addr) { return ::fi_connect(impl_->ep, &addr, nullptr, 0); }, *native);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto endpoint::accept() -> void_result {
    if (!impl_ || !impl_->ep) {
        return make_error_result<void>(errc::invalid_argument);
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

    std::array<std::byte, 256> raw_addr{};
    std::size_t addrlen = raw_addr.size();

    int ret = ::fi_getname(&impl_->ep->fid, raw_addr.data(), &addrlen);

    if (ret != 0) {
        return make_error_result_from_fi_errno<address>(-ret);
    }

    return address_from_raw(raw_addr.data(), addrlen, impl_->addr_format);
}

auto endpoint::get_peer_address() const -> result<address> {
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

auto endpoint::read(std::span<std::byte> local_buffer,
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

auto endpoint::write(std::span<const std::byte> local_buffer,
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

auto endpoint::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->ep : nullptr;
}

}  // namespace loom
