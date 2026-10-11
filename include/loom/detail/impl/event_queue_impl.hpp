// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "loom/core/allocator.hpp"
#include "loom/core/error.hpp"
#include "loom/core/event_queue.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/impl/fabric_impl.hpp"
#include "loom/detail/unique_fid.hpp"

namespace loom::detail {

/// @brief Converts libfabric event type to loom event type.
inline auto fi_event_to_loom(std::uint32_t fi_event) -> event_type {
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
            return event_type::unknown;
    }
}

// Control completions and connection notifications have distinct native layouts.
inline auto decode_eq_event(std::uint32_t type,
                            std::span<const std::byte> buffer,
                            std::size_t count,
                            memory_resource* resource) -> result<fabric_event> {
    if (count > buffer.size()) {
        return make_error_result_from_fi_errno<fabric_event>(FI_EIO);
    }

    auto* allocator = resource ? resource : get_default_resource();
    fabric_event event{
        {}, event_type::unknown, nullptr, nullptr, {}, 0, std::pmr::vector<std::byte>{allocator}};
    event.type = fi_event_to_loom(type);
    switch (event.type) {
        case event_type::mr_complete:
        case event_type::av_complete:
        case event_type::join_complete: {
            if (count < sizeof(::fi_eq_entry)) {
                return make_error_result_from_fi_errno<fabric_event>(FI_EIO);
            }
            ::fi_eq_entry entry{};
            std::memcpy(&entry, buffer.data(), sizeof(entry));
            event.fid = entry.fid;
            event.context = entry.context;
            event.data = entry.data;
            break;
        }
        case event_type::connected:
        case event_type::connreq:
        case event_type::shutdown: {
            if (count < sizeof(::fi_eq_cm_entry)) {
                return make_error_result_from_fi_errno<fabric_event>(FI_EIO);
            }
            ::fi_eq_cm_entry entry{};
            std::memcpy(&entry, buffer.data(), sizeof(entry));
            event.fid = entry.fid;
            if (event.type == event_type::connreq && entry.info) {
                std::unique_ptr<::fi_info, decltype(&::fi_freeinfo)> pending{entry.info,
                                                                             ::fi_freeinfo};
                auto impl = make_pmr_unique<fabric_info_impl>(allocator, entry.info);
                (void)pending.release();
                fabric_info info{std::move(impl)};
                event.connection_info = std::allocate_shared<fabric_info>(
                    pmr_allocator<fabric_info>{allocator}, std::move(info));
            }
            const auto payload = buffer.subspan(sizeof(entry), count - sizeof(entry));
            event.info.assign(payload.begin(), payload.end());
            event.info_len = event.info.size();
            break;
        }
        case event_type::unknown:
            break;
    }
    return event;
}

inline auto decode_eq_error(const fi_eq_err_entry& entry,
                            std::size_t capacity,
                            memory_resource* resource) -> result<fabric_event> {
    if (entry.err_data_size > capacity || (entry.err_data_size && !entry.err_data)) {
        return make_error_result_from_fi_errno<fabric_event>(FI_EIO);
    }
    auto* allocator = resource ? resource : get_default_resource();
    fabric_event event{{},
                       event_type::unknown,
                       entry.fid,
                       entry.context,
                       make_error_code_from_fi_errno(entry.err),
                       entry.data,
                       std::pmr::vector<std::byte>{allocator}};
    event.prov_errno = entry.prov_errno;
    if (entry.err_data_size) {
        const auto bytes =
            std::span{static_cast<const std::byte*>(entry.err_data), entry.err_data_size};
        event.info.assign(bytes.begin(), bytes.end());
    }
    event.info_len = event.info.size();
    return event;
}

/// @brief Internal implementation of the event queue.
struct event_queue_impl {
    ::fid_eq* eq{nullptr};                ///< Underlying libfabric event queue handle.
    std::size_t event_buffer_size{1024};  ///< Per-read native buffer size, including the header.
    bool wait_enabled{false};             ///< Whether a native wait object was requested.
    std::size_t capacity{};               ///< Requested minimum queue capacity.

    event_queue_impl() = default;

    /// @brief Constructs event_queue_impl with a libfabric event queue handle.
    /// @param eq_ptr The libfabric event queue pointer.
    explicit event_queue_impl(::fid_eq* eq_ptr,
                              bool can_wait = false,
                              std::size_t minimum_capacity = 0,
                              std::size_t buffer_size = 1024)
        : eq(eq_ptr), event_buffer_size(buffer_size), wait_enabled(can_wait),
          capacity(minimum_capacity) {}

    /// @brief Destructor that closes the libfabric event queue.
    ~event_queue_impl() {
        if (eq != nullptr) {
            ::fi_close(&eq->fid);
        }
    }

    event_queue_impl(const event_queue_impl&) = delete;
    auto operator=(const event_queue_impl&) -> event_queue_impl& = delete;
    event_queue_impl(event_queue_impl&&) = delete;
    auto operator=(event_queue_impl&&) -> event_queue_impl& = delete;
};

}  // namespace loom::detail

#ifdef LOOM_IMPLEMENTATION

namespace loom {

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
    if (!fab ||
        attr.event_buffer_size < std::max(sizeof(::fi_eq_entry), sizeof(::fi_eq_cm_entry)) ||
        attr.event_buffer_size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
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
        auto impl = detail::make_pmr_unique<detail::event_queue_impl>(
            resource, eq, attr.wait_obj, attr.size.get(), attr.event_buffer_size);
        (void)pending.release();
        return event_queue{std::move(impl)};
    }

