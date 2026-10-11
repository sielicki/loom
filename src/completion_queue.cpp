// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/async/completion_queue.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ranges>
#include <system_error>

#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/impl/completion_queue_impl.hpp"
#include "loom/detail/unique_fid.hpp"

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
            resource, cq, progress, attr.wait_obj, format, attr.size.get(), attr.source_addresses);
        impl->max_error_data = dom.impl_->max_error_data;
        (void)pending.release();
        return completion_queue{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::completion_queue_impl>(default_resource,
                                                                       cq,
                                                                       progress,
                                                                       attr.wait_obj,
                                                                       format,
                                                                       attr.size.get(),
                                                                       attr.source_addresses);
    impl->max_error_data = dom.impl_->max_error_data;
    (void)pending.release();
    return completion_queue{std::move(impl)};
}

auto completion_queue::poll() const -> std::optional<completion_event> {
    if (!impl_ || !impl_->cq) {
        return std::nullopt;
    }

    completion_event event{};
    const auto ret = detail::read_cq_entries<1>(
        impl_->format,
        std::span{&event, 1},
        impl_->source_addresses,
        [this](void* entries, std::size_t count, fi_addr_t* sources) -> ssize_t {
            if (sources) {
                if (!FI_CHECK_OP(impl_->cq->ops, fi_ops_cq, readfrom))
                    return -FI_ENOSYS;
                return ::fi_cq_readfrom(impl_->cq, entries, count, sources);
            }
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
        impl_->format,
        std::span{&event, 1},
        impl_->source_addresses,
        [this, timeout_ms](void* entries, std::size_t count, fi_addr_t* sources) -> ssize_t {
            if (sources) {
                if (!FI_CHECK_OP(impl_->cq->ops, fi_ops_cq, sreadfrom))
                    return -FI_ENOSYS;
                return ::fi_cq_sreadfrom(impl_->cq, entries, count, sources, nullptr, timeout_ms);
            }
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
        impl_->format,
        events,
        impl_->source_addresses,
        [this](void* entries, std::size_t count, fi_addr_t* sources) -> ssize_t {
            if (sources) {
                if (!FI_CHECK_OP(impl_->cq->ops, fi_ops_cq, readfrom))
                    return -FI_ENOSYS;
                return ::fi_cq_readfrom(impl_->cq, entries, count, sources);
            }
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

    // A nonzero input size requests caller-owned data, including when the
    // provider advertises no diagnostics. Allocate before consuming the error.
    const auto capacity = std::max(impl_->max_error_data, std::size_t{1});
    const auto words =
        capacity / sizeof(std::max_align_t) + (capacity % sizeof(std::max_align_t) != 0);
    auto* resource = impl_.get_deleter().resource;
    auto storage = std::allocate_shared<std::max_align_t[]>(
        pmr_allocator<std::max_align_t>{resource ? resource : get_default_resource()}, words);
    ::fi_cq_err_entry entry{};
    entry.err_data = storage.get();
    entry.err_data_size = capacity;
    const auto ret = ::fi_cq_readerr(impl_->cq, &entry, 0);
    if (ret <= 0) {
        return make_error_result_from_fi_errno<completion_event>(ret == 0 ? FI_EAGAIN
                                                                          : static_cast<int>(-ret));
    }

    if (entry.err_data_size > capacity ||
        (entry.err_data_size && entry.err_data != storage.get())) {
        return make_error_result_from_fi_errno<completion_event>(FI_EIO);
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
    event.error_info.prov_errno = entry.prov_errno;
    if (entry.err_data_size) {
        const auto* data = reinterpret_cast<const std::byte*>(storage.get());
        event.error_info.err_data = {std::move(storage), data};
        event.error_info.err_data_size = entry.err_data_size;
    }
    if (impl_->source_addresses)
        event.source = fabric_addr{entry.src_addr};
    return event;
}

auto completion_queue::read() const -> result<completion_event> {
    auto event = poll();
    if (!event) {
        return make_error_result<completion_event>(errc::again);
    }
    return std::move(*event);
}

auto completion_queue::capacity() const noexcept -> std::size_t {
    if (!impl_ || !impl_->cq) {
        return 0;
    }

    return impl_->capacity;
}

auto completion_queue::progress() const noexcept -> void_result {
    if (!impl_ || !impl_->cq) {
        return make_error_result<void>(errc::invalid_argument);
    }

    const auto status = ::fi_cq_read(impl_->cq, nullptr, 0);
    if (status < 0 && status != -FI_EAGAIN) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    }
    return make_success();
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
