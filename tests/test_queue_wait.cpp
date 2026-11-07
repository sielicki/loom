// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/endpoint.hpp>
#include <loom/core/event_queue.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <semaphore>
#include <thread>
#include <vector>

#include "joining_thread.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct queue_provider {
    int calls{};
    int timeout{};
    int status{1};
    int error_status{1};
    int error_reads{};
    std::uint32_t native_event{FI_CONNECTED};
    fi_wait_obj requested_wait{};
    std::size_t requested_capacity{};
    bool short_entry{};
    fi_info* accepted_info{};
    int freed_infos{};
    std::vector<std::byte> private_data;
    int provider_error{};
    int strerror_calls{};
    std::vector<std::byte> error_data;
    bool oversized_error{};
    bool missing_error_data{};
    void* read_buffer{};
    std::size_t read_capacity{};

    auto read_event(fid_eq* eq, std::uint32_t* type, void* buffer, std::size_t length) -> ssize_t {
        ++calls;
        read_buffer = buffer;
        read_capacity = length;
        *type = native_event;
        if (status != 1)
            return status;
        if (native_event == FI_MR_COMPLETE || native_event == FI_AV_COMPLETE ||
            native_event == FI_JOIN_COMPLETE) {
            const fi_eq_entry entry{.fid = &eq->fid, .context = &calls, .data = 73};
            if (length < sizeof(entry))
                return -FI_ETOOSMALL;
            const auto count = short_entry ? sizeof(entry) - 1 : sizeof(entry);
            std::memcpy(buffer, &entry, count);
            return static_cast<ssize_t>(count);
        }
        if (length < sizeof(fi_eq_cm_entry))
            return -FI_ETOOSMALL;
        const fi_eq_cm_entry entry{
            .fid = &eq->fid,
            .info = native_event == FI_CONNREQ ? connection_request.release() : nullptr,
        };
        const auto count = short_entry ? sizeof(entry) - 1 : sizeof(entry);
        std::memcpy(buffer, &entry, count);
        const auto payload = short_entry ? 0 : std::min(private_data.size(), length - count);
        if (payload)
            std::memcpy(static_cast<std::byte*>(buffer) + count, private_data.data(), payload);
        return static_cast<ssize_t>(count + payload);
    }
    fi_ops info_ops{.size = sizeof(fi_ops), .close = [](fid* handle) {
                        ++static_cast<queue_provider*>(handle->context)->freed_infos;
                        return 0;
                    }};
    fid_nic native_nic{.fid = {.context = this, .ops = &info_ops}};
    std::unique_ptr<fi_info, decltype(&fi_freeinfo)> connection_request{nullptr, fi_freeinfo};
    fi_ops lifetime_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .readerr = [](fid_cq* cq, fi_cq_err_entry* error, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(cq->fid.context);
            ++self.error_reads;
            if (self.error_status != 1)
                return self.error_status;
            *error = {.len = 5, .err = FI_ECANCELED};
            return 1;
        },
        .sread = [](fid_cq* cq, void* buffer, std::size_t, const void*, int timeout) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(cq->fid.context);
            ++self.calls;
            self.timeout = timeout;
            if (self.status != 1)
                return self.status;
            *static_cast<fi_cq_tagged_entry*>(buffer) = {.len = 17};
            return 1;
        },
    };
    fi_ops_eq eq_ops{
        .size = sizeof(fi_ops_eq),
        .read = [](fid_eq* eq, std::uint32_t* type, void* buffer, std::size_t length, std::uint64_t)
            -> ssize_t {
            return static_cast<queue_provider*>(eq->fid.context)
                ->read_event(eq, type, buffer, length);
        },
        .readerr = [](fid_eq* eq, fi_eq_err_entry* error, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(eq->fid.context);
            ++self.error_reads;
            if (self.error_status != 1)
                return self.error_status;
            auto* output = error->err_data;
            const auto capacity = error->err_data_size;
            *error = {.fid = &eq->fid,
                      .context = &self.calls,
                      .data = 42,
                      .err = FI_ECANCELED,
                      .prov_errno = self.provider_error};
            if (output && capacity) {
                error->err_data_size = std::min(capacity, self.error_data.size());
                if (error->err_data_size)
                    std::memcpy(output, self.error_data.data(), error->err_data_size);
                error->err_data = output;
            } else {
                error->err_data = self.error_data.data();
                error->err_data_size = self.error_data.size();
            }
            if (self.oversized_error)
                error->err_data_size = capacity + 1;
            if (self.missing_error_data)
                error->err_data = nullptr;
            return sizeof(fi_eq_err_entry);
        },
        .sread = [](fid_eq* eq,
                    std::uint32_t* type,
                    void* buffer,
                    std::size_t length,
                    int timeout,
                    std::uint64_t) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(eq->fid.context);
            self.timeout = timeout;
            return self.read_event(eq, type, buffer, length);
        },
        .strerror = [](fid_eq* eq, int error, const void* data, char*, std::size_t) -> const char* {
            auto& self = *static_cast<queue_provider*>(eq->fid.context);
            ++self.strerror_calls;
            constexpr std::array expected{
                std::byte{0x52}, std::byte{0}, std::byte{0x79}, std::byte{0}};
            return error == 37 && data && std::memcmp(data, expected.data(), expected.size()) == 0
                       ? "provider diagnostic"
                       : "wrong provider diagnostic";
        },
    };
    fid_cq native_cq{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &cq_ops};
    fid_eq native_eq{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &eq_ops};
    fid_ep native_endpoint{.fid = {.context = this, .ops = &lifetime_ops}};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cq_open =
            [](fid_domain* dom, fi_cq_attr* attr, fid_cq** result, void*) {
                auto& self = *static_cast<queue_provider*>(dom->fid.context);
                self.requested_wait = attr->wait_obj;
                self.requested_capacity = attr->size;
                *result = &self.native_cq;
                return 0;
            },
        .endpoint =
            [](fid_domain* dom, fi_info* info, fid_ep** result, void*) {
                auto& self = *static_cast<queue_provider*>(dom->fid.context);
                self.accepted_info = info;
                *result = &self.native_endpoint;
                return 0;
            },
    };
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .eq_open =
            [](fid_fabric* fab, fi_eq_attr* attr, fid_eq** result, void*) {
                auto& self = *static_cast<queue_provider*>(fab->fid.context);
                self.requested_wait = attr->wait_obj;
                self.requested_capacity = attr->size;
                *result = &self.native_eq;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &domain_ops};
    fid_fabric native_fabric{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &fabric_ops};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native_domain)};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &native_fabric, nullptr)};

    template <typename Action>
    void with_queue(bool completion, bool waitable, Action action) {
        if (completion) {
            auto created = loom::completion_queue::create(domain, {.wait_obj = waitable});
            REQUIRE(created);
            action(*created);
        } else {
            auto created = loom::event_queue::create(fabric, {.wait_obj = waitable});
            REQUIRE(created);
            action(*created);
        }
    }
};
struct limited_resource : loom::memory_resource {
    int remaining{-1};
    int live{};
    int allocations{};

    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        if (remaining == 0)
            throw std::bad_alloc{};
        if (remaining > 0)
            --remaining;
        auto* ptr = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live;
        ++allocations;
        return ptr;
    }
    void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }
    auto do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};
}  // namespace

