// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/asio.hpp>
#include <loom/core/event_queue.hpp>
#include <loom/core/passive_endpoint.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace std::chrono_literals;

namespace {
struct failing_resource : loom::memory_resource {
    std::atomic<bool> fail_once{};
    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        if (fail_once.exchange(false))
            throw std::bad_alloc{};
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};

struct blocking_provider {
    failing_resource resource;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<iovec> buffers;
    std::vector<void*> descriptors;
    std::deque<fi_cq_tagged_entry> foreign;
    void* context{};
    std::uint64_t flags{};
    int submit_status{};
    int read_status{};
    int cancel_status{};
    int terminal_error{};
    int retired{};
    int cancellations{};
    bool posted{}, pending{}, receive{}, allowed{}, native_waiter{};

    auto post(std::span<const iovec> iov, void** desc, void* ctx, std::uint64_t op_flags, bool rx)
        -> ssize_t {
        const std::lock_guard lock{mutex};
        buffers.assign(iov.begin(), iov.end());
        if (desc)
            descriptors.assign(desc, desc + iov.size());
        context = ctx;
        flags = op_flags;
        receive = rx;
        pending = submit_status == 0;
        if (ctx)
            std::ranges::fill(static_cast<fi_context2*>(ctx)->internal, nullptr);
        posted = true;
        changed.notify_all();
        return submit_status;
    }
    void retire(std::size_t length) {
        if (receive) {
            for (const auto& buffer : buffers) {
                const auto count = std::min(length, buffer.iov_len);
                std::fill_n(static_cast<std::byte*>(buffer.iov_base), count, std::byte{0x52});
                length -= count;
            }
        }
        pending = false;
        ++retired;
    }
    auto read(void* output) -> ssize_t {
        if (read_status)
            return std::exchange(read_status, 0);
        if (!foreign.empty()) {
            *static_cast<fi_cq_tagged_entry*>(output) = foreign.front();
            foreign.pop_front();
            return 1;
        }
        if (!pending || !allowed)
            return -FI_EAGAIN;
        if (terminal_error)
            return -FI_EAVAIL;
        const auto bytes = receive ? 7UZ : 0UZ;
        retire(bytes);
        *static_cast<fi_cq_tagged_entry*>(output) = {.op_context = context, .len = bytes};
        return 1;
    }
    fi_ops resources{.size = sizeof(fi_ops),
                     .close = [](fid*) { return 0; },
                     .bind = [](fid*, fid*, std::uint64_t) { return 0; }};
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .cancel = [](fid* ep, void* context) -> ssize_t {
            auto& self = *static_cast<blocking_provider*>(ep->context);
            const std::lock_guard lock{self.mutex};
            if (context != self.context)
                return -FI_EINVAL;
            ++self.cancellations;
            if (!self.cancel_status)
                self.terminal_error = FI_ECANCELED;
            self.changed.notify_all();
            return self.cancel_status;
        },
    };
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recv =
            [](fid_ep* ep, void* data, std::size_t bytes, void*, fi_addr_t, void* ctx) {
                const iovec buffer{data, bytes};
                return static_cast<blocking_provider*>(ep->fid.context)
                    ->post({&buffer, 1}, nullptr, ctx, 0, true);
            },
        .recvmsg =
            [](fid_ep* ep, const fi_msg* message, std::uint64_t flags) {
                return static_cast<blocking_provider*>(ep->fid.context)
                    ->post({message->msg_iov, message->iov_count},
                           message->desc,
                           message->context,
                           flags,
                           true);
            },
        .send =
            [](fid_ep* ep, const void* data, std::size_t bytes, void*, fi_addr_t, void* ctx) {
                const iovec buffer{const_cast<void*>(data), bytes};
                return static_cast<blocking_provider*>(ep->fid.context)
                    ->post({&buffer, 1}, nullptr, ctx, 0, false);
            },
        .sendmsg =
            [](fid_ep* ep, const fi_msg* message, std::uint64_t flags) {
                return static_cast<blocking_provider*>(ep->fid.context)
                    ->post({message->msg_iov, message->iov_count},
                           message->desc,
                           message->context,
                           flags,
                           false);
            },
    };
    fi_ops_cq completions{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* output, std::size_t) -> ssize_t {
            auto& self = *static_cast<blocking_provider*>(cq->fid.context);
            const std::lock_guard lock{self.mutex};
            return self.read(output);
        },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* output, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<blocking_provider*>(cq->fid.context);
            const std::lock_guard lock{self.mutex};
            if (!self.pending || !self.allowed || !self.terminal_error)
                return -FI_EAGAIN;
            const auto bytes = self.terminal_error == FI_ETRUNC ? 3UZ : 0UZ;
            self.retire(bytes);
            *output = {.op_context = self.context, .len = bytes, .err = self.terminal_error};
            return 1;
        },
        .sread = [](fid_cq* cq, void* output, std::size_t, const void*, int) -> ssize_t {
            auto& self = *static_cast<blocking_provider*>(cq->fid.context);
            std::unique_lock lock{self.mutex};
            self.native_waiter = true;
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return self.pending && self.allowed; });
            return self.read(output);
        },
    };
    fid_cq cq_handle{.fid = {.context = this, .ops = &resources}, .ops = &completions};
    fid_ep ep_handle{
        .fid = {.context = this, .ops = &resources}, .ops = &endpoint_ops, .msg = &messages};
    loom::completion_queue cq{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(),
        &cq_handle,
        loom::runtime_progress_policy{},
        true,
        FI_CQ_FORMAT_TAGGED,
        0UZ,
        false,
        &resource)};
    auto endpoint(std::uint64_t rx_defaults = FI_MORE) -> loom::endpoint {
        loom::endpoint ep{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(),
            &ep_handle,
            loom::endpoint_types::msg,
            loom::address_format::inet,
            0UZ,
            0UZ,
            loom::caps{},
            FI_MORE | FI_FENCE | FI_DELIVERY_COMPLETE,
            rx_defaults)};
        REQUIRE(ep.bind_cq(cq, loom::cq_bind::transmit | loom::cq_bind::recv));
        return ep;
    }
    void release() {
        const std::lock_guard lock{mutex};
        allowed = true;
        changed.notify_all();
    }
    auto await(auto predicate) -> bool {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, 2s, predicate);
    }
};
}  // namespace

