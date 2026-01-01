// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/memory.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"

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
 * @struct memory_region_impl
 * @brief Internal implementation for memory_region.
 */
struct memory_region_impl {
    fid_mr* mr{nullptr};    ///< Libfabric memory region handle
    void* addr{nullptr};    ///< Registered memory address
    std::size_t length{0};  ///< Length of registered memory in bytes

    /**
     * @brief Default constructor.
     */
    memory_region_impl() = default;

    /**
     * @brief Constructs implementation with libfabric MR handle and buffer info.
     * @param mr_ The libfabric memory region handle.
     * @param addr_ The registered memory address.
     * @param len_ The length of the registered memory.
     */
    memory_region_impl(fid_mr* mr_, void* addr_, std::size_t len_)
        : mr(mr_), addr(addr_), length(len_) {}

    /**
     * @brief Destructor that closes the libfabric MR handle.
     */
    ~memory_region_impl() {
        if (mr) {
            fi_close(&mr->fid);
        }
    }

    /**
     * @brief Deleted copy constructor.
     */
    memory_region_impl(const memory_region_impl&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const memory_region_impl&) -> memory_region_impl& = delete;
    /**
     * @brief Deleted move constructor.
     */
    memory_region_impl(memory_region_impl&&) = delete;
    /**
     * @brief Deleted move assignment operator.
     */
    auto operator=(memory_region_impl&&) -> memory_region_impl& = delete;
};
}  // namespace detail

memory_region::memory_region(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

memory_region::~memory_region() = default;

memory_region::memory_region(memory_region&&) noexcept = default;

auto memory_region::operator=(memory_region&&) noexcept -> memory_region& = default;

auto memory_region::impl_valid() const noexcept -> bool {
    return impl_ && impl_->mr;
}

auto memory_region::register_memory(domain& dom,
                                    std::span<std::byte> buffer,
                                    mr_access access,
                                    memory_resource* resource) -> result<memory_region> {
    return register_memory(dom, buffer.data(), buffer.size(), access, resource);
}

auto memory_region::register_memory(domain& dom,
                                    void* addr,
                                    std::size_t length,
                                    mr_access access,
                                    memory_resource* resource) -> result<memory_region> {
    auto* fid_dom = static_cast<fid_domain*>(dom.internal_ptr());

    fid_mr* mr = nullptr;
    auto ret = fi_mr_reg(fid_dom, addr, length, access.get(), 0, 0, 0, &mr, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<memory_region>(-ret);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::memory_region_impl>(resource, mr, addr, length);
        return memory_region{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::memory_region_impl>(default_resource, mr, addr, length);
    return memory_region{std::move(impl)};
}

auto memory_region::register_hmem(domain& dom,
                                  void* addr,
                                  std::size_t length,
                                  mr_access access,
                                  hmem_device device,
                                  memory_resource* resource) -> result<memory_region> {
    auto* fid_dom = static_cast<fid_domain*>(dom.internal_ptr());

    fi_mr_attr mr_attr{};
    mr_attr.mr_iov = nullptr;
    mr_attr.iov_count = 0;
    mr_attr.access = access.get();
    mr_attr.offset = 0;
    mr_attr.requested_key = 0;
    mr_attr.context = nullptr;
    mr_attr.auth_key_size = 0;
    mr_attr.auth_key = nullptr;
    mr_attr.iface = static_cast<fi_hmem_iface>(device.iface);
    mr_attr.hmem_data = device.hmem_data;

    iovec iov{};
    iov.iov_base = addr;
    iov.iov_len = length;
    mr_attr.mr_iov = &iov;
    mr_attr.iov_count = 1;

    fid_mr* mr = nullptr;
    auto ret = fi_mr_regattr(fid_dom, &mr_attr, 0, &mr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<memory_region>(-ret);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::memory_region_impl>(resource, mr, addr, length);
        return memory_region{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::memory_region_impl>(default_resource, mr, addr, length);
    return memory_region{std::move(impl)};
}

auto memory_region::register_dmabuf(domain& dom,
                                    void* addr,
                                    std::size_t length,
                                    mr_access access,
                                    int fd,
                                    std::uint64_t offset,
                                    memory_resource* resource) -> result<memory_region> {
    auto* fid_dom = static_cast<fid_domain*>(dom.internal_ptr());

    fi_mr_dmabuf dmabuf{};
    dmabuf.fd = fd;
    dmabuf.offset = offset;
    dmabuf.len = length;
    dmabuf.base_addr = addr;

    fi_mr_attr mr_attr{};
    mr_attr.dmabuf = &dmabuf;
    mr_attr.iov_count = 1;
    mr_attr.access = access.get() | FI_MR_DMABUF;
    mr_attr.offset = offset;
    mr_attr.requested_key = 0;
    mr_attr.context = nullptr;

    fid_mr* mr = nullptr;
    auto ret = fi_mr_regattr(fid_dom, &mr_attr, 0, &mr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<memory_region>(-ret);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::memory_region_impl>(resource, mr, addr, length);
        return memory_region{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::memory_region_impl>(default_resource, mr, addr, length);
    return memory_region{std::move(impl)};
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

auto memory_region::length() const noexcept -> std::size_t {
    return impl_ ? impl_->length : 0;
}

auto memory_region::bind(endpoint& ep) -> result<void> {
    if (!impl_ || !impl_->mr) {
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