TEST_CASE("queue waits require a configured wait object", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    INFO("completion queue: " << completion);
    queue_provider provider;
    provider.with_queue(completion, false, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == loom::errc::invalid_argument);
    });
    REQUIRE(provider.requested_wait == FI_WAIT_NONE);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("queues report their configured minimum capacity after moves", "[queue][capacity]") {
    const auto completion = GENERATE(false, true);
    const auto capacity =
        GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{17}, std::size_t{4096});
    queue_provider provider;
    const auto check = [&](auto& queue) {
        REQUIRE(provider.requested_capacity == capacity);
        CHECK(queue.capacity() == capacity);
        auto moved = std::move(queue);
        CHECK(moved.capacity() == capacity);
        REQUIRE(queue.capacity() == 0);
    };
    if (completion) {
        auto created =
            loom::completion_queue::create(provider.domain, {.size = loom::queue_size{capacity}});
        REQUIRE(created);
        check(*created);
    } else {
        auto created =
            loom::event_queue::create(provider.fabric, {.size = loom::queue_size{capacity}});
        REQUIRE(created);
        check(*created);
    }
}

TEST_CASE("queue waits preserve valid timeouts after moving", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    const auto timeout = GENERATE(std::chrono::milliseconds{0},
                                  std::chrono::milliseconds{37},
                                  std::chrono::milliseconds{std::numeric_limits<int>::max()},
                                  std::chrono::milliseconds::min());
    queue_provider provider;
    provider.with_queue(completion, true, [&](auto& queue) {
        auto moved = std::move(queue);
        REQUIRE(moved.wait(timeout));
        REQUIRE(provider.timeout == (timeout.count() < 0 ? -1 : timeout.count()));
        REQUIRE(moved.wait());
        REQUIRE(provider.timeout == -1);
        REQUIRE_FALSE(queue.wait());
    });
    REQUIRE(provider.requested_wait == FI_WAIT_UNSPEC);
    REQUIRE(provider.calls == 2);
}