TEST_CASE("blocking Asio messages await their own completion", "[asio][blocking]") {
    const auto receive = GENERATE(false, true);
    const auto registered = GENERATE(false, true);
    const auto fail = GENERATE(false, true);
    blocking_provider native;
    native.terminal_error = fail ? (receive ? FI_ETRUNC : FI_EIO) : 0;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 8> storage{};
    fid_mr first_handle{.fid = {.ops = &native.resources}, .mem_desc = &first_handle};
    fid_mr second_handle{.fid = {.ops = &native.resources}, .mem_desc = &second_handle};
    loom::memory_region first{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &first_handle, storage.data(), 3UZ)};
    loom::memory_region second{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &second_handle, storage.data() + 3, 5UZ)};
    int unrelated{}, calls{};
    loom::completion_registration other;
    REQUIRE(other.bind(
        native.cq,
        loom::context_ptr<void>{&unrelated},
        &calls,
        [](void* count, const loom::completion_event&) { ++*static_cast<int*>(count); }));
    native.foreign.push_back({.op_context = nullptr, .data = 13});
    native.foreign.push_back({.op_context = &unrelated, .len = 5, .tag = 99});
    auto result = std::async(std::launch::async, [&] {
        if (registered) {
            const std::array buffers{
                loom::asio::mutable_registered_buffer{
                    ::asio::buffer(storage.data(), 3), &first, loom::asio::registered_buffer_id{0}},
                loom::asio::mutable_registered_buffer{::asio::buffer(storage.data() + 3, 5),
                                                      &second,
                                                      loom::asio::registered_buffer_id{1}}};
            return receive ? ep.receive(buffers) : ep.send(buffers);
        }
        const std::array buffers{::asio::buffer(storage.data(), 3),
                                 ::asio::buffer(storage.data() + 3, 5)};
        return receive ? ep.receive(buffers) : ep.send(buffers);
    });
    CHECK(native.await([&] { return native.posted; }));
    CHECK(result.wait_for(20ms) == std::future_status::timeout);
    native.release();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto [error, bytes] = result.get();
    CHECK(error ==
          (fail ? loom::make_error_code_from_fi_errno(native.terminal_error) : std::error_code{}));
    CHECK(bytes == (receive ? (fail ? 3 : 7) : (fail ? 0 : 8)));
    CHECK(native.retired == 1);
    CHECK_FALSE(native.pending);
    CHECK(native.buffers.size() == 2);
    CHECK(native.flags == (FI_COMPLETION | (receive ? 0 : FI_FENCE | FI_DELIVERY_COMPLETE)));
    CHECK(native.context != nullptr);
    CHECK(native.descriptors ==
          (registered ? std::vector<void*>{&first_handle, &second_handle} : std::vector<void*>{}));
    if (receive) {
        CHECK(std::ranges::all_of(std::span{storage}.first(bytes),
                                  [](auto byte) { return byte == std::byte{0x52}; }));
        CHECK(storage.back() == std::byte{});
    }
    CHECK(calls == 0);
    const auto notification = native.cq.read().value();
    CHECK_FALSE(notification.context);
    CHECK(notification.data == 13);
    const auto foreign = native.cq.read().value();
    CHECK(foreign.context.get() == &unrelated);
    CHECK(foreign.bytes_transferred == 5);
    CHECK(foreign.tag == 99);
    REQUIRE(native.cq.dispatch(foreign));
    CHECK(calls == 1);
    CHECK_FALSE(native.cq.poll());
    CHECK(ioc.poll() == 0);
}

