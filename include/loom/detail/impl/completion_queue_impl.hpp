// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <ranges>
#include <system_error>

#include "loom/async/completion_queue.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom::detail {

constexpr auto to_fi_cq_format(cq_format format) noexcept -> fi_cq_format {
    switch (format) {
        case cq_format::context:
            return FI_CQ_FORMAT_CONTEXT;
        case cq_format::msg:
            return FI_CQ_FORMAT_MSG;
        case cq_format::data:
            return FI_CQ_FORMAT_DATA;
        case cq_format::tagged:
        case cq_format::unspecified:
            return FI_CQ_FORMAT_TAGGED;
    }
    return FI_CQ_FORMAT_UNSPEC;
}

template <std::size_t Capacity, typename Read>
auto read_cq_entries(fi_cq_format format, std::span<completion_event> events, Read&& read)
    -> ssize_t {
    const auto read_as = [&]<typename Entry>() -> ssize_t {
        std::array<Entry, Capacity> entries{};
        const auto result = read(entries.data(), std::min(events.size(), Capacity));
        if (result <= 0)
            return result;
        for (std::size_t i = 0; i < static_cast<std::size_t>(result); ++i) {
            const auto& entry = entries[i];
            completion_event event{};
            event.context = context_ptr<void>{entry.op_context};
            if constexpr (requires { entry.flags; }) {
                event.flags = entry.flags;
                event.len = entry.len;
                event.bytes_transferred = entry.len;
            }
            if constexpr (requires { entry.buf; }) {
                event.buffer = entry.buf;
                event.data = entry.data;
            }
            if constexpr (requires { entry.tag; }) {
                event.tag = entry.tag;
            }
            events[i] = event;
        }
        return result;
    };
    switch (format) {
        case FI_CQ_FORMAT_CONTEXT:
            return read_as.template operator()<fi_cq_entry>();
        case FI_CQ_FORMAT_MSG:
            return read_as.template operator()<fi_cq_msg_entry>();
        case FI_CQ_FORMAT_DATA:
            return read_as.template operator()<fi_cq_data_entry>();
        case FI_CQ_FORMAT_TAGGED:
            return read_as.template operator()<fi_cq_tagged_entry>();
        default:
            return -FI_EINVAL;
    }
}

/// @brief Internal implementation of the completion queue.
struct completion_queue_impl {
    ::fid_cq* cq{nullptr};               ///< Underlying libfabric completion queue handle.
    runtime_progress_policy progress{};  ///< Progress policy for the completion queue.
    bool wait_enabled{false};            ///< Whether a native wait object was requested.
    fi_cq_format format{FI_CQ_FORMAT_TAGGED};  ///< Layout used by native reads.
    std::size_t capacity{};                    ///< Requested minimum queue capacity.

    /// @brief Default constructor.
    completion_queue_impl() = default;

    /// @brief Constructs completion_queue_impl with a libfabric CQ handle and progress policy.
    /// @param cq_ptr The libfabric completion queue pointer.
    /// @param prog The progress policy.
    explicit completion_queue_impl(::fid_cq* cq_ptr,
                                   runtime_progress_policy prog = runtime_progress_policy{},
                                   bool can_wait = false,
                                   fi_cq_format entry_format = FI_CQ_FORMAT_TAGGED,
                                   std::size_t minimum_capacity = 0)
        : cq(cq_ptr), progress(prog), wait_enabled(can_wait), format(entry_format),
          capacity(minimum_capacity) {}

    /// @brief Destructor that closes the libfabric completion queue.
    ~completion_queue_impl() {
        if (cq != nullptr) {
            ::fi_close(&cq->fid);
        }
    }

    completion_queue_impl(const completion_queue_impl&) = delete;
    auto operator=(const completion_queue_impl&) -> completion_queue_impl& = delete;
    completion_queue_impl(completion_queue_impl&&) = delete;
    auto operator=(completion_queue_impl&&) -> completion_queue_impl& = delete;
};

}  // namespace loom::detail

#ifdef LOOM_IMPLEMENTATION