TEST_CASE("queue waits reject overflowing finite timeouts", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    using namespace std::chrono;
    const auto timeout =
        GENERATE(milliseconds{std::numeric_limits<int>::max()} + 1ms, milliseconds::max());
    queue_provider provider;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(timeout);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == loom::errc::invalid_argument);
    });
    REQUIRE(provider.calls == 0);
}

TEST_CASE("queue waits do not invent events from failed or empty reads", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EINTR, -FI_EINVAL, -FI_ETIMEDOUT);
    queue_provider provider;
    provider.status = status;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(result);
        REQUIRE(result.error() ==
                loom::make_error_code_from_fi_errno(status == 0 ? FI_EAGAIN : -status));
    });
    REQUIRE(provider.calls == 1);
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("queue waits preserve actual asynchronous error events", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE(result);
        REQUIRE(result->error == loom::errc::canceled);
    });
    REQUIRE(provider.error_reads == 1);
}

TEST_CASE("queue waits propagate error queue read failures", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.error_status = status;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(result);
        REQUIRE(result.error() ==
                loom::make_error_code_from_fi_errno(status == 0 ? FI_EAGAIN : -status));
    });
    REQUIRE(provider.error_reads == 1);
}

TEST_CASE("event polling returns no event after an empty read", "[queue][poll]") {
    const auto status = GENERATE(0, -FI_EAGAIN);
    queue_provider provider;
    provider.status = status;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    REQUIRE_FALSE(queue->poll());
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("event reads reuse native storage after moving the queue", "[queue][buffer_reuse]") {
    const auto size = GENERATE(sizeof(fi_eq_entry), 1024UZ, 1025UZ, 65537UZ);
    limited_resource resource;
    queue_provider provider;
    provider.status = -FI_EAGAIN;
    auto created = loom::event_queue::create(
        provider.fabric, {.wait_obj = true, .event_buffer_size = size}, &resource);
    REQUIRE(created);
    REQUIRE_FALSE(created->poll());
    const auto warmed_allocations = resource.allocations;
    auto queue = std::move(*created);
    for (int i = 0; i < 100; ++i) {
        REQUIRE_FALSE(queue.poll());
        REQUIRE(queue.wait(std::chrono::milliseconds{0}).error() == loom::errc::again);
        REQUIRE(provider.read_capacity == size);
        REQUIRE(reinterpret_cast<std::uintptr_t>(provider.read_buffer) %
                    alignof(std::max_align_t) ==
                0);
    }
    CHECK(resource.allocations == warmed_allocations);
    queue = {};
    REQUIRE(resource.live == 0);
}

TEST_CASE("event polling progresses while another read is waiting", "[queue][buffer_reuse]") {
    const auto size = GENERATE(1024UZ, 65537UZ);
    limited_resource resource;
    struct read_state {
        std::binary_semaphore entered{0};
        std::binary_semaphore release{0};
        void* wait_buffer{};
        void* poll_buffer{};
        bool released{};

        static auto
        write(fid_eq* eq, std::uint32_t* type, void* buffer, std::size_t size, std::byte value)
            -> ssize_t {
            *type = FI_CONNECTED;
            auto bytes = std::span{static_cast<std::byte*>(buffer), size};
            std::ranges::fill(bytes, value);
            const fi_eq_cm_entry entry{.fid = &eq->fid};
            std::memcpy(buffer, &entry, sizeof(entry));
            return static_cast<ssize_t>(size);
        }
    } state;
    queue_provider provider;
    provider.native_eq.fid.context = &state;
    provider.eq_ops.read = [](fid_eq* eq,
                              std::uint32_t* type,
                              void* buffer,
                              std::size_t size,
                              std::uint64_t) -> ssize_t {
        auto& state = *static_cast<read_state*>(eq->fid.context);
        state.poll_buffer = buffer;
        return read_state::write(eq, type, buffer, size, std::byte{0x52});
    };
    provider.eq_ops.sread =
        [](fid_eq* eq, std::uint32_t* type, void* buffer, std::size_t size, int, std::uint64_t)
        -> ssize_t {
        auto& state = *static_cast<read_state*>(eq->fid.context);
        state.wait_buffer = buffer;
        const auto count = read_state::write(eq, type, buffer, size, std::byte{0x41});
        state.entered.release();
        state.released = state.release.try_acquire_for(std::chrono::seconds{5});
        return count;
    };
    auto queue = loom::event_queue::create(
        provider.fabric, {.wait_obj = true, .event_buffer_size = size}, &resource);
    REQUIRE(queue);
    loom::result<loom::fabric_event> waited =
        loom::make_error_result<loom::fabric_event>(loom::errc::again);
    loom::test::joining_thread reader{[&] { waited = queue->wait(std::chrono::seconds{5}); }};
    REQUIRE(state.entered.try_acquire_for(std::chrono::seconds{5}));
    // A failed fallback allocation must not release the waiting reader's claim.
    resource.remaining = 0;
    CHECK_THROWS_AS(queue->poll(), std::bad_alloc);
    resource.remaining = -1;
    const auto polled = queue->poll();
    state.release.release();
    reader.join();
    REQUIRE(state.released);
    REQUIRE(state.wait_buffer != state.poll_buffer);
    REQUIRE(polled);
    REQUIRE(waited);
    REQUIRE(polled->info.size() == size - sizeof(fi_eq_cm_entry));
    REQUIRE(waited->info.size() == polled->info.size());
    REQUIRE(std::ranges::all_of(polled->info, [](auto byte) { return byte == std::byte{0x52}; }));
    REQUIRE(std::ranges::all_of(waited->info, [](auto byte) { return byte == std::byte{0x41}; }));
}

TEST_CASE("event polling preserves read failures without consuming operation errors",
          "[queue][poll]") {
    const auto status = GENERATE(-FI_EINTR, -FI_EINVAL, -FI_EIO);
    queue_provider provider;
    provider.status = status;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    const auto event = queue->poll();
    REQUIRE(event);
    REQUIRE(event->error == loom::make_error_code_from_fi_errno(-status));
    REQUIRE(event->fid == nullptr);
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("event polling preserves error entry metadata and read failures", "[queue][poll]") {
    const auto error_status = GENERATE(1, 0, -FI_EAGAIN, -FI_EIO);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.error_status = error_status;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    const auto event = queue->poll();
    REQUIRE(event);
    if (error_status == 1) {
        REQUIRE(event->error == loom::errc::canceled);
        REQUIRE(event->fid == &provider.native_eq.fid);
        REQUIRE(event->context == &provider.calls);
        REQUIRE(event->data == 42);
    } else {
        REQUIRE(event->error ==
                loom::make_error_code_from_fi_errno(error_status == 0 ? FI_EAGAIN : -error_status));
    }
    REQUIRE(provider.error_reads == 1);
}

TEST_CASE("event errors do not report a successful connection", "[queue][event_type]") {
    const auto waiting = GENERATE(false, true);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    const auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE(event->type == loom::event_type::unknown);
    REQUIRE(event->error == loom::errc::canceled);
    REQUIRE(event->prov_errno == 0);
}

TEST_CASE("unrecognized events do not report a successful connection", "[queue][event_type]") {
    const auto waiting = GENERATE(false, true);
    queue_provider provider;
    provider.native_event = std::numeric_limits<std::uint32_t>::max();
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    const auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE(event->type == loom::event_type::unknown);
    REQUIRE_FALSE(event->error);
}

TEST_CASE("control event entries retain their context and data", "[queue][event_entry]") {
    const auto waiting = GENERATE(false, true);
    const auto type = GENERATE(std::uint32_t{FI_MR_COMPLETE},
                               std::uint32_t{FI_AV_COMPLETE},
                               std::uint32_t{FI_JOIN_COMPLETE});
    queue_provider provider;
    provider.native_event = type;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    const auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE_FALSE(event->error);
    REQUIRE(event->fid == &provider.native_eq.fid);
    REQUIRE(event->context == &provider.calls);
    REQUIRE(event->data == 73);
    const auto expected = type == FI_MR_COMPLETE   ? loom::event_type::mr_complete
                          : type == FI_AV_COMPLETE ? loom::event_type::av_complete
                                                   : loom::event_type::join_complete;
    REQUIRE(event->type == expected);
}

TEST_CASE("short native event entries are rejected", "[queue][event_entry]") {
    const auto waiting = GENERATE(false, true);
    const auto type = GENERATE(std::uint32_t{FI_CONNECTED}, std::uint32_t{FI_MR_COMPLETE});
    queue_provider provider;
    provider.native_event = type;
    provider.short_entry = true;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    if (waiting) {
        const auto event = queue->wait();
        REQUIRE_FALSE(event);
        REQUIRE(event.error() == loom::make_error_code_from_fi_errno(FI_EIO));
    } else {
        const auto event = queue->poll();
        REQUIRE(event);
        REQUIRE(event->error == loom::make_error_code_from_fi_errno(FI_EIO));
    }
}

TEST_CASE("connection events own their native request information", "[queue][connreq]") {
    const auto waiting = GENERATE(false, true);
    queue_provider provider;
    provider.native_event = FI_CONNREQ;
    provider.connection_request.reset(fi_allocinfo());
    REQUIRE(provider.connection_request);
    provider.connection_request->nic = &provider.native_nic;
    provider.connection_request->caps = FI_MSG;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE_FALSE(event->error);
    REQUIRE(event->type == loom::event_type::connreq);
    REQUIRE(event->connection_info);
    REQUIRE(event->connection_info->get_caps() == loom::capability::msg);
    auto copy = *event;
    event.reset();
    *queue = {};
    REQUIRE(copy.connection_info);
    REQUIRE(copy.connection_info->get_caps() == loom::capability::msg);
    auto endpoint = loom::endpoint::create(provider.domain, *copy.connection_info);
    REQUIRE(endpoint);
    REQUIRE(provider.accepted_info == copy.connection_info->internal_ptr());
    REQUIRE(provider.freed_infos == 0);
    copy.connection_info.reset();
    REQUIRE(provider.freed_infos == 1);
}

TEST_CASE("connection information is released after allocation failures", "[queue][connreq]") {
    const auto waiting = GENERATE(false, true);
    const auto remaining = GENERATE(0, 1, 2);
    limited_resource resource;
    queue_provider provider;
    provider.native_event = FI_CONNREQ;
    provider.connection_request.reset(fi_allocinfo());
    REQUIRE(provider.connection_request);
    provider.connection_request->nic = &provider.native_nic;
    provider.private_data.assign(64, std::byte{0x42});
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting}, &resource);
    REQUIRE(queue);
    provider.status = -FI_EAGAIN;
    REQUIRE_FALSE(queue->poll());
    provider.status = 1;
    const auto before = resource.live;
    resource.remaining = remaining;
    if (waiting) {
        REQUIRE_THROWS_AS(queue->wait(), std::bad_alloc);
    } else {
        REQUIRE_THROWS_AS(queue->poll(), std::bad_alloc);
    }
    REQUIRE(resource.live == before);
    REQUIRE_FALSE(provider.connection_request);
    REQUIRE(provider.freed_infos == 1);
    provider.status = -FI_EAGAIN;
    resource.remaining = 0;
    REQUIRE_FALSE(queue->poll());
    *queue = {};
    REQUIRE(resource.live == 0);
}

