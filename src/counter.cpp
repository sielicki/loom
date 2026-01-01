// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/counter.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

#include <cstring>

#include "loom/core/allocator.hpp"
#include "loom/core/error.hpp"

namespace loom {

namespace detail {

/// @brief Internal implementation of the counter.
struct counter_impl {
    ::fid_cntr* cntr{nullptr};  ///< Underlying libfabric counter handle.

    /// @brief Default constructor.
    counter_impl() = default;

    /// @brief Constructs counter_impl with a libfabric counter handle.
    /// @param cntr_ptr The libfabric counter pointer.
    explicit counter_impl(::fid_cntr* cntr_ptr) : cntr(cntr_ptr) {}

    /// @brief Destructor that closes the libfabric counter.
    ~counter_impl() {
        if (cntr != nullptr) {
            ::fi_close(&cntr->fid);
        }
    }

    counter_impl(const counter_impl&) = delete;
    auto operator=(const counter_impl&) -> counter_impl& = delete;
    counter_impl(counter_impl&&) = delete;
    auto operator=(counter_impl&&) -> counter_impl& = delete;
};

}  // namespace detail

counter::counter(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

counter::~counter() = default;

counter::counter(counter&&) noexcept = default;

auto counter::operator=(counter&&) noexcept -> counter& = default;

auto counter::impl_valid() const noexcept -> bool {
    return impl_ && impl_->cntr != nullptr;
}

auto counter::create(const domain& dom, const counter_attr& attr, memory_resource* resource)
    -> result<counter> {
    if (!dom) {
        return make_error_result<counter>(errc::invalid_argument);
    }

    ::fi_cntr_attr cntr_attr{};

    cntr_attr.flags = attr.flags;
    cntr_attr.wait_obj = attr.wait_obj ? FI_WAIT_UNSPEC : FI_WAIT_NONE;
    cntr_attr.events = FI_CNTR_EVENTS_COMP;

    ::fid_cntr* cntr = nullptr;
    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    int ret = ::fi_cntr_open(domain_ptr, &cntr_attr, &cntr, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<counter>(-ret);
    }

    if (attr.initial != 0) {
        ret = ::fi_cntr_set(cntr, attr.initial);
        if (ret != 0) {
            ::fi_close(&cntr->fid);
            return make_error_result_from_fi_errno<counter>(-ret);
        }
    }

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::counter_impl>(resource, cntr);
        return counter{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::counter_impl>(default_resource, cntr);
    return counter{std::move(impl)};
}

auto counter::read() const -> result<std::uint64_t> {
    if (!impl_ || !impl_->cntr) {
        return make_error_result<std::uint64_t>(errc::invalid_argument);
    }

    std::uint64_t value = ::fi_cntr_read(impl_->cntr);
    return value;
}

auto counter::set(std::uint64_t value) -> void_result {
    if (!impl_ || !impl_->cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_cntr_set(impl_->cntr, value);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto counter::add(std::uint64_t value) -> void_result {
    if (!impl_ || !impl_->cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_cntr_add(impl_->cntr, value);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto counter::wait(std::optional<std::chrono::milliseconds> timeout) -> void_result {
    if (!impl_ || !impl_->cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int timeout_ms = -1;
    if (timeout.has_value()) {
        timeout_ms = static_cast<int>(timeout->count());
    }

    std::uint64_t threshold = 1;
    int ret = ::fi_cntr_wait(impl_->cntr, threshold, timeout_ms);

    if (ret == -FI_ETIMEDOUT) {
        return make_error_result<void>(errc::timeout);
    }

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto counter::check_threshold() const -> bool {
    if (!impl_ || !impl_->cntr) {
        return false;
    }

    std::uint64_t value = ::fi_cntr_read(impl_->cntr);
    return value > 0;
}

auto counter::get_error() const -> std::error_code {
    if (!impl_ || !impl_->cntr) {
        return make_error_code(errc::invalid_argument);
    }

    uint64_t err_count = ::fi_cntr_readerr(impl_->cntr);

    if (err_count == 0) {
        return std::error_code{};
    }

    return make_error_code(errc::io_error);
}

auto counter::ack(std::uint64_t count) -> void_result {
    if (!impl_ || !impl_->cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret =
        ::fi_cntr_add(impl_->cntr, static_cast<std::uint64_t>(-static_cast<std::int64_t>(count)));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto counter::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->cntr : nullptr;
}

}  // namespace loom
