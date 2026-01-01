// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/passive_endpoint.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_endpoint.h>

#include <array>

#include "loom/core/address.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/error.hpp"
#include "loom/core/event_queue.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/conversions.hpp"

namespace loom {

namespace detail {

/// @brief Internal implementation of the passive endpoint.
struct passive_endpoint_impl {
    ::fid_pep* pep{nullptr};  ///< Underlying libfabric passive endpoint handle.
    address_format addr_format{address_format::unspecified};  ///< Address format for the endpoint.

    /// @brief Default constructor.
    passive_endpoint_impl() = default;

    /// @brief Constructs passive_endpoint_impl with a libfabric PEP handle and address format.
    /// @param pep_ The libfabric passive endpoint pointer.
    /// @param fmt The address format.
    passive_endpoint_impl(::fid_pep* pep_, address_format fmt) : pep(pep_), addr_format(fmt) {}

    /// @brief Destructor that closes the libfabric passive endpoint.
    ~passive_endpoint_impl() {
        if (pep) {
            ::fi_close(&pep->fid);
        }
    }

    passive_endpoint_impl(const passive_endpoint_impl&) = delete;
    auto operator=(const passive_endpoint_impl&) -> passive_endpoint_impl& = delete;
    passive_endpoint_impl(passive_endpoint_impl&&) = delete;
    auto operator=(passive_endpoint_impl&&) -> passive_endpoint_impl& = delete;
};
}  // namespace detail

passive_endpoint::passive_endpoint(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

passive_endpoint::~passive_endpoint() = default;

passive_endpoint::passive_endpoint(passive_endpoint&&) noexcept = default;

auto passive_endpoint::operator=(passive_endpoint&&) noexcept -> passive_endpoint& = default;

auto passive_endpoint::impl_valid() const noexcept -> bool {
    return impl_ && impl_->pep;
}

auto passive_endpoint::create(const fabric& fab, const fabric_info& info, memory_resource* resource)
    -> result<passive_endpoint> {
    if (!fab || !info) {
        return make_error_result<passive_endpoint>(errc::invalid_argument);
    }

    ::fid_pep* pep = nullptr;
    auto* fab_ptr = static_cast<::fid_fabric*>(fab.internal_ptr());
    auto* info_ptr = static_cast<::fi_info*>(info.internal_ptr());

    int ret = ::fi_passive_ep(fab_ptr, info_ptr, &pep, nullptr);

    if (ret != 0) {
        return make_error_result_from_fi_errno<passive_endpoint>(-ret);
    }

    auto addr_fmt = detail::from_fi_addr_format(info_ptr->addr_format);

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::passive_endpoint_impl>(resource, pep, addr_fmt);
        return passive_endpoint{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::passive_endpoint_impl>(default_resource, pep, addr_fmt);
    return passive_endpoint{std::move(impl)};
}

auto passive_endpoint::bind_eq(event_queue& eq, std::uint64_t flags) -> void_result {
    if (!impl_ || !impl_->pep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!eq) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* eq_fid = static_cast<::fid*>(eq.internal_ptr());
    int ret = ::fi_pep_bind(impl_->pep, eq_fid, flags);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto passive_endpoint::listen() -> void_result {
    if (!impl_ || !impl_->pep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_listen(impl_->pep);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto passive_endpoint::get_local_address() const -> result<address> {
    if (!impl_ || !impl_->pep) {
        return make_error_result<address>(errc::invalid_argument);
    }

    std::array<std::byte, 256> raw_addr{};
    std::size_t addrlen = raw_addr.size();

    int ret = ::fi_getname(&impl_->pep->fid, raw_addr.data(), &addrlen);

    if (ret != 0) {
        return make_error_result_from_fi_errno<address>(-ret);
    }

    return address_from_raw(raw_addr.data(), addrlen, impl_->addr_format);
}

auto passive_endpoint::reject(const void* handle) -> void_result {
    if (!impl_ || !impl_->pep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!handle) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_reject(impl_->pep, static_cast<::fid_t>(const_cast<void*>(handle)), nullptr, 0);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto passive_endpoint::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->pep : nullptr;
}

}  // namespace loom