TEST_CASE("connection payloads survive copies and subsequent queue reads", "[queue][payload]") {
    const auto waiting = GENERATE(false, true);
    const auto type = GENERATE(
        std::uint32_t{FI_CONNREQ}, std::uint32_t{FI_CONNECTED}, std::uint32_t{FI_SHUTDOWN});
    const auto size = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{9}, std::size_t{128});
    queue_provider provider;
    provider.native_event = type;
    provider.private_data.resize(size);
    for (std::size_t i = 0; i < size; ++i)
        provider.private_data[i] = static_cast<std::byte>(i);
    const auto expected = provider.private_data;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE_FALSE(event->error);
    REQUIRE(std::ranges::equal(event->info, expected));
    REQUIRE(event->info_len == size);
    auto copy = *event;
    event.reset();
    provider.private_data.assign(size, std::byte{0xff});
    REQUIRE(queue->poll());
    *queue = {};
    REQUIRE(std::ranges::equal(copy.info, expected));
    REQUIRE(copy.info_len == size);
}

TEST_CASE("event read buffers use their configured size and allocator", "[queue][payload]") {
    const auto method = GENERATE(0, 1, 2);
    const auto size = GENERATE(std::size_t{2048}, std::size_t{8192});
    limited_resource resource;
    queue_provider provider;
    provider.private_data.assign(size, std::byte{0x53});
    auto queue = loom::event_queue::create(
        provider.fabric,
        {.wait_obj = method == 1, .event_buffer_size = sizeof(fi_eq_cm_entry) + size},
        &resource);
    REQUIRE(queue);
    auto event = method == 0 ? queue->poll()
                             : std::optional{(method == 1 ? queue->wait() : queue->read()).value()};
    REQUIRE(event);
    REQUIRE_FALSE(event->error);
    REQUIRE(event->info.get_allocator().resource() == &resource);
    REQUIRE(std::ranges::equal(event->info, provider.private_data));
    REQUIRE(event->info_len == size);
    auto moved = std::move(*event);
    event.reset();
    *queue = {};
    REQUIRE(std::ranges::equal(moved.info, provider.private_data));
}