namespace loom {

completion_queue::completion_queue() = default;

completion_queue::completion_queue(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

completion_queue::~completion_queue() = default;

completion_queue::completion_queue(completion_queue&&) noexcept = default;

auto completion_queue::operator=(completion_queue&&) noexcept -> completion_queue& = default;

auto completion_queue::impl_valid() const noexcept -> bool {
    return impl_ && impl_->cq != nullptr;
}

auto completion_queue::create(const domain& dom,
                              const completion_queue_attr& attr,
                              memory_resource* resource) -> result<completion_queue> {
    if (!dom) {
        return make_error_result<completion_queue>(errc::invalid_argument);
    }
    const auto format = detail::to_fi_cq_format(attr.format);
    if (format == FI_CQ_FORMAT_UNSPEC) {
        return make_error_result<completion_queue>(errc::invalid_argument);
    }

    ::fi_cq_attr cq_attr{};

    cq_attr.size = attr.size.get();
    cq_attr.flags = attr.flags;
    cq_attr.format = format;
    cq_attr.wait_obj = attr.wait_obj ? FI_WAIT_UNSPEC : FI_WAIT_NONE;

    ::fid_cq* cq = nullptr;
    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    int ret = ::fi_cq_open(domain_ptr, &cq_attr, &cq, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<completion_queue>(-ret);
    }

    detail::unique_fid<::fid_cq> pending{cq};

    auto progress = dom.progress_policy();

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::completion_queue_impl>(
            resource, cq, progress, attr.wait_obj, format, attr.size.get());
        (void)pending.release();
        return completion_queue{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::completion_queue_impl>(
        default_resource, cq, progress, attr.wait_obj, format, attr.size.get());
    (void)pending.release();
    return completion_queue{std::move(impl)};
}

auto completion_queue::poll() const -> std::optional<completion_event> {
    if (!impl_ || !impl_->cq) {
        return std::nullopt;
    }

    completion_event event{};
    const auto ret = detail::read_cq_entries<1>(
        impl_->format, std::span{&event, 1}, [this](void* entries, std::size_t count) {
            return ::fi_cq_read(impl_->cq, entries, count);
        });

    if (ret == -FI_EAGAIN) {
        return std::nullopt;
    }

    if (ret < 0) {
        const auto error = read_error(static_cast<int>(-ret));
        event = error.value_or(completion_event{});
        if (!error)
            event.error = error.error();
        return event;
    }

    if (ret == 0) {
        return std::nullopt;
    }

    return event;
}

auto completion_queue::wait(std::optional<std::chrono::milliseconds> timeout) const
    -> result<completion_event> {
    if (!impl_ || !impl_->cq || !impl_->wait_enabled) {
        return make_error_result<completion_event>(errc::invalid_argument);
    }

    int timeout_ms = -1;
    if (timeout && timeout->count() >= 0) {
        if (timeout->count() > std::numeric_limits<int>::max()) {
            return make_error_result<completion_event>(errc::invalid_argument);
        }
        timeout_ms = static_cast<int>(timeout->count());
    }

    completion_event event{};
    const auto ret = detail::read_cq_entries<1>(
        impl_->format, std::span{&event, 1}, [this, timeout_ms](void* entries, std::size_t count) {
            return ::fi_cq_sread(impl_->cq, entries, count, nullptr, timeout_ms);
        });

    if (ret == 0) {
        return make_error_result<completion_event>(errc::again);
    }

    if (ret == -FI_EAVAIL) {
        return read_error(FI_EAVAIL);
    }

    if (ret < 0) {
        return make_error_result_from_fi_errno<completion_event>(static_cast<int>(-ret));
    }

    return event;
}

auto completion_queue::poll_batch(std::span<completion_event> events) const -> std::size_t {
    if (!impl_ || !impl_->cq || events.empty()) {
        return 0;
    }

    const auto ret = detail::read_cq_entries<64>(
        impl_->format, events, [this](void* entries, std::size_t count) {
            return ::fi_cq_read(impl_->cq, entries, count);
        });

    if (ret == -FI_EAGAIN || ret == 0) {
        return 0;
    }
    if (ret < 0) {
        const auto error = read_error(static_cast<int>(-ret));
        events.front() = error.value_or(completion_event{});
        if (!error)
            events.front().error = error.error();
        return 1;
    }

    return static_cast<std::size_t>(ret);
}

auto completion_queue::read_error(int status) const -> result<completion_event> {
    if (status != FI_EAVAIL) {
        return make_error_result_from_fi_errno<completion_event>(status);
    }

    ::fi_cq_err_entry entry{};
    const auto ret = ::fi_cq_readerr(impl_->cq, &entry, 0);
    if (ret <= 0) {
        return make_error_result_from_fi_errno<completion_event>(ret == 0 ? FI_EAGAIN
                                                                          : static_cast<int>(-ret));
    }

    completion_event event{};
    event.context = context_ptr<void>{entry.op_context};
    event.buffer = entry.buf;
    event.error = make_error_code_from_fi_errno(entry.err);
    event.flags = entry.flags;
    event.bytes_transferred = entry.len;
    event.len = entry.len;
    event.tag = entry.tag;
    event.data = entry.data;
    event.error_info = {entry.prov_errno, entry.err_data, entry.err_data_size};
    return event;
}

auto completion_queue::read() const -> result<completion_event> {
    auto event = poll();
    if (!event) {
        return make_error_result<completion_event>(errc::again);
    }
    return *event;
}

auto completion_queue::capacity() const noexcept -> std::size_t {
    if (!impl_ || !impl_->cq) {
        return 0;
    }

    return impl_->capacity;
}

auto completion_queue::pending() const noexcept -> std::size_t {
    if (!impl_ || !impl_->cq) {
        return 0;
    }

    ::fi_cq_msg_entry entry{};
    ssize_t ret = ::fi_cq_read(impl_->cq, &entry, 0);

    if (ret < 0) {
        return 0;
    }

    return static_cast<std::size_t>(ret);
}

auto completion_queue::ack(const completion_event&) -> void_result {
    return make_success();
}

auto completion_queue::get_progress_policy() const noexcept -> runtime_progress_policy {
    if (!impl_) {
        return runtime_progress_policy{};
    }
    return impl_->progress;
}

auto completion_queue::supports_blocking_wait() const noexcept -> bool {
    if (!impl_ || !impl_->cq) {
        return false;
    }
    fi_wait_obj object = FI_WAIT_NONE;
    return ::fi_control(&impl_->cq->fid, FI_GETWAITOBJ, &object) == 0 && object != FI_WAIT_NONE &&
           object != FI_WAIT_UNSPEC;
}

auto completion_queue::requires_manual_progress() const noexcept -> bool {
    if (!impl_) {
        return true;
    }
    return impl_->progress.requires_manual_data_progress();
}

auto completion_queue::try_wait(const fabric& parent) const -> result<bool> {
    if (!impl_ || !impl_->cq || !parent) {
        return make_error_result<bool>(errc::invalid_argument);
    }
    auto* fabric_ptr = static_cast<::fid_fabric*>(parent.internal_ptr());
    auto* descriptor = &impl_->cq->fid;
    const int ret = ::fi_trywait(fabric_ptr, &descriptor, 1);
    if (ret == -FI_EAGAIN) {
        return false;
    }
    if (ret != 0) {
        return make_error_result_from_fi_errno<bool>(ret);
    }
    return true;
}

auto completion_queue::get_wait_fd() const -> result<int> {
    if (!impl_ || !impl_->cq) {
        return make_error_result<int>(errc::invalid_argument);
    }

    fi_wait_obj object = FI_WAIT_NONE;
    int ret = ::fi_control(&impl_->cq->fid, FI_GETWAITOBJ, &object);
    if (ret != 0) {
        return make_error_result_from_fi_errno<int>(-ret);
    }
    if (object != FI_WAIT_FD) {
        return make_error_result<int>(errc::not_supported);
    }

    int fd = -1;
    ret = ::fi_control(&impl_->cq->fid, FI_GETWAIT, &fd);
    if (ret != 0) {
        return make_error_result_from_fi_errno<int>(-ret);
    }

    if (fd < 0) {
        return make_error_result<int>(errc::not_supported);
    }

    return fd;
}

auto completion_queue::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->cq : nullptr;
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
