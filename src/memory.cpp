// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/memory.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#include <utility>

#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/detail/conversions.hpp"
#include "loom/detail/impl/memory_impl.hpp"
#include "loom/detail/unique_fid.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

memory_region::memory_region() = default;

memory_region::memory_region(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

memory_region::~memory_region() = default;

memory_region::memory_region(memory_region&&) noexcept = default;

auto memory_region::operator=(memory_region&&) noexcept -> memory_region& = default;

auto memory_region::impl_valid() const noexcept -> bool {
    return impl_ && impl_->mr;
}

auto memory_region::register_memory(const domain& dom,
                                    std::span<std::byte> buffer,
                                    mr_access access,
                                    memory_resource* resource) -> result<memory_region> {
    return register_memory(dom, buffer.data(), buffer.size(), access, resource);
}

auto memory_region::register_memory(const domain& dom,
                                    void* addr,
                                    std::size_t length,
                                    mr_access access,
                                    memory_resource* resource) -> result<memory_region> {
    if (!dom) {
        return make_error_result<memory_region>(errc::invalid_argument);
    }
    auto remote_address = detail::mr_remote_address(dom.impl_->mr_mode, addr);
    if (!remote_address) {
        return std::unexpected(remote_address.error());
    }
    auto requested_key = detail::mr_key_lease::acquire(dom.impl_->mr_keys);
    if (!requested_key) {
        return std::unexpected(requested_key.error());
    }
    auto* fid_dom = static_cast<fid_domain*>(dom.internal_ptr());

    fid_mr* mr = nullptr;
    auto ret = fi_mr_reg(fid_dom,
                         addr,
                         length,
                         detail::to_fi_mr_access(access),
                         0,
                         requested_key->get(),
                         0,
                         &mr,
                         nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<memory_region>(-ret);
    }

    detail::unique_fid<fid_mr> pending{mr};
    auto impl = detail::make_pmr_unique<detail::memory_region_impl>(
        resource ? resource : get_default_resource(),
        mr,
        addr,
        length,
        std::move(*requested_key),
        *remote_address);
    (void)pending.release();
    return memory_region{std::move(impl)};
}

auto memory_region::register_memory(const domain& dom,
                                    void* addr,
                                    std::size_t length,
                                    const memory_registration& attributes,
                                    memory_resource* resource) -> result<memory_region> {
    if (!dom || attributes.device.hmem_data || !std::in_range<int>(attributes.device.device_id) ||
        (attributes.dmabuf && attributes.dmabuf->fd < 0)) {
        return make_error_result<memory_region>(errc::invalid_argument);
    }
    fi_mr_attr mr_attr{};
    const auto ordinal = static_cast<int>(attributes.device.device_id);
    switch (attributes.device.iface) {
        case hmem_iface::system:
            mr_attr.iface = FI_HMEM_SYSTEM;
            break;
        case hmem_iface::cuda:
            mr_attr.iface = FI_HMEM_CUDA;
            mr_attr.device.cuda = ordinal;
            break;
        case hmem_iface::rocr:
#if FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION) >= FI_VERSION(2, 5)
            mr_attr.iface = FI_HMEM_ROCR;
            mr_attr.device.rocr = ordinal;
            break;
#else
            return make_error_result<memory_region>(errc::not_supported);
#endif
        case hmem_iface::level_zero:
            mr_attr.iface = FI_HMEM_ZE;
            mr_attr.device.ze = ordinal;
            break;
        case hmem_iface::neuron:
            mr_attr.iface = FI_HMEM_NEURON;
            mr_attr.device.neuron = ordinal;
            break;
        case hmem_iface::synapseai:
            mr_attr.iface = FI_HMEM_SYNAPSEAI;
            mr_attr.device.synapseai = ordinal;
            break;
        default:
            return make_error_result<memory_region>(errc::invalid_argument);
    }
    std::uint64_t flags{};
    switch (attributes.allocation) {
        case hmem_allocation_kind::unspecified:
            break;
        case hmem_allocation_kind::device:
            flags = FI_HMEM_DEVICE_ONLY;
            break;
        case hmem_allocation_kind::host:
            flags = FI_HMEM_HOST_ALLOC;
            break;
        default:
            return make_error_result<memory_region>(errc::invalid_argument);
    }
    if (flags && attributes.device.iface == hmem_iface::system) {
        return make_error_result<memory_region>(errc::invalid_argument);
    }
    const auto local_address =
        attributes.dmabuf ? detail::dmabuf_local_address(addr, attributes.dmabuf->offset, length)
                          : result<void*>{addr};
    if (!local_address)
        return std::unexpected(local_address.error());
    auto remote_address = detail::mr_remote_address(
        dom.impl_->mr_mode, addr, attributes.dmabuf ? attributes.dmabuf->offset : 0);
    if (!remote_address) {
        return std::unexpected(remote_address.error());
    }
    auto requested_key = detail::mr_key_lease::acquire(dom.impl_->mr_keys);
    if (!requested_key) {
        return std::unexpected(requested_key.error());
    }
    iovec iov{.iov_base = addr, .iov_len = length};
    fi_mr_dmabuf dmabuf{};
    if (attributes.dmabuf) {
        dmabuf.fd = attributes.dmabuf->fd;
        dmabuf.offset = attributes.dmabuf->offset;
        dmabuf.len = length;
        dmabuf.base_addr = addr;
        mr_attr.dmabuf = &dmabuf;
        flags |= FI_MR_DMABUF;
    } else {
        mr_attr.mr_iov = &iov;
    }
    mr_attr.iov_count = 1;
    mr_attr.access = detail::to_fi_mr_access(attributes.access);
    mr_attr.requested_key = requested_key->get();

    fid_mr* mr = nullptr;
    const auto ret =
        fi_mr_regattr(static_cast<fid_domain*>(dom.internal_ptr()), &mr_attr, flags, &mr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<memory_region>(-ret);
    }
    detail::unique_fid<fid_mr> pending{mr};
    auto impl = detail::make_pmr_unique<detail::memory_region_impl>(
        resource ? resource : get_default_resource(),
        mr,
        *local_address,
        length,
        std::move(*requested_key),
        *remote_address);
    (void)pending.release();
    return memory_region{std::move(impl)};
}

auto memory_region::register_hmem(const domain& dom,
                                  void* addr,
                                  std::size_t length,
                                  mr_access access,
                                  hmem_device device,
                                  memory_resource* resource) -> result<memory_region> {
    return register_memory(
        dom, addr, length, memory_registration{.access = access, .device = device}, resource);
}

auto memory_region::register_dmabuf(const domain& dom,
                                    void* addr,
                                    std::size_t length,
                                    mr_access access,
                                    int fd,
                                    std::uint64_t offset,
                                    memory_resource* resource) -> result<memory_region> {
    return register_memory(
        dom,
        addr,
        length,
        memory_registration{.access = access, .dmabuf = dmabuf_registration{fd, offset}},
        resource);
}

auto memory_region::descriptor() const noexcept -> mr_descriptor {
    if (!impl_ || !impl_->mr) {
        return mr_descriptor{};
    }
    return mr_descriptor{fi_mr_desc(impl_->mr)};
}

auto memory_region::key() const noexcept -> std::uint64_t {
    if (!impl_ || !impl_->mr) {
        return 0;
    }
    return fi_mr_key(impl_->mr);
}

auto memory_region::address() const noexcept -> void* {
    return impl_ ? impl_->addr : nullptr;
}

auto memory_region::remote_address() const noexcept -> std::uint64_t {
    return impl_ ? impl_->remote_addr : 0;
}

auto memory_region::length() const noexcept -> std::size_t {
    return impl_ ? impl_->length : 0;
}

auto memory_region::contains(const void* buffer, std::size_t size) const noexcept -> bool {
    if (!impl_ || !impl_->mr || !impl_->addr)
        return false;
    const auto base = reinterpret_cast<std::uintptr_t>(impl_->addr);
    const auto address = reinterpret_cast<std::uintptr_t>(buffer);
    return address >= base && address - base <= impl_->length &&
           size <= impl_->length - (address - base) &&
           size <= std::numeric_limits<std::uintptr_t>::max() - address;
}

auto memory_region::bind(endpoint& ep) -> result<void> {
    if (!impl_ || !impl_->mr || !ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());
    auto ret = fi_mr_bind(impl_->mr, &fid_ep->fid, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto memory_region::enable() -> result<void> {
    if (!impl_ || !impl_->mr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto ret = fi_mr_enable(impl_->mr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto memory_region::refresh() -> result<void> {
    if (!impl_ || !impl_->mr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto ret = fi_mr_refresh(impl_->mr, nullptr, 0, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto memory_region::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->mr : nullptr;
}

}  // namespace loom