TEST_CASE("event read allocation failure leaves the native event queued", "[queue][payload]") {
    const auto waiting = GENERATE(false, true);
    limited_resource resource;
    queue_provider provider;
    provider.native_event = FI_CONNREQ;
    provider.connection_request.reset(fi_allocinfo());
    REQUIRE(provider.connection_request);
    provider.connection_request->nic = &provider.native_nic;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting}, &resource);
    REQUIRE(queue);
    resource.remaining = 0;
    if (waiting) {
        REQUIRE_THROWS_AS(queue->wait(), std::bad_alloc);
    } else {
        REQUIRE_THROWS_AS(queue->poll(), std::bad_alloc);
    }
    REQUIRE(provider.calls == 0);
    REQUIRE(provider.connection_request);
    REQUIRE(provider.freed_infos == 0);
    resource.remaining = -1;
    auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE(event->connection_info);
    provider.status = -FI_EAGAIN;
    resource.remaining = 0;
    REQUIRE_FALSE(queue->poll());
}

TEST_CASE("invalid event timeouts fail before allocating a read buffer", "[queue][payload]") {
    limited_resource resource;
    queue_provider provider;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = true}, &resource);
    REQUIRE(queue);
    resource.remaining = 0;
    const auto event = queue->wait(std::chrono::milliseconds::max());
    REQUIRE_FALSE(event);
    REQUIRE(event.error() == loom::errc::invalid_argument);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("event buffers must accommodate native headers", "[queue][payload]") {
    const auto size = GENERATE(std::size_t{0}, sizeof(fi_eq_entry) - 1, SIZE_MAX);
    queue_provider provider;
    const auto queue = loom::event_queue::create(provider.fabric, {.event_buffer_size = size});
    REQUIRE_FALSE(queue);
    REQUIRE(queue.error() == loom::errc::invalid_argument);
}

