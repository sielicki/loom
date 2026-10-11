// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/event_queue.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>

#include <cstring>
#include <vector>

#include "loom/core/allocator.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/impl/event_queue_impl.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom {



/// @brief Converts libfabric event type to loom event type.
static auto fi_event_to_loom(std::uint32_t fi_event) -> event_type {
    switch (fi_event) {
        case FI_CONNECTED:
            return event_type::connected;
        case FI_CONNREQ:
            return event_type::connreq;
        case FI_SHUTDOWN:
            return event_type::shutdown;
        case FI_JOIN_COMPLETE:
            return event_type::join_complete;
        case FI_MR_COMPLETE:
            return event_type::mr_complete;
        case FI_AV_COMPLETE:
            return event_type::av_complete;
        default:
            return event_type::connected;
    }
}

event_queue::event_queue() = default;

event_queue::event_queue(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

event_queue::~event_queue() = default;

event_queue::event_queue(event_queue&&) noexcept = default;

auto event_queue::operator=(event_queue&&) noexcept -> event_queue& = default;

auto event_queue::impl_valid() const noexcept -> bool {
    return impl_ && impl_->eq != nullptr;
}

auto event_queue::create(const fabric& fab, const event_queue_attr& attr, memory_resource* resource)
    -> result<event_queue> {
    if (!fab) {
        return make_error_result<event_queue>(errc::invalid_argument);
    }

    ::fi_eq_attr eq_attr{};

    eq_attr.size = attr.size.get();
    eq_attr.flags = attr.flags;
    eq_attr.wait_obj = attr.wait_obj ? FI_WAIT_UNSPEC : FI_WAIT_NONE;
    eq_attr.signaling_vector = static_cast<int>(attr.signaling_vector);

    ::fid_eq* eq = nullptr;
    auto* fabric_ptr = static_cast<::fid_fabric*>(fab.internal_ptr());

    int ret = ::fi_eq_open(fabric_ptr, &eq_attr, &eq, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<event_queue>(-ret);
    }

    detail::unique_fid<::fid_eq> pending{eq};

    if (resource) {
        auto impl = detail::make_pmr_unique<detail::event_queue_impl>(resource, eq, attr.wait_obj);
        (void)pending.release();
        return event_queue{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl =
        detail::make_pmr_unique<detail::event_queue_impl>(default_resource, eq, attr.wait_obj);
    (void)pending.release();
    return event_queue{std::move(impl)};
}

auto event_queue::poll() -> std::optional<fabric_event> {
    if (!impl_ || !impl_->eq) {
        return std::nullopt;
    }

    std::uint32_t event_type = 0;
    ::fi_eq_cm_entry entry{};
    std::uint64_t flags = 0;

    ssize_t ret = ::fi_eq_read(impl_->eq, &event_type, &entry, sizeof(entry), flags);

    if (ret == 0 || ret == -FI_EAGAIN) {
        return std::nullopt;
    }

    if (ret < 0 && ret != -FI_EAVAIL) {
        fabric_event event{};
        event.error = make_error_code_from_fi_errno(static_cast<int>(-ret));
        return event;
    }

    if (ret == -FI_EAVAIL) {
        ::fi_eq_err_entry err_entry{};
        const auto count = ::fi_eq_readerr(impl_->eq, &err_entry, flags);
        if (count <= 0) {
            fabric_event event{};
            event.error =
                make_error_code_from_fi_errno(count == 0 ? FI_EAGAIN : static_cast<int>(-count));
            return event;
        }

        fabric_event event{};
        event.type = fi_event_to_loom(event_type);
        event.fid = err_entry.fid;
        event.context = err_entry.context;
        event.error = make_error_code_from_fi_errno(static_cast<int>(err_entry.err));
        event.data = err_entry.data;
        return event;
    }

    fabric_event event{};
    event.type = fi_event_to_loom(event_type);
    event.fid = entry.fid;
    event.context = nullptr;
    event.error = std::error_code{};

    return event;
}

auto event_queue::wait(std::optional<std::chrono::milliseconds> timeout) -> result<fabric_event> {
    if (!impl_ || !impl_->eq || !impl_->wait_enabled) {
        return make_error_result<fabric_event>(errc::invalid_argument);
    }

    std::uint32_t event_type = 0;
    ::fi_eq_cm_entry entry{};
    std::uint64_t flags = 0;

    int timeout_ms = -1;
    if (timeout && timeout->count() >= 0) {
        if (timeout->count() > std::numeric_limits<int>::max()) {
            return make_error_result<fabric_event>(errc::invalid_argument);
        }
        timeout_ms = static_cast<int>(timeout->count());
    }

    ssize_t ret = ::fi_eq_sread(impl_->eq, &event_type, &entry, sizeof(entry), timeout_ms, flags);

    if (ret == 0) {
        return make_error_result<fabric_event>(errc::again);
    }

    if (ret < 0 && ret != -FI_EAVAIL) {
        return make_error_result_from_fi_errno<fabric_event>(static_cast<int>(-ret));
    }

    if (ret == -FI_EAVAIL) {
        ::fi_eq_err_entry err_entry{};
        const auto count = ::fi_eq_readerr(impl_->eq, &err_entry, flags);
        if (count <= 0) {
            return make_error_result_from_fi_errno<fabric_event>(
                count == 0 ? FI_EAGAIN : static_cast<int>(-count));
        }

        fabric_event event{};
        event.type = fi_event_to_loom(event_type);
        event.fid = err_entry.fid;
        event.context = err_entry.context;
        event.error = make_error_code_from_fi_errno(static_cast<int>(err_entry.err));
        event.data = err_entry.data;
        return event;
    }

    fabric_event event{};
    event.type = fi_event_to_loom(event_type);
    event.fid = entry.fid;
    event.context = nullptr;
    event.error = std::error_code{};

    return event;
}

auto event_queue::read() -> result<fabric_event> {
    auto event = poll();
    if (!event) {
        return make_error_result<fabric_event>(errc::again);
    }
    return *event;
}

auto event_queue::write_error(const fabric_event&) -> void_result {
    return make_error_result<void>(errc::not_supported);
}

auto event_queue::event_to_string(const fabric_event& event) const -> std::string {
    if (!impl_ || !impl_->eq) {
        return "Invalid event queue";
    }

    const char* str = ::fi_eq_strerror(impl_->eq,
                                       static_cast<int>(event.type),
                                       event.error.value() != 0 ? nullptr : nullptr,
                                       nullptr,
                                       0);
    return str ? std::string(str) : "Unknown event";
}

auto event_queue::capacity() const noexcept -> std::size_t {
    if (!impl_ || !impl_->eq) {
        return 0;
    }
    return 128;
}

auto event_queue::ack(const fabric_event&) -> void_result {
    return make_success();
}

auto event_queue::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->eq : nullptr;
}

}  // namespace loom
