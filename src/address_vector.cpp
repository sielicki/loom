// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/address_vector.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

#include <cstring>

#include "loom/core/allocator.hpp"
#include "loom/core/error.hpp"

namespace loom {

namespace detail {

/// @brief Internal implementation of the address vector.
struct address_vector_impl {
    ::fid_av* av{nullptr};                                    ///< Underlying libfabric AV handle.
    av_type type{av_type::map};                               ///< Address vector type.
    std::size_t addr_count{0};                                ///< Number of addresses inserted.
    address_format addr_format{address_format::unspecified};  ///< Address format.

    /// @brief Default constructor.
    address_vector_impl() = default;

    /// @brief Constructs address_vector_impl with a libfabric AV handle and attributes.
    /// @param av_ptr The libfabric address vector pointer.
    /// @param av_type The address vector type.
    /// @param fmt The address format.
    address_vector_impl(::fid_av* av_ptr, av_type av_type, address_format fmt)
        : av(av_ptr), type(av_type), addr_format(fmt) {}

    /// @brief Destructor that closes the libfabric address vector.
    ~address_vector_impl() {
        if (av != nullptr) {
            ::fi_close(&av->fid);
        }
    }

    address_vector_impl(const address_vector_impl&) = delete;
    auto operator=(const address_vector_impl&) -> address_vector_impl& = delete;
    address_vector_impl(address_vector_impl&&) = delete;
    auto operator=(address_vector_impl&&) -> address_vector_impl& = delete;
};

}  // namespace detail

/// @brief Converts loom av_type to libfabric fi_av_type.
static auto av_type_to_fi(av_type type) -> ::fi_av_type {
    switch (type) {
        case av_type::map:
            return FI_AV_MAP;
        case av_type::table:
            return FI_AV_TABLE;
        default:
            return FI_AV_UNSPEC;
    }
}

address_vector::address_vector(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

address_vector::~address_vector() = default;

address_vector::address_vector(address_vector&&) noexcept = default;

auto address_vector::operator=(address_vector&&) noexcept -> address_vector& = default;

auto address_vector::impl_valid() const noexcept -> bool {
    return impl_ && impl_->av != nullptr;
}

auto address_vector::create(const domain& dom,
                            const address_vector_attr& attr,
                            memory_resource* resource) -> result<address_vector> {
    if (!dom) {
        return make_error_result<address_vector>(errc::invalid_argument);
    }

    ::fi_av_attr av_attr{};

    av_attr.type = av_type_to_fi(attr.type);
    av_attr.count = attr.count;
    av_attr.flags = attr.flags;
    av_attr.rx_ctx_bits = static_cast<int>(attr.rx_ctx_bits);
    av_attr.ep_per_node = attr.ep_per_node;
    av_attr.name = attr.name;

    ::fid_av* av = nullptr;
    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    int ret = ::fi_av_open(domain_ptr, &av_attr, &av, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<address_vector>(-ret);
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::address_vector_impl>(
            resource, av, attr.type, attr.addr_format);
        return address_vector{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::address_vector_impl>(
        default_resource, av, attr.type, attr.addr_format);
    return address_vector{std::move(impl)};
}

auto address_vector::insert(const address& addr, void* context) -> result<av_handle> {
    if (!impl_ || !impl_->av) {
        return make_error_result<av_handle>(errc::invalid_argument);
    }

    ::fi_addr_t fi_addr = FI_ADDR_NOTAVAIL;
    std::uint64_t flags = 0;

    int ret = ::fi_av_insert(impl_->av, get_address_data(addr), 1, &fi_addr, flags, context);

    if (ret < 0) {
        return make_error_result_from_fi_errno<av_handle>(-ret);
    }

    if (ret == 0) {
        return make_error_result<av_handle>(errc::invalid_argument);
    }

    impl_->addr_count++;
    return av_handle{fi_addr};
}

auto address_vector::insert_batch(std::span<const address> addresses,
                                  std::span<av_handle> handles,
                                  std::span<void*> contexts) -> result<std::size_t> {
    if (!impl_ || !impl_->av) {
        return make_error_result<std::size_t>(errc::invalid_argument);
    }

    if (addresses.empty() || handles.size() < addresses.size()) {
        return make_error_result<std::size_t>(errc::invalid_argument);
    }

    std::vector<const void*> addr_ptrs;
    addr_ptrs.reserve(addresses.size());
    for (const auto& addr : addresses) {
        addr_ptrs.push_back(get_address_data(addr));
    }

    std::vector<::fi_addr_t> fi_addrs(addresses.size());

    std::uint64_t flags = 0;
    void* context = contexts.empty() ? nullptr : contexts.data();

    int ret = ::fi_av_insert(
        impl_->av, addr_ptrs.data(), addresses.size(), fi_addrs.data(), flags, context);

    if (ret < 0) {
        return make_error_result_from_fi_errno<std::size_t>(-ret);
    }

    auto count = static_cast<std::size_t>(ret);
    for (std::size_t i = 0; i < count; ++i) {
        handles[i] = av_handle{fi_addrs[i]};
    }

    impl_->addr_count += count;
    return count;
}

auto address_vector::remove(av_handle handle) -> void_result {
    if (!impl_ || !impl_->av) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!handle.valid()) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ::fi_addr_t fi_addr = handle.value;
    std::uint64_t flags = 0;

    int ret = ::fi_av_remove(impl_->av, &fi_addr, 1, flags);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    if (impl_->addr_count > 0) {
        impl_->addr_count--;
    }

    return make_success();
}

auto address_vector::remove_batch(std::span<const av_handle> handles) -> void_result {
    if (!impl_ || !impl_->av) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (handles.empty()) {
        return make_success();
    }

    std::vector<::fi_addr_t> fi_addrs;
    fi_addrs.reserve(handles.size());
    for (const auto& handle : handles) {
        if (handle.valid()) {
            fi_addrs.push_back(handle.value);
        }
    }

    std::uint64_t flags = 0;
    int ret = ::fi_av_remove(impl_->av, fi_addrs.data(), fi_addrs.size(), flags);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    impl_->addr_count -= fi_addrs.size();
    return make_success();
}

auto address_vector::lookup(av_handle handle) const -> result<address> {
    if (!impl_ || !impl_->av) {
        return make_error_result<address>(errc::invalid_argument);
    }

    if (!handle.valid()) {
        return make_error_result<address>(errc::invalid_argument);
    }

    std::vector<std::byte> addr_buf(256);
    ::fi_addr_t fi_addr = handle.value;
    std::size_t addrlen = addr_buf.size();

    int ret = ::fi_av_lookup(impl_->av, fi_addr, addr_buf.data(), &addrlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<address>(-ret);
    }

    return address_from_raw(addr_buf.data(), addrlen, impl_->addr_format);
}

auto address_vector::address_to_string(av_handle handle) const -> result<std::string> {
    if (!impl_ || !impl_->av) {
        return make_error_result<std::string>(errc::invalid_argument);
    }

    if (!handle.valid()) {
        return make_error_result<std::string>(errc::invalid_argument);
    }

    char buf[256];
    std::size_t len = sizeof(buf);

    std::vector<std::byte> addr_buf(256);
    ::fi_addr_t fi_addr = handle.value;
    std::size_t addrlen = addr_buf.size();

    int ret_lookup = ::fi_av_lookup(impl_->av, fi_addr, addr_buf.data(), &addrlen);
    if (ret_lookup != 0) {
        return make_error_result_from_fi_errno<std::string>(-ret_lookup);
    }

    const char* ret_str = ::fi_av_straddr(impl_->av, addr_buf.data(), buf, &len);
    if (!ret_str) {
        return make_error_result<std::string>(errc::invalid_argument);
    }

    return std::string(buf, len);
}

auto address_vector::get_type() const -> av_type {
    return impl_ ? impl_->type : av_type::unspecified;
}

auto address_vector::count() const noexcept -> std::size_t {
    return impl_ ? impl_->addr_count : 0;
}

auto address_vector::bind(endpoint&) -> void_result {
    return make_success();
}

auto address_vector::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->av : nullptr;
}

}  // namespace loom