TEST_CASE("event errors preserve provider diagnostics across later reads", "[queue][diagnostic]") {
    const auto waiting = GENERATE(false, true);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.provider_error = 37;
    provider.error_data = {std::byte{0x52}, std::byte{0}, std::byte{0x79}, std::byte{0}};
    const auto expected = provider.error_data;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    auto event = waiting ? std::optional{queue->wait().value()} : queue->poll();
    REQUIRE(event);
    REQUIRE(event->error == loom::errc::canceled);
    CHECK(std::ranges::equal(event->info, expected));
    REQUIRE(event->prov_errno == 37);
    CHECK(event->info_len == expected.size());
    auto copy = *event;
    event.reset();
    provider.error_data.assign(expected.size(), std::byte{0xff});
    REQUIRE(queue->poll());
    CHECK(queue->event_to_string(copy) == "provider diagnostic");
    REQUIRE(provider.strerror_calls == 1);
    *queue = {};
    CHECK(std::ranges::equal(copy.info, expected));
}

TEST_CASE("generic event errors use their fabric error text", "[queue][diagnostic]") {
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    const auto event = queue->poll();
    REQUIRE(event);
    CHECK(queue->event_to_string(*event) == event->error.message());
    CHECK(provider.strerror_calls == 0);
}

TEST_CASE("successful events do not request provider error text", "[queue][diagnostic]") {
    queue_provider provider;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    const auto event = queue->poll();
    REQUIRE(event);
    REQUIRE_FALSE(event->error);
    REQUIRE_FALSE(queue->event_to_string(*event).empty());
    CHECK(provider.strerror_calls == 0);
}

TEST_CASE("invalid provider error payloads fail without reading outside the buffer",
          "[queue][diagnostic]") {
    const auto waiting = GENERATE(false, true);
    const auto missing = GENERATE(false, true);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.error_data.assign(4, std::byte{0x52});
    provider.oversized_error = !missing;
    provider.missing_error_data = missing;
    auto queue = loom::event_queue::create(provider.fabric, {.wait_obj = waiting});
    REQUIRE(queue);
    if (waiting) {
        const auto event = queue->wait();
        REQUIRE_FALSE(event);
        REQUIRE(event.error() == loom::errc::io_error);
    } else {
        const auto event = queue->poll();
        REQUIRE(event);
        REQUIRE(event->error == loom::errc::io_error);
    }
}
