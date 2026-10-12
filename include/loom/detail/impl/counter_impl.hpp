// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

#include <cstring>
#include <limits>

#include "loom/core/allocator.hpp"
#include "loom/core/counter.hpp"
#include "loom/core/error.hpp"
#include "loom/detail/impl/domain_impl.hpp"

namespace loom::detail {

/// @brief Internal implementation of the counter.
struct counter_impl {
    ::fid_cntr* cntr{nullptr};  ///< Underlying libfabric counter handle.
    std::uint64_t threshold{1};  ///< Configured threshold for waits and checks.
    bool wait_enabled{false};    ///< Whether creation requested a native wait object.
    std::uint64_t max_value{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_error_value{std::numeric_limits<std::uint64_t>::max()};

    /// @brief Default constructor.
    counter_impl() = default;

    /// @brief Constructs counter_impl with a libfabric counter handle.
    /// @param cntr_ptr The libfabric counter pointer.
    explicit counter_impl(::fid_cntr* cntr_ptr, std::uint64_t target = 1, bool can_wait = false)
        : cntr(cntr_ptr), threshold(target), wait_enabled(can_wait) {}

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

}  // namespace loom::detail

#ifdef LOOM_IMPLEMENTATION

namespace loom {

counter::counter() = default;

counter::counter(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

counter::~counter() = default;

counter::counter(counter&&) noexcept = default;

auto counter::operator=(counter&&) noexcept -> counter& = default;

auto counter::impl_valid() const noexcept -> bool {
    return impl_ && impl_->cntr != nullptr;
}

auto counter::create(const domain& dom, const counter_attr& attr, memory_resource* resource)
    -> result<counter> {
    if (!dom || attr.initial > dom.impl_->max_counter_value ||
        attr.threshold > dom.impl_->max_counter_value) {
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

    auto close = [](::fid_cntr* handle) noexcept {
        ::fi_close(&handle->fid);
    };
    std::unique_ptr<::fid_cntr, decltype(close)> pending{cntr, close};

    if (attr.initial != 0) {
        ret = ::fi_cntr_set(cntr, attr.initial);
        if (ret != 0) {
            return make_error_result_from_fi_errno<counter>(-ret);
        }
    }

    auto impl = detail::make_pmr_unique<detail::counter_impl>(
        resource ? resource : get_default_resource(), cntr, attr.threshold, attr.wait_obj);
    impl->max_value = dom.impl_->max_counter_value;
    impl->max_error_value = dom.impl_->max_error_counter_value;
    (void)pending.release();
    return counter{std::move(impl)};
}

auto counter::read() const -> result<std::uint64_t> {
    if (!impl_ || !impl_->cntr) {
        return make_error_result<std::uint64_t>(errc::invalid_argument);
    }

    std::uint64_t value = ::fi_cntr_read(impl_->cntr);
    return value;
}

auto counter::read_error_count() const -> result<std::uint64_t> {
    if (!impl_ || !impl_->cntr)
        return make_error_result<std::uint64_t>(errc::invalid_argument);
    if (!FI_CHECK_OP(impl_->cntr->ops, fi_ops_cntr, readerr))
        return make_error_result<std::uint64_t>(errc::not_supported);
    return ::fi_cntr_readerr(impl_->cntr);
}

auto counter::set_error_count(std::uint64_t value) -> void_result {
    if (!impl_ || !impl_->cntr || value > impl_->max_error_value)
        return make_error_result<void>(errc::invalid_argument);
    if (!FI_CHECK_OP(impl_->cntr->ops, fi_ops_cntr, seterr))
        return make_error_result<void>(errc::not_supported);
    const auto status = ::fi_cntr_seterr(impl_->cntr, value);
    if (status != 0)
        return make_error_result_from_fi_errno<void>(-status);
    return make_success();
}

auto counter::add_error_count(std::uint64_t value) -> void_result {
    if (!impl_ || !impl_->cntr || value > impl_->max_error_value)
        return make_error_result<void>(errc::invalid_argument);
    if (!FI_CHECK_OP(impl_->cntr->ops, fi_ops_cntr, adderr))
        return make_error_result<void>(errc::not_supported);
    const auto status = ::fi_cntr_adderr(impl_->cntr, value);
    if (status != 0)
        return make_error_result_from_fi_errno<void>(-status);
    return make_success();
}

auto counter::max_value() const noexcept -> std::uint64_t {
    return impl_ && impl_->cntr ? impl_->max_value : 0;
}

auto counter::max_error_value() const noexcept -> std::uint64_t {
    return impl_ && impl_->cntr ? impl_->max_error_value : 0;
}

auto counter::set(std::uint64_t value) -> void_result {
    if (!impl_ || !impl_->cntr || value > impl_->max_value) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_cntr_set(impl_->cntr, value);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto counter::add(std::uint64_t value) -> void_result {
    if (!impl_ || !impl_->cntr || value > impl_->max_value) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_cntr_add(impl_->cntr, value);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto counter::wait(std::optional<std::chrono::milliseconds> timeout) -> void_result {
    return wait_until(impl_ ? impl_->threshold : 0, timeout);
}

auto counter::wait_until(std::uint64_t threshold, std::optional<std::chrono::milliseconds> timeout)
    -> void_result {
    if (!impl_ || !impl_->cntr || !impl_->wait_enabled || threshold > impl_->max_value) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int timeout_ms = -1;
    if (timeout && timeout->count() >= 0) {
        if (timeout->count() > std::numeric_limits<int>::max()) {
            return make_error_result<void>(errc::invalid_argument);
        }
        timeout_ms = static_cast<int>(timeout->count());
    }

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
    return value >= impl_->threshold;
}

auto counter::get_error() const -> std::error_code {
    const auto count = read_error_count();
    if (!count)
        return count.error();
    return *count ? make_error_code(errc::io_error) : std::error_code{};
}

auto counter::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->cntr : nullptr;
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