TEST_CASE("blocking Asio completion can be dispatched by another reader", "[asio][blocking]") {
    const auto receive = GENERATE(false, true);
    blocking_provider native;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 16> storage{};
    auto reader = std::async(std::launch::async, [&] {
        const auto event = native.cq.wait().value();
        return native.cq.dispatch(event);
    });
    CHECK(native.await([&] { return native.native_waiter; }));
    auto result = std::async(std::launch::async, [&] {
        return receive ? ep.receive(::asio::buffer(storage)) : ep.send(::asio::buffer(storage));
    });
    CHECK(native.await([&] { return native.posted; }));
    CHECK(result.wait_for(20ms) == std::future_status::timeout);
    native.release();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto [error, bytes] = result.get();
    REQUIRE(reader.get());
    CHECK_FALSE(error);
    CHECK(bytes == (receive ? 7 : 16));
    CHECK(native.retired == 1);
    CHECK_FALSE(native.pending);
    CHECK(ioc.poll() == 0);
}

TEST_CASE("blocking Asio faults retain accepted work until retirement", "[asio][blocking]") {
    const auto receive = GENERATE(false, true);
    const auto allocation_failure = GENERATE(false, true);
    const auto cancellable = GENERATE(false, true);
    blocking_provider native;
    native.cancel_status = cancellable ? 0 : -FI_ENOSYS;
    if (allocation_failure)
        native.resource.fail_once = true;
    else
        native.read_status = -FI_EIO;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 16> storage{};
    auto result = std::async(std::launch::async, [&] {
        return receive ? ep.receive(::asio::buffer(storage)) : ep.send(::asio::buffer(storage));
    });
    CHECK(native.await([&] { return native.cancellations != 0; }));
    CHECK(result.wait_for(20ms) == std::future_status::timeout);
    {
        loom::completion_registration duplicate;
        const auto bound = duplicate.bind(native.cq,
                                          loom::context_ptr<void>{native.context},
                                          nullptr,
                                          [](void*, const loom::completion_event&) {});
        CHECK_FALSE(bound);
        CHECK(bound.error() == loom::errc::address_in_use);
    }
    native.release();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    if (allocation_failure) {
        REQUIRE_THROWS_AS(result.get(), std::bad_alloc);
    } else {
        const auto [error, bytes] = result.get();
        CHECK(error == loom::errc::io_error);
        CHECK(bytes == (receive && !cancellable ? 7 : 0));
    }
    CHECK(native.retired == 1);
    CHECK(native.cancellations == 1);
    CHECK_FALSE(native.pending);
    loom::completion_event old;
    old.context = loom::context_ptr<void>{native.context};
    CHECK_FALSE(native.cq.dispatch(old));
}