    auto* default_resource = std::pmr::get_default_resource();
    auto impl = detail::make_pmr_unique<detail::event_queue_impl>(
        default_resource, eq, attr.wait_obj, attr.size.get(), attr.event_buffer_size);
    (void)pending.release();
    return event_queue{std::move(impl)};
}

auto event_queue::poll() -> std::optional<fabric_event> {
    if (!impl_ || !impl_->eq) {
        return std::nullopt;
    }

    std::uint32_t event_type = 0;
    const auto units = impl_->event_buffer_size / sizeof(std::max_align_t) +
                       (impl_->event_buffer_size % sizeof(std::max_align_t) != 0);
    auto* allocator = impl_.get_deleter().resource;
    std::pmr::vector<std::max_align_t> storage(units,
                                               allocator ? allocator : get_default_resource());
    const auto buffer = std::as_writable_bytes(std::span{storage}).first(impl_->event_buffer_size);
    std::uint64_t flags = 0;

    ssize_t ret = ::fi_eq_read(impl_->eq, &event_type, buffer.data(), buffer.size(), flags);

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
        err_entry.err_data = buffer.data();
        err_entry.err_data_size = buffer.size();
        const auto count = ::fi_eq_readerr(impl_->eq, &err_entry, flags);
        if (count <= 0) {
            fabric_event event{};
            event.error =
                make_error_code_from_fi_errno(count == 0 ? FI_EAGAIN : static_cast<int>(-count));
            return event;
        }

        auto decoded =
            detail::decode_eq_error(err_entry, buffer.size(), impl_.get_deleter().resource);
        if (!decoded) {
            fabric_event event{};
            event.error = decoded.error();
            return event;
        }
        return std::move(*decoded);
    }

    auto decoded = detail::decode_eq_event(
        event_type, buffer, static_cast<std::size_t>(ret), impl_.get_deleter().resource);
    if (!decoded) {
        fabric_event event{};
        event.error = decoded.error();
        return event;
    }
    return std::move(*decoded);
}

auto event_queue::wait(std::optional<std::chrono::milliseconds> timeout) -> result<fabric_event> {
    if (!impl_ || !impl_->eq || !impl_->wait_enabled) {
        return make_error_result<fabric_event>(errc::invalid_argument);
    }

    std::uint32_t event_type = 0;
    std::uint64_t flags = 0;

    int timeout_ms = -1;
    if (timeout && timeout->count() >= 0) {
        if (timeout->count() > std::numeric_limits<int>::max()) {
            return make_error_result<fabric_event>(errc::invalid_argument);
        }
        timeout_ms = static_cast<int>(timeout->count());
    }

    const auto units = impl_->event_buffer_size / sizeof(std::max_align_t) +
                       (impl_->event_buffer_size % sizeof(std::max_align_t) != 0);
    auto* allocator = impl_.get_deleter().resource;
    std::pmr::vector<std::max_align_t> storage(units,
                                               allocator ? allocator : get_default_resource());
    const auto buffer = std::as_writable_bytes(std::span{storage}).first(impl_->event_buffer_size);

    ssize_t ret =
        ::fi_eq_sread(impl_->eq, &event_type, buffer.data(), buffer.size(), timeout_ms, flags);

    if (ret == 0) {
        return make_error_result<fabric_event>(errc::again);
    }

    if (ret < 0 && ret != -FI_EAVAIL) {
        return make_error_result_from_fi_errno<fabric_event>(static_cast<int>(-ret));
    }

    if (ret == -FI_EAVAIL) {
        ::fi_eq_err_entry err_entry{};
        err_entry.err_data = buffer.data();
        err_entry.err_data_size = buffer.size();
        const auto count = ::fi_eq_readerr(impl_->eq, &err_entry, flags);
        if (count <= 0) {
            return make_error_result_from_fi_errno<fabric_event>(
                count == 0 ? FI_EAGAIN : static_cast<int>(-count));
        }

        return detail::decode_eq_error(err_entry, buffer.size(), impl_.get_deleter().resource);
    }

    return detail::decode_eq_event(
        event_type, buffer, static_cast<std::size_t>(ret), impl_.get_deleter().resource);
}

auto event_queue::read() -> result<fabric_event> {
    auto event = poll();
    if (!event) {
        return make_error_result<fabric_event>(errc::again);
    }
    return std::move(*event);
}

auto event_queue::write_error(const fabric_event&) -> void_result {
    return make_error_result<void>(errc::not_supported);
}

auto event_queue::event_to_string(const fabric_event& event) const -> std::string {
    if (!impl_ || !impl_->eq) {
        return "Invalid event queue";
    }

    if (event.error) {
        if (event.prov_errno != 0) {
            const auto* text = ::fi_eq_strerror(impl_->eq,
                                                event.prov_errno,
                                                event.info.empty() ? nullptr : event.info.data(),
                                                nullptr,
                                                0);
            if (text)
                return text;
        }
        return event.error.message();
    }
    switch (event.type) {
        case event_type::connected:
            return "Connected";
        case event_type::connreq:
            return "Connection request";
        case event_type::shutdown:
            return "Shutdown";
        case event_type::join_complete:
            return "Join complete";
        case event_type::mr_complete:
            return "Memory registration complete";
        case event_type::av_complete:
            return "Address resolution complete";
        case event_type::unknown:
            return "Unknown event";
    }
    return "Unknown event";
}

auto event_queue::capacity() const noexcept -> std::size_t {
    if (!impl_ || !impl_->eq) {
        return 0;
    }
    return impl_->capacity;
}

auto event_queue::ack(const fabric_event&) -> void_result {
    return make_success();
}

auto event_queue::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->eq : nullptr;
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
