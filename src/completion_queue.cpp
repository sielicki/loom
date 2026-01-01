// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/async/completion_queue.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <system_error>

#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"

namespace loom {

namespace detail {

/// @brief Internal implementation of the completion queue.
struct completion_queue_impl {
    ::fid_cq* cq{nullptr};               ///< Underlying libfabric completion queue handle.
    runtime_progress_policy progress{};  ///< Progress policy for the completion queue.

    /// @brief Default constructor.
    completion_queue_impl() = default;

    /// @brief Constructs completion_queue_impl with a libfabric CQ handle and progress policy.
    /// @param cq_ptr The libfabric completion queue pointer.
    /// @param prog The progress policy.
    explicit completion_queue_impl(::fid_cq* cq_ptr,
                                   runtime_progress_policy prog = runtime_progress_policy{})
        : cq(cq_ptr), progress(prog) {}

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

}  // namespace detail

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

    ::fi_cq_attr cq_attr{};

    cq_attr.size = attr.size.get();
    cq_attr.flags = attr.flags;
    cq_attr.format = FI_CQ_FORMAT_DATA;
    cq_attr.wait_obj = attr.wait_obj ? FI_WAIT_UNSPEC : FI_WAIT_NONE;

    ::fid_cq* cq = nullptr;
    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    int ret = ::fi_cq_open(domain_ptr, &cq_attr, &cq, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<completion_queue>(-ret);
    }

    auto progress = dom.progress_policy();

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::completion_queue_impl>(resource, cq, progress);
        return completion_queue{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::completion_queue_impl>(default_resource, cq, progress);
    return completion_queue{std::move(impl)};
}

auto completion_queue::poll() -> std::optional<completion_event> {
    if (!impl_ || !impl_->cq) {
        return std::nullopt;
    }

    ::fi_cq_data_entry entry{};
    ssize_t ret = ::fi_cq_read(impl_->cq, &entry, 1);

    if (ret == -FI_EAGAIN) {
        return std::nullopt;
    }

    if (ret < 0) {
        ::fi_cq_err_entry err_entry{};
        ::fi_cq_readerr(impl_->cq, &err_entry, 0);

        completion_event event{};
        event.context = context_ptr<void>{err_entry.op_context};
        event.error = make_error_code_from_fi_errno(static_cast<int>(err_entry.err));
        event.flags = err_entry.flags;
        event.len = err_entry.len;
        event.error_info.prov_errno = err_entry.prov_errno;
        event.error_info.err_data = err_entry.err_data;
        event.error_info.err_data_size = err_entry.err_data_size;
        return event;
    }

    if (ret == 0) {
        return std::nullopt;
    }

    completion_event event{};
    event.context = context_ptr<void>{entry.op_context};
    event.error = std::error_code{};
    event.bytes_transferred = entry.len;
    event.flags = entry.flags;
    event.len = entry.len;
    event.data = entry.data;

    return event;
}

auto completion_queue::wait(std::optional<std::chrono::milliseconds> timeout)
    -> result<completion_event> {
    if (!impl_ || !impl_->cq) {
        return make_error_result<completion_event>(errc::invalid_argument);
    }

    ::fi_cq_data_entry entry{};

    int timeout_ms = -1;
    if (timeout.has_value()) {
        timeout_ms = static_cast<int>(timeout->count());
    }

    ssize_t ret = ::fi_cq_sread(impl_->cq, &entry, 1, nullptr, timeout_ms);

    if (ret == -FI_ETIMEDOUT) {
        return make_error_result<completion_event>(errc::timeout);
    }

    if (ret < 0) {
        ::fi_cq_err_entry err_entry{};
        ::fi_cq_readerr(impl_->cq, &err_entry, 0);

        completion_event event{};
        event.context = context_ptr<void>{err_entry.op_context};
        event.error = make_error_code_from_fi_errno(static_cast<int>(err_entry.err));
        event.flags = err_entry.flags;
        event.len = err_entry.len;
        event.error_info.prov_errno = err_entry.prov_errno;
        event.error_info.err_data = err_entry.err_data;
        event.error_info.err_data_size = err_entry.err_data_size;
        return event;
    }

    completion_event event{};
    event.context = context_ptr<void>{entry.op_context};
    event.error = std::error_code{};
    event.bytes_transferred = entry.len;
    event.flags = entry.flags;
    event.len = entry.len;
    event.data = entry.data;

    return event;
}

auto completion_queue::poll_batch(std::span<completion_event> events) -> std::size_t {
    if (!impl_ || !impl_->cq || events.empty()) {
        return 0;
    }

    constexpr std::size_t max_batch_size = 64;
    std::array<::fi_cq_data_entry, max_batch_size> fi_entries{};

    auto batch_size = std::min(events.size(), max_batch_size);
    ssize_t ret = ::fi_cq_read(impl_->cq, fi_entries.data(), batch_size);

    if (ret == -FI_EAGAIN || ret <= 0) {
        return 0;
    }

    auto count = static_cast<std::size_t>(ret);
    for (std::size_t i = 0; i < count; ++i) {
        events[i].context = context_ptr<void>{fi_entries[i].op_context};
        events[i].error = std::error_code{};
        events[i].bytes_transferred = fi_entries[i].len;
        events[i].flags = fi_entries[i].flags;
        events[i].len = fi_entries[i].len;
        events[i].data = fi_entries[i].data;
    }

    return count;
}

auto completion_queue::read() -> result<completion_event> {
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

    return 1024;
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
    if (!impl_) {
        return false;
    }
    return impl_->progress.supports_blocking_wait();
}

auto completion_queue::requires_manual_progress() const noexcept -> bool {
    if (!impl_) {
        return true;
    }
    return impl_->progress.requires_manual_data_progress();
}

auto completion_queue::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->cq : nullptr;
}

}  // namespace loom