TEST_CASE("blocking Asio rejects unsupported receives before submission", "[asio][blocking]") {
    const auto multi = GENERATE(false, true);
    blocking_provider native;
    if (!multi) {
        native.cq = loom::completion_queue{
            loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
                loom::get_default_resource(),
                &native.cq_handle,
                loom::runtime_progress_policy{},
                false,
                FI_CQ_FORMAT_CONTEXT)};
    }
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint(multi ? FI_MULTI_RECV : 0)};
    std::array<std::byte, 16> storage{};
    const auto [error, bytes] = ep.receive(::asio::buffer(storage));
    CHECK(error == loom::errc::not_supported);
    CHECK(bytes == 0);
    CHECK_FALSE(native.posted);
}

TEST_CASE("blocking Asio submission errors do not wait for a completion", "[asio][blocking]") {
    const auto receive = GENERATE(false, true);
    const auto status = GENERATE(-FI_EAGAIN, -FI_EIO);
    blocking_provider native;
    native.submit_status = status;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 16> storage{};
    const auto [error, bytes] =
        receive ? ep.receive(::asio::buffer(storage)) : ep.send(::asio::buffer(storage));
    CHECK(error == loom::make_error_code_from_fi_errno(-status));
    CHECK(bytes == 0);
    CHECK(native.retired == 0);
    CHECK(native.cancellations == 0);
    CHECK_FALSE(native.pending);
    loom::completion_event old;
    old.context = loom::context_ptr<void>{native.context};
    CHECK_FALSE(native.cq.dispatch(old));
}

