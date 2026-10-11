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
#include <list>
#include <mutex>
#include <ranges>
#include <system_error>

#include "loom/async/completion_queue.hpp"
#include "loom/core/allocator.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/impl/domain_impl.hpp"
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
auto read_cq_entries(fi_cq_format format,
                     std::span<completion_event> events,
                     bool with_sources,
                     Read&& read) -> ssize_t {
    const auto read_as = [&]<typename Entry>() -> ssize_t {
        std::array<Entry, Capacity> entries{};
        std::array<fi_addr_t, Capacity> sources;
        if (with_sources)
            sources.fill(FI_ADDR_NOTAVAIL);
        const auto result = read(entries.data(),
                                 std::min(events.size(), Capacity),
                                 with_sources ? sources.data() : nullptr);
        if (result <= 0)
            return result;
        for (std::size_t i = 0; i < static_cast<std::size_t>(result); ++i) {
            const auto& entry = entries[i];
            completion_event event{};
            event.context = context_ptr<void>{entry.op_context};
            if (with_sources)
                event.source = fabric_addr{sources[i]};
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
    bool source_addresses{false};              ///< Whether reads capture receive source addresses.
    std::size_t max_error_data{};              ///< Negotiated diagnostic buffer size.
    // Serialize native consumption with preserved events. Nonblocking readers
    // only try this lock, including while another thread waits natively.
    std::timed_mutex read_mutex;
    std::pmr::list<completion_event> preserved;
    std::pmr::list<completion_event> spare;
    std::mutex registrations_mutex;
    const completion_queue* wrapper{};
    completion_registration* registrations{};
    completion_queue_binding* bindings{};

    /// @brief Default constructor.
    completion_queue_impl() = default;

    /// @brief Constructs completion_queue_impl with a libfabric CQ handle and progress policy.
    /// @param cq_ptr The libfabric completion queue pointer.
    /// @param prog The progress policy.
    explicit completion_queue_impl(::fid_cq* cq_ptr,
                                   runtime_progress_policy prog = runtime_progress_policy{},
                                   bool can_wait = false,
                                   fi_cq_format entry_format = FI_CQ_FORMAT_TAGGED,
                                   std::size_t minimum_capacity = 0,
                                   bool with_sources = false,
                                   memory_resource* resource = get_default_resource())
        : cq(cq_ptr), progress(prog), wait_enabled(can_wait), format(entry_format),
          capacity(minimum_capacity), source_addresses(with_sources), preserved(resource),
          spare(resource) {}

    auto take_preserved() -> std::optional<completion_event> {
        if (preserved.empty())
            return std::nullopt;
        auto event = std::move(preserved.front());
        preserved.pop_front();
        return event;
    }

    /// @brief Destructor that closes the libfabric completion queue.
    ~completion_queue_impl() {
        // Queue lifetime must cover progress. Detach any remaining callbacks
        // so later registration destruction never refers to a freed queue.
        while (registrations) {
            registrations->reset();
        }
        while (bindings) {
            bindings->reset();
        }
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

detail::completion_queue_binding::~completion_queue_binding() {
    reset();
}

void detail::completion_queue_binding::bind(completion_queue_impl& queue) noexcept {
    reset();
    const std::lock_guard lock{queue.registrations_mutex};
    owner = &queue;
    next = queue.bindings;
    if (next) {
        next->previous = this;
    }
    queue.bindings = this;
}

void detail::completion_queue_binding::reset() noexcept {
    if (!owner) {
        return;
    }
    const std::lock_guard lock{owner->registrations_mutex};
    if (previous) {
        previous->next = next;
    } else {
        owner->bindings = next;
    }
    if (next) {
        next->previous = previous;
    }
    owner = nullptr;
    previous = nullptr;
    next = nullptr;
}

completion_registration::~completion_registration() {
    reset();
}

auto completion_registration::bind(completion_queue& queue,
                                   context_ptr<void> context,
                                   void* object,
                                   callback complete) noexcept -> void_result {
    if (!queue) {
        return make_error_result<void>(errc::invalid_argument);
    }
    return bind_to(*queue.impl_, context, object, complete);
}

auto completion_registration::bind_to(detail::completion_queue_impl& owner,
                                      context_ptr<void> context,
                                      void* object,
                                      callback complete) noexcept -> void_result {
    if (!owner.cq || !context || !complete) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (owner_) {
        return make_error_result<void>(errc::busy);
    }
    const std::lock_guard lock{owner.registrations_mutex};
    for (auto* entry = owner.registrations; entry; entry = entry->next_) {
        if (entry->context_ == context.get()) {
            return make_error_result<void>(errc::address_in_use);
        }
    }
    owner_ = &owner;
    context_ = context.get();
    object_ = object;
    complete_ = complete;
    next_ = owner.registrations;
    if (next_) {
        next_->previous_ = this;
    }
    owner.registrations = this;
    return make_success();
}

void completion_registration::unlink() noexcept {
    if (previous_) {
        previous_->next_ = next_;
    } else {
        owner_->registrations = next_;
    }
    if (next_) {
        next_->previous_ = previous_;
    }
    owner_ = nullptr;
    previous_ = nullptr;
    next_ = nullptr;
    context_ = nullptr;
    object_ = nullptr;
    complete_ = nullptr;
}

void completion_registration::reset() noexcept {
    if (owner_) {
        const std::lock_guard lock{owner_->registrations_mutex};
        unlink();
    }
}

auto completion_queue::dispatch(const completion_event& event) const -> bool {
    if (!impl_ || !event.context) {
        return false;
    }
    completion_registration::callback complete{};
    void* object{};
    {
        const std::lock_guard lock{impl_->registrations_mutex};
        for (auto* entry = impl_->registrations; entry; entry = entry->next_) {
            if (entry->context_ == event.context.get()) {
                complete = entry->complete_;
                object = entry->object_;
                entry->unlink();
                break;
            }
        }
    }
    if (!complete) {
        return false;
    }
    complete(object, event);
    return true;
}

completion_queue::completion_queue() = default;

completion_queue::completion_queue(impl_ptr impl) noexcept : impl_(std::move(impl)) {
    if (impl_)
        impl_->wrapper = this;
}

completion_queue::~completion_queue() = default;

completion_queue::completion_queue(completion_queue&& other) noexcept
    : completion_queue(std::move(other.impl_)) {}

auto completion_queue::operator=(completion_queue&& other) noexcept -> completion_queue& {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        if (impl_)
            impl_->wrapper = this;
    }
    return *this;
}

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
        auto impl = detail::make_pmr_unique<detail::completion_queue_impl>(resource,
                                                                           cq,
                                                                           progress,
                                                                           attr.wait_obj,
                                                                           format,
                                                                           attr.size.get(),
                                                                           attr.source_addresses,
                                                                           resource);
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
                                                                       attr.source_addresses,
                                                                       default_resource);
    impl->max_error_data = dom.impl_->max_error_data;
    (void)pending.release();
    return completion_queue{std::move(impl)};
}

auto completion_queue::poll() const -> std::optional<completion_event> {
    if (!impl_ || !impl_->cq)
        return std::nullopt;
    const std::unique_lock lock{impl_->read_mutex, std::try_to_lock};
    if (!lock)
        return std::nullopt;
    if (auto event = impl_->take_preserved())
        return event;
    auto result = read_native();
    if (result)
        return std::move(*result);
    if (result.error() == errc::again)
        return std::nullopt;
    completion_event event{};
    event.error = result.error();
    return event;
}

auto completion_queue::read_native() const -> result<completion_event> {
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
    if (ret == 0 || ret == -FI_EAGAIN)
        return make_error_result<completion_event>(errc::again);
    if (ret < 0)
        return read_error(static_cast<int>(-ret));
    return event;
}

auto completion_queue::read_for(context_ptr<void> context) const -> result<completion_event> {
    if (!impl_ || !impl_->cq || !context)
        return make_error_result<completion_event>(errc::invalid_argument);
    const std::unique_lock lock{impl_->read_mutex, std::try_to_lock};
    if (!lock)
        return make_error_result<completion_event>(errc::again);
    const auto found =
        std::ranges::find(impl_->preserved, context.get(), [](const completion_event& event) {
            return event.context.get();
        });
    if (found != impl_->preserved.end()) {
        auto event = std::move(*found);
        impl_->preserved.erase(found);
        return event;
    }
    // Reserve a node before consumption. Retain the spare across empty reads
    // so waiting for one operation does not allocate on every poll.
    if (impl_->spare.empty())
        impl_->spare.emplace_back();
    auto event = read_native();
    if (!event || event->context.get() == context.get())
        return event;
    impl_->spare.front() = std::move(*event);
    impl_->preserved.splice(impl_->preserved.end(), impl_->spare);
    return make_error_result<completion_event>(errc::again);
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

    std::unique_lock lock{impl_->read_mutex, std::defer_lock};
    if (timeout_ms < 0) {
        lock.lock();
    } else {
        const auto deadline = std::chrono::steady_clock::now() + *timeout;
        if (!lock.try_lock_until(deadline))
            return make_error_result<completion_event>(errc::again);
        const auto remaining = std::max(std::chrono::milliseconds{0},
                                        std::chrono::ceil<std::chrono::milliseconds>(
                                            deadline - std::chrono::steady_clock::now()));
        timeout_ms = static_cast<int>(remaining.count());
    }
    if (auto event = impl_->take_preserved())
        return std::move(*event);

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

    const std::unique_lock lock{impl_->read_mutex, std::try_to_lock};
    if (!lock)
        return 0;
    if (!impl_->preserved.empty()) {
        std::size_t count{};
        while (count < std::min(events.size(), 64UZ)) {
            auto event = impl_->take_preserved();
            if (!event)
                break;
            events[count++] = std::move(*event);
        }
        return count;
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

auto completion_queue::format() const noexcept -> cq_format {
    if (!impl_ || !impl_->cq)
        return cq_format::unspecified;
    switch (impl_->format) {
        case FI_CQ_FORMAT_CONTEXT:
            return cq_format::context;
        case FI_CQ_FORMAT_MSG:
            return cq_format::msg;
        case FI_CQ_FORMAT_DATA:
            return cq_format::data;
        case FI_CQ_FORMAT_TAGGED:
            return cq_format::tagged;
        default:
            return cq_format::unspecified;
    }
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

    const std::unique_lock lock{impl_->read_mutex, std::try_to_lock};
    if (!lock)
        return make_error_result<void>(errc::again);
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
    const std::unique_lock lock{impl_->read_mutex, std::try_to_lock};
    if (!lock || !impl_->preserved.empty())
        return false;
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
