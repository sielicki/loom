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
    detail::event_queue_impl::buffer_lease storage{*impl_, impl_.get_deleter().resource};
    const auto buffer = storage.bytes();
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

    detail::event_queue_impl::buffer_lease storage{*impl_, impl_.get_deleter().resource};
    const auto buffer = storage.bytes();

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