TEST_CASE("blocking Asio transfers a short message across native endpoints",
          "[asio][blocking][.provider]") {
    const auto provider = GENERATE("sockets", "tcp");
    CAPTURE(provider);
    const auto checked = [](auto result) {
        if (!result)
            FAIL(result.error().message());
        return std::move(result).value();
    };
    loom::fabric_hints hints;
    hints.capabilities = loom::capability::msg;
    hints.mode_bits = loom::mode_bits::context | loom::mode_bits::context2;
    hints.addr_format = loom::address_format::inet;
    hints.ep_type = loom::endpoint_types::msg;
    hints.provider_name = provider;
    hints.domain_name = "lo";
    hints.threading = loom::threading_mode::safe;
    auto info = loom::query_fabric(hints).value();
    auto fabric = loom::fabric::create(info).value();
    auto domain = loom::domain::create(fabric, info).value();
    auto server_eq = loom::event_queue::create(fabric, {.wait_obj = true}).value();
    auto client_eq = loom::event_queue::create(fabric, {.wait_obj = true}).value();
    auto server_cq = loom::completion_queue::create(domain).value();
    auto client_cq = loom::completion_queue::create(domain).value();
    auto listener = loom::passive_endpoint::create(fabric, info).value();
    REQUIRE(listener.bind_eq(server_eq));
    REQUIRE(listener.listen());
    auto client = loom::endpoint::create(domain, info).value();
    REQUIRE(client.bind_eq(client_eq));
    REQUIRE(client.bind_cq(client_cq,
                           loom::cq_bind::transmit | loom::cq_bind::recv |
                               loom::cq_bind::selective_completion));
    REQUIRE(client.enable());
    REQUIRE(client.connect(checked(listener.get_local_address())));
    std::optional<loom::fabric_event> request;
    bool client_connected{}, server_connected{};
    const auto progress_connections = [&] {
        // Providers may require data progress to advance connection setup.
        REQUIRE(client_cq.progress());
        REQUIRE(server_cq.progress());
        if (auto event = client_eq.poll()) {
            REQUIRE_FALSE(event->error);
            REQUIRE(event->type == loom::event_type::connected);
            client_connected = true;
        }
        if (auto event = server_eq.poll()) {
            REQUIRE_FALSE(event->error);
            if (event->type == loom::event_type::connreq)
                request = std::move(*event);
            else {
                REQUIRE(event->type == loom::event_type::connected);
                server_connected = true;
            }
        }
    };
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!request && std::chrono::steady_clock::now() < deadline) {
        progress_connections();
        std::this_thread::yield();
    }
    REQUIRE(request);
    REQUIRE(request->connection_info);
    auto server = loom::endpoint::create(domain, *request->connection_info).value();
    REQUIRE(server.bind_eq(server_eq));
    REQUIRE(server.bind_cq(server_cq,
                           loom::cq_bind::transmit | loom::cq_bind::recv |
                               loom::cq_bind::selective_completion));
    REQUIRE(server.enable());
    REQUIRE(server.accept());
    deadline = std::chrono::steady_clock::now() + 2s;
    while ((!client_connected || !server_connected) &&
           std::chrono::steady_clock::now() < deadline) {
        progress_connections();
        std::this_thread::yield();
    }
    REQUIRE(client_connected);
    REQUIRE(server_connected);
    ::asio::io_context ioc;
    loom::asio::endpoint sender{ioc.get_executor(), std::move(client)};
    loom::asio::endpoint receiver{ioc.get_executor(), std::move(server)};
    std::array<std::byte, 7> source;
    source.fill(std::byte{0x72});
    std::array<std::byte, 16> destination;
    destination.fill(std::byte{0x63});
    const std::array send_buffers{::asio::buffer(source.data(), 3),
                                  ::asio::buffer(source.data() + 3, 4)};
    const std::array recv_buffers{::asio::buffer(destination.data(), 4),
                                  ::asio::buffer(destination.data() + 4, 12)};
    auto send_mrs =
        loom::asio::register_buffers(ioc, domain, send_buffers, loom::mr_access_flags::send)
            .value();
    auto recv_mrs =
        loom::asio::register_buffers(ioc, domain, recv_buffers, loom::mr_access_flags::recv)
            .value();
    const std::array registered_sends{send_mrs[0], send_mrs[1]};
    const std::array registered_recvs{recv_mrs[0], recv_mrs[1]};
    auto received =
        std::async(std::launch::async, [&] { return receiver.receive(registered_recvs); });
    CHECK(received.wait_for(20ms) == std::future_status::timeout);
    const auto [send_error, sent] = sender.send(registered_sends);
    REQUIRE_FALSE(send_error);
    REQUIRE(sent == source.size());
    REQUIRE(received.wait_for(2s) == std::future_status::ready);
    const auto [recv_error, bytes] = received.get();
    REQUIRE_FALSE(recv_error);
    REQUIRE(bytes == source.size());
    REQUIRE(std::ranges::equal(source, std::span{destination}.first(bytes)));
    REQUIRE(std::ranges::all_of(std::span{destination}.subspan(bytes),
                                [](auto byte) { return byte == std::byte{0x63}; }));
    REQUIRE(ioc.poll() == 0);
}
