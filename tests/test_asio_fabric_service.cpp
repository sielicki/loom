// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_fabric_service.cpp
 * @brief Tests for loom::asio::fabric_service.
 */

#include <loom/asio.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <latch>
#include <thread>
#include <vector>

#include "joining_thread.hpp"
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace std::literals;

namespace {
struct service_queue {
    std::deque<fi_cq_tagged_entry> pending;
    std::optional<fi_cq_err_entry> error;
    std::function<void()> before_read;
    int reads{};

    static auto read(fid_cq* cq, void* output, std::size_t) -> ssize_t {
        auto& self = *static_cast<service_queue*>(cq->fid.context);
        ++self.reads;
        if (self.before_read)
            self.before_read();
        if (self.error)
            return -FI_EAVAIL;
        if (self.pending.empty())
            return -FI_EAGAIN;
        *static_cast<fi_cq_tagged_entry*>(output) = self.pending.front();
        self.pending.pop_front();
        return 1;
    }
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq completions{
        .size = sizeof(fi_ops_cq),
        .read = read,
        .readfrom = [](fid_cq* cq, void* output, std::size_t count, fi_addr_t* source) -> ssize_t {
            *source = 37;
            return read(cq, output, count);
        },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* output, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<service_queue*>(cq->fid.context);
            if (!self.error)
                return -FI_EAGAIN;
            auto* storage = output->err_data;
            if (output->err_data_size < self.error->err_data_size)
                return -FI_ETOOSMALL;
            if (self.error->err_data_size)
                std::memcpy(storage, self.error->err_data, self.error->err_data_size);
            *output = *std::exchange(self.error, std::nullopt);
            output->err_data = storage;
            return 1;
        },
    };
    fid_cq handle{.fid = {.context = this, .ops = &ops}, .ops = &completions};
    loom::detail::completion_queue_impl* implementation{};
    loom::completion_queue queue{[this] {
        auto impl = loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
            loom::get_default_resource(),
            &handle,
            loom::runtime_progress_policy{},
            false,
            FI_CQ_FORMAT_TAGGED,
            64,
            true);
        impl->max_error_data = 64;
        implementation = impl.get();
        return impl;
    }()};
};

#if defined(ASIO_HAS_POSIX_STREAM_DESCRIPTOR)
struct fd_service_queue : service_queue {
    std::array<int, 2> descriptors{-1, -1};
    fi_wait_obj wait_object{FI_WAIT_FD};
    int wait_calls{};
    bool valid_wait_arguments{true};
    int wait_status{};
    int descriptor_queries{};
    std::function<void()> before_wait;
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .trywait =
            [](fid_fabric* fabric, fid** queues, int count) {
                auto& self = *static_cast<fd_service_queue*>(fabric->fid.context);
                self.valid_wait_arguments &= count == 1 && queues[0] == &self.handle.fid;
                ++self.wait_calls;
                if (self.before_wait)
                    self.before_wait();
                char byte;
                while (::read(self.descriptors[0], &byte, 1) == 1) {}
                if (self.wait_status)
                    return self.wait_status;
                return self.pending.empty() && !self.error ? 0 : -FI_EAGAIN;
            },
    };
    fid_fabric fabric{.fid = {.context = this}, .ops = &fabric_ops};

    fd_service_queue() {
        REQUIRE(::pipe(descriptors.data()) == 0);
        for (const auto fd : descriptors) {
            REQUIRE(::fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
            REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
        }
        ops.control = [](fid* queue, int command, void* output) {
            auto& self =
                *static_cast<fd_service_queue*>(static_cast<service_queue*>(queue->context));
            if (command == FI_GETWAITOBJ) {
                *static_cast<fi_wait_obj*>(output) = self.wait_object;
                return 0;
            }
            if (command == FI_GETWAIT) {
                ++self.descriptor_queries;
                *static_cast<int*>(output) = self.descriptors[0];
                return 0;
            }
            return -FI_ENOSYS;
        };
        implementation->parent_fabric = &fabric;
    }
    ~fd_service_queue() {
        for (const auto fd : descriptors)
            ::close(fd);
    }
    void signal() {
        constexpr char byte{1};
        REQUIRE(::write(descriptors[1], &byte, 1) == 1);
    }
};
#endif
}  // namespace

TEST_CASE("poll_once reports dispatched completions and honors the limit",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    std::vector<void*> pending;
    int completed{};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq completions{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* output, std::size_t) -> ssize_t {
            auto& entries = *static_cast<std::vector<void*>*>(cq->fid.context);
            if (entries.empty()) {
                return -FI_EAGAIN;
            }
            *static_cast<fi_cq_tagged_entry*>(output) = {.op_context = entries.back(), .len = 42};
            entries.pop_back();
            return 1;
        },
    };
    fid_cq handle{.fid = {.context = &pending, .ops = &ops}, .ops = &completions};
    loom::completion_queue cq{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &handle)};
    for (int i = 0; i < 2; ++i) {
        auto* context = loom::asio::make_asio_context(
            [&](std::error_code error, std::size_t bytes) {
                REQUIRE_FALSE(error);
                REQUIRE(bytes == 42);
                ++completed;
            },
            ioc.get_executor());
        REQUIRE(context->bind_completion(cq));
        pending.push_back(context->as_fi_context());
    }
    auto& service = loom::asio::fabric_service::use(ioc);
    loom::asio::fabric_service_options options;
    options.max_completions_per_poll = 1;
    service.register_cq(cq, options);
    service.register_cq(cq, {.max_completions_per_poll = 2});
    REQUIRE(service.poll_once() == 1);
    REQUIRE(service.poll_once() == 1);
    REQUIRE(service.poll_once() == 0);
    ioc.run();
    REQUIRE(completed == 2);
    REQUIRE_FALSE(service.deregister_cq(cq));
}

TEST_CASE("fabric_service creation", "[asio][fabric_service]") {
    ::asio::io_context ioc;

    REQUIRE_FALSE(loom::asio::fabric_service::has_service(ioc));

    [[maybe_unused]] auto& service = loom::asio::fabric_service::use(ioc);

    REQUIRE(loom::asio::fabric_service::has_service(ioc));
}

TEST_CASE("fabric_service use returns same instance", "[asio][fabric_service]") {
    ::asio::io_context ioc;

    auto& service1 = loom::asio::fabric_service::use(ioc);
    auto& service2 = loom::asio::fabric_service::use(ioc);

    REQUIRE(&service1 == &service2);
}

TEST_CASE("fabric_service creation is safe across threads", "[asio][fabric_service]") {
    for (int iteration = 0; iteration < 20; ++iteration) {
        ::asio::io_context ioc;
        std::array<loom::asio::fabric_service*, 8> services{};
        std::array<std::exception_ptr, 8> errors{};
        std::barrier ready{8};
        {
            std::array<loom::test::joining_thread, 8> threads;
            for (std::size_t i = 0; i < threads.size(); ++i) {
                threads[i] = loom::test::joining_thread{[&, i] {
                    ready.arrive_and_wait();
                    try {
                        services[i] = &loom::asio::fabric_service::use(ioc);
                    } catch (...) {
                        errors[i] = std::current_exception();
                    }
                }};
            }
        }
        for (std::size_t i = 0; i < services.size(); ++i) {
            REQUIRE_FALSE(errors[i]);
            REQUIRE(services[i] == services.front());
        }
    }
}

TEST_CASE("fabric_service start and stop", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    REQUIRE_FALSE(service.is_running());

    service.start();
    REQUIRE(service.is_running());

    service.stop();
    REQUIRE_FALSE(service.is_running());
}

TEST_CASE("fabric_service preserves polling state across a parent fork notification",
          "[asio][fabric_service]") {
    const bool running = GENERATE(false, true);
    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);
    if (running) {
        service.start();
    }
    ioc.notify_fork(::asio::execution_context::fork_prepare);
    REQUIRE_FALSE(service.is_running());
    ioc.notify_fork(::asio::execution_context::fork_parent);
    REQUIRE(service.is_running() == running);
    service.stop();
}

TEST_CASE("fabric_service accepts its first queue after starting", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    loom::completion_queue cq;
    auto& service = loom::asio::fabric_service::use(ioc);
    service.start();
    service.register_cq(cq);
    REQUIRE(service.is_running());
    REQUIRE_FALSE(service.deregister_cq(cq));
    service.stop();
}

TEST_CASE("fabric_service register and deregister cq", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    service.register_cq(*cq_result);
    REQUIRE_FALSE(service.deregister_cq(*cq_result));
}

TEST_CASE("fabric_service poll_once with no completions", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    service.register_cq(*cq_result);

    auto count = service.poll_once();
    REQUIRE(count == 0UL);

    REQUIRE_FALSE(service.deregister_cq(*cq_result));
}

TEST_CASE("fabric_service timer-based polling runs", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    loom::asio::fabric_service::options opts{
        .poll_interval = std::chrono::microseconds{1000},
        .max_completions_per_poll = 16,
    };
    service.register_cq(*cq_result, opts);
    service.start();

    ::asio::steady_timer timer(ioc);
    timer.expires_after(5ms);
    timer.async_wait([&service](const std::error_code&) { service.stop(); });

    ioc.run();

    REQUIRE_FALSE(service.is_running());
    REQUIRE_FALSE(service.deregister_cq(*cq_result));
}

TEST_CASE("fabric_service with multiple completion queues", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq1_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{32UL}});
    auto cq2_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{32UL}});
    REQUIRE(cq1_result);
    REQUIRE(cq2_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    service.register_cq(*cq1_result);
    service.register_cq(*cq2_result);

    auto count = service.poll_once();
    REQUIRE(count == 0UL);

    REQUIRE_FALSE(service.deregister_cq(*cq1_result));
    REQUIRE_FALSE(service.deregister_cq(*cq2_result));
}

TEST_CASE("Asio service preserves unclaimed events without blocking other queues",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue first, second;
    auto& service = loom::asio::fabric_service::use(ioc);
    fi_context2 foreign{}, known{}, other{};
    std::byte storage{};
    const std::array<void*, 3> unknown{nullptr, &foreign, reinterpret_cast<void*>(1)};
    for (auto* context : unknown)
        first.pending.push_back({.op_context = context,
                                 .flags = FI_REMOTE_CQ_DATA,
                                 .len = 7,
                                 .buf = &storage,
                                 .data = 11,
                                 .tag = 13});
    int callbacks{};
    loom::completion_registration registration, other_registration;
    auto complete = [](void* object, const loom::completion_event&) {
        ++*static_cast<int*>(object);
    };
    REQUIRE(registration.bind(first.queue, loom::context_ptr<void>{&known}, &callbacks, complete));
    REQUIRE(other_registration.bind(
        second.queue, loom::context_ptr<void>{&other}, &callbacks, complete));
    auto* asio_context = loom::asio::make_asio_context(
        [&](std::error_code error, std::size_t bytes) {
            REQUIRE_FALSE(error);
            REQUIRE(bytes == 17);
            ++callbacks;
        },
        ioc.get_executor());
    REQUIRE(asio_context->bind_completion(first.queue));
    first.pending.push_back({.op_context = &known});
    first.pending.push_back({.op_context = asio_context->as_fi_context(), .len = 17});
    second.pending.push_back({.op_context = &other});
    service.register_cq(first.queue);
    service.register_cq(second.queue);
    REQUIRE_FALSE(service.take_unhandled(first.queue));
    for (std::size_t i = 0; i < unknown.size(); ++i) {
        REQUIRE(service.poll_once() == (i == 0 ? 1 : 0));
        const auto reads = first.reads;
        REQUIRE(service.poll_once() == 0);
        REQUIRE(first.reads == reads);
        REQUIRE(callbacks == 1);
        auto event = service.take_unhandled(first.queue);
        REQUIRE(event);
        REQUIRE(event->context.get() == unknown[i]);
        REQUIRE(event->flags == FI_REMOTE_CQ_DATA);
        REQUIRE(event->bytes_transferred == 7);
        REQUIRE(event->buffer == &storage);
        REQUIRE(event->source == loom::fabric_addr{37});
        REQUIRE(event->data == 11);
        REQUIRE(event->tag == 13);
        REQUIRE_FALSE(service.take_unhandled(first.queue));
    }
    REQUIRE(service.poll_once() == 2);
    ioc.run();
    REQUIRE(callbacks == 3);
    REQUIRE_FALSE(service.deregister_cq(first.queue));
    REQUIRE_FALSE(service.deregister_cq(second.queue));
}

TEST_CASE("Asio service returns owned unclaimed errors on removal", "[asio][fabric_service]") {
    const bool remove = GENERATE(false, true);
    ::asio::io_context ioc;
    service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    std::array<std::byte, 3> diagnostic{std::byte{1}, std::byte{2}, std::byte{3}};
    native.error.emplace();
    native.error->err = FI_EIO;
    native.error->prov_errno = 23;
    native.error->len = 5;
    native.error->data = 7;
    native.error->tag = 11;
    native.error->err_data = diagnostic.data();
    native.error->err_data_size = diagnostic.size();
    service.register_cq(native.queue);
    REQUIRE(service.poll_once() == 0);
    const auto reads = native.reads;
    REQUIRE(service.poll_once() == 0);
    REQUIRE(native.reads == reads);
    auto event =
        remove ? service.deregister_cq(native.queue) : service.take_unhandled(native.queue);
    REQUIRE(event);
    REQUIRE_FALSE(service.take_unhandled(native.queue));
    REQUIRE_FALSE(service.deregister_cq(native.queue));
    native.queue = loom::completion_queue{};
    diagnostic.fill(std::byte{});
    REQUIRE(event->context.get() == nullptr);
    REQUIRE(event->error == loom::errc::io_error);
    REQUIRE(event->bytes_transferred == 5);
    REQUIRE(event->data == 7);
    REQUIRE(event->tag == 11);
    REQUIRE(event->error_info.prov_errno == 23);
    REQUIRE(event->error_info.err_data_size == 3);
    REQUIRE(event->error_info.err_data[0] == std::byte{1});
    REQUIRE(event->error_info.err_data[2] == std::byte{3});
}

TEST_CASE("Asio callbacks may deregister and destroy queues during polling",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue first, second, added;
    auto& service = loom::asio::fabric_service::use(ioc);
    struct state {
        loom::asio::fabric_service& service;
        service_queue& first;
        service_queue& second;
        service_queue& added;
        int calls{};
    } state{service, first, second, added};
    loom::completion_registration registration;
    fi_context2 context{};
    REQUIRE(registration.bind(first.queue,
                              loom::context_ptr<void>{&context},
                              &state,
                              [](void* object, const loom::completion_event&) {
                                  auto& state = *static_cast<struct state*>(object);
                                  ++state.calls;
                                  REQUIRE_FALSE(state.service.deregister_cq(state.first.queue));
                                  REQUIRE_FALSE(state.service.deregister_cq(state.second.queue));
                                  state.first.queue = loom::completion_queue{};
                                  state.second.queue = loom::completion_queue{};
                                  state.service.register_cq(state.added.queue);
                              }));
    first.pending.push_back({.op_context = &context});
    service.register_cq(first.queue);
    service.register_cq(second.queue);
    REQUIRE(service.poll_once() == 1);
    REQUIRE(state.calls == 1);
    REQUIRE(first.reads == 1);
    REQUIRE(second.reads == 0);
    REQUIRE(added.reads == 0);
    REQUIRE(service.poll_once() == 0);
    REQUIRE(added.reads == 1);
    REQUIRE_FALSE(service.deregister_cq(added.queue));
}

TEST_CASE("Asio deregistration waits for an in-flight queue read", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    std::latch reading{1}, release_read{1}, removing{1};
    native.before_read = [&] {
        reading.count_down();
        release_read.wait();
    };
    service.register_cq(native.queue);
    std::size_t count{};
    loom::test::joining_thread poller{[&] { count = service.poll_once(); }};
    reading.wait();
    std::promise<std::optional<loom::completion_event>> result;
    auto removed = result.get_future();
    loom::test::joining_thread remover{[&] {
        removing.count_down();
        result.set_value(service.deregister_cq(native.queue));
    }};
    removing.wait();
    const bool waited = removed.wait_for(20ms) == std::future_status::timeout;
    release_read.count_down();
    poller.join();
    remover.join();
    REQUIRE(waited);
    REQUIRE_FALSE(removed.get());
    REQUIRE(count == 0);
    REQUIRE(native.reads == 1);
    native.queue = loom::completion_queue{};
    REQUIRE(service.poll_once() == 0);
}

TEST_CASE("nested Asio polling preserves entries while registration grows",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue first, removed, remaining;
    std::array<service_queue, 64> added;
    auto& service = loom::asio::fabric_service::use(ioc);
    auto callback = [&] {
        REQUIRE_FALSE(service.deregister_cq(first.queue));
        REQUIRE_FALSE(service.deregister_cq(removed.queue));
        for (auto& native : added)
            service.register_cq(native.queue);
        REQUIRE(service.poll_once() == 0);
    };
    fi_context2 context{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(first.queue,
                              loom::context_ptr<void>{&context},
                              &callback,
                              [](void* object, const loom::completion_event&) {
                                  (*static_cast<decltype(callback)*>(object))();
                              }));
    first.pending.push_back({.op_context = &context});
    for (auto* native : {&first, &removed, &remaining})
        service.register_cq(native->queue);
    REQUIRE(service.poll_once() == 1);
    REQUIRE(first.reads == 1);
    REQUIRE(removed.reads == 0);
    REQUIRE(remaining.reads == 2);
    for (const auto& native : added)
        REQUIRE(native.reads == 1);
    REQUIRE(service.poll_once() == 0);
    REQUIRE(remaining.reads == 3);
    for (auto& native : added) {
        REQUIRE(native.reads == 2);
        REQUIRE_FALSE(service.deregister_cq(native.queue));
    }
    REQUIRE_FALSE(service.deregister_cq(remaining.queue));
}

TEST_CASE("Asio registration changes survive a throwing completion callback",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue first, removed;
    auto& service = loom::asio::fabric_service::use(ioc);
    struct handler_error {};
    auto callback = [&] {
        REQUIRE_FALSE(service.deregister_cq(first.queue));
        REQUIRE_FALSE(service.deregister_cq(removed.queue));
        service.register_cq(first.queue);
        throw handler_error{};
    };
    fi_context2 context{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(first.queue,
                              loom::context_ptr<void>{&context},
                              &callback,
                              [](void* object, const loom::completion_event&) {
                                  (*static_cast<decltype(callback)*>(object))();
                              }));
    first.pending.push_back({.op_context = &context});
    first.pending.push_back({.data = 42});
    service.register_cq(first.queue);
    service.register_cq(removed.queue);
    REQUIRE_THROWS_AS(service.poll_once(), handler_error);
    REQUIRE(first.reads == 1);
    REQUIRE(removed.reads == 0);
    REQUIRE(service.poll_once() == 0);
    const auto unhandled = service.deregister_cq(first.queue);
    REQUIRE(unhandled);
    REQUIRE(unhandled->data == 42);
    REQUIRE(service.poll_once() == 0);
    REQUIRE(first.reads == 2);
    REQUIRE(removed.reads == 0);
}

TEST_CASE("Asio fork notification invalidates queues in an active poll", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue first, removed, added;
    auto& service = loom::asio::fabric_service::use(ioc);
    auto callback = [&] {
        ioc.notify_fork(::asio::execution_context::fork_prepare);
        ioc.notify_fork(::asio::execution_context::fork_child);
        service.register_cq(added.queue);
    };
    fi_context2 context{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(first.queue,
                              loom::context_ptr<void>{&context},
                              &callback,
                              [](void* object, const loom::completion_event&) {
                                  (*static_cast<decltype(callback)*>(object))();
                              }));
    first.pending.push_back({.op_context = &context});
    service.register_cq(first.queue);
    service.register_cq(removed.queue);
    REQUIRE(service.poll_once() == 1);
    REQUIRE(first.reads == 1);
    REQUIRE(removed.reads == 0);
    REQUIRE(added.reads == 0);
    REQUIRE(service.poll_once() == 0);
    REQUIRE(added.reads == 1);
    REQUIRE_FALSE(service.deregister_cq(added.queue));
}

TEST_CASE("Asio callback replacement of every queue reschedules the timer",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue first, added;
    auto& service = loom::asio::fabric_service::use(ioc);
    bool completed{};
    auto replace = [&] {
        REQUIRE_FALSE(service.deregister_cq(first.queue));
        service.register_cq(added.queue, {.poll_interval = 0us, .use_fd_polling = false});
    };
    auto complete = [&] {
        completed = true;
        service.stop();
    };
    std::array<fi_context2, 2> contexts{};
    std::array<loom::completion_registration, 2> registrations;
    REQUIRE(registrations[0].bind(first.queue,
                                  loom::context_ptr<void>{&contexts[0]},
                                  &replace,
                                  [](void* object, const loom::completion_event&) {
                                      (*static_cast<decltype(replace)*>(object))();
                                  }));
    REQUIRE(registrations[1].bind(added.queue,
                                  loom::context_ptr<void>{&contexts[1]},
                                  &complete,
                                  [](void* object, const loom::completion_event&) {
                                      (*static_cast<decltype(complete)*>(object))();
                                  }));
    first.pending.push_back({.op_context = &contexts[0]});
    added.pending.push_back({.op_context = &contexts[1]});
    service.register_cq(first.queue, {.poll_interval = 1h, .use_fd_polling = false});
    service.start();
    REQUIRE(service.poll_once() == 1);
    ioc.run_for(100ms);
    service.stop();
    REQUIRE(completed);
    REQUIRE_FALSE(service.deregister_cq(added.queue));
}

TEST_CASE("Asio timer honors the shortest registered interval", "[asio][fabric_service]") {
    enum class registration_order { fast_first, slow_first, while_running };
    const auto order = GENERATE(registration_order::fast_first,
                                registration_order::slow_first,
                                registration_order::while_running);
    ::asio::io_context ioc;
    service_queue slow, fast;
    auto& service = loom::asio::fabric_service::use(ioc);
    constexpr loom::asio::fabric_service_options slow_options{.poll_interval = 1h,
                                                              .use_fd_polling = false};
    constexpr loom::asio::fabric_service_options fast_options{.poll_interval = 0us,
                                                              .use_fd_polling = false};
    bool completed{};
    auto callback = [&] {
        completed = true;
        service.stop();
    };
    fi_context2 context{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(fast.queue,
                              loom::context_ptr<void>{&context},
                              &callback,
                              [](void* object, const loom::completion_event&) {
                                  (*static_cast<decltype(callback)*>(object))();
                              }));
    fast.pending.push_back({.op_context = &context});
    if (order == registration_order::fast_first)
        service.register_cq(fast.queue, fast_options);
    service.register_cq(slow.queue, slow_options);
    if (order == registration_order::while_running)
        service.start();
    if (order != registration_order::fast_first)
        service.register_cq(fast.queue, fast_options);
    service.start();
    ioc.run_for(100ms);
    service.stop();
    REQUIRE(completed);
    REQUIRE_FALSE(service.deregister_cq(slow.queue));
    REQUIRE_FALSE(service.deregister_cq(fast.queue));
}

TEST_CASE("reentrant Asio progress cannot overwrite an unclaimed event", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    fi_context2 context{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(
        native.queue,
        loom::context_ptr<void>{&context},
        &service,
        [](void* object, const loom::completion_event&) {
            REQUIRE(static_cast<loom::asio::fabric_service*>(object)->poll_once() == 0);
        }));
    native.pending.push_back({.op_context = &context});
    native.pending.push_back({.data = 7});
    native.pending.push_back({.data = 11});
    service.register_cq(native.queue);
    REQUIRE(service.poll_once() == 1);
    REQUIRE(native.reads == 2);
    REQUIRE(native.pending.size() == 1);
    const auto first = service.take_unhandled(native.queue);
    REQUIRE(first);
    REQUIRE(first->data == 7);
    REQUIRE(service.poll_once() == 0);
    const auto second = service.deregister_cq(native.queue);
    REQUIRE(second);
    REQUIRE(second->data == 11);
}

TEST_CASE("Asio timer progress resumes after a registered callback throws",
          "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    struct handler_error {};
    struct state {
        loom::asio::fabric_service& service;
        int callbacks{};
    } state{service};
    std::array<fi_context2, 2> contexts{};
    std::array<loom::completion_registration, 2> registrations;
    for (std::size_t index = 0; index < contexts.size(); ++index) {
        REQUIRE(registrations[index].bind(native.queue,
                                          loom::context_ptr<void>{&contexts[index]},
                                          &state,
                                          [](void* object, const loom::completion_event&) {
                                              auto& state = *static_cast<struct state*>(object);
                                              if (++state.callbacks == 1)
                                                  throw handler_error{};
                                              state.service.stop();
                                          }));
        native.pending.push_back({.op_context = &contexts[index]});
    }
    service.register_cq(native.queue, {.poll_interval = 0us, .max_completions_per_poll = 1});
    service.start();
    REQUIRE_THROWS_AS(ioc.run_one(), handler_error);
    REQUIRE(state.callbacks == 1);
    ioc.run_for(100ms);
    const bool resumed = state.callbacks == 2;
    service.stop();
    // Retire the second operation even if the timer failed to resume.
    service.poll_once();
    ioc.restart();
    ioc.poll();
    REQUIRE_FALSE(service.deregister_cq(native.queue));
    REQUIRE(resumed);
    REQUIRE(state.callbacks == 2);
}

TEST_CASE("Asio timer preserves a stop requested by a throwing handler", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    struct handler_error {};
    auto* context = loom::asio::make_asio_context(
        [&](std::error_code, std::size_t) {
            service.stop();
            throw handler_error{};
        },
        ioc.get_executor());
    REQUIRE(context->bind_completion(native.queue));
    native.pending.push_back({.op_context = context->as_fi_context()});
    service.register_cq(native.queue, {.poll_interval = 0us, .max_completions_per_poll = 1});
    service.start();
    REQUIRE_THROWS_AS(ioc.run_one(), handler_error);
    REQUIRE_FALSE(service.is_running());
    const auto reads = native.reads;
    ioc.run();
    REQUIRE(native.reads == reads);
    REQUIRE_FALSE(service.deregister_cq(native.queue));
}

TEST_CASE("Asio polling stays stopped after concurrent timer restarts", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    std::atomic<int> polls{};
    native.before_read = [&] {
        ++polls;
    };
    service.register_cq(native.queue, {.poll_interval = 0us});
    auto work = ::asio::make_work_guard(ioc);
    loom::test::joining_thread runner{[&] { ioc.run(); }};
    bool stayed_stopped = true;
    for (int iteration = 0; iteration < 64; ++iteration) {
        service.start();
        std::this_thread::yield();
        service.stop();
        const auto stopped_at = polls.load();
        std::promise<void> drained;
        auto ready = drained.get_future();
        ::asio::post(ioc, [drained = std::move(drained)]() mutable { drained.set_value(); });
        ready.wait();
        stayed_stopped &= polls.load() == stopped_at;
    }
    work.reset();
    runner.join();
    REQUIRE(stayed_stopped);
    REQUIRE_FALSE(service.is_running());
    REQUIRE_FALSE(service.deregister_cq(native.queue));
}

#if defined(ASIO_HAS_POSIX_STREAM_DESCRIPTOR)
TEST_CASE("Asio FD progress sleeps until readiness and rearms after manual reads",
          "[asio][fabric_service][fd]") {
    ::asio::io_context ioc;
    fd_service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(native.queue, {.poll_interval = 0us});
    service.start();
    ioc.run_for(5ms);
    REQUIRE(native.reads == 0);
    REQUIRE(native.wait_calls == 1);
    REQUIRE(service.poll_once() == 0);
    REQUIRE(native.reads == 1);
    REQUIRE(native.wait_calls == 2);
    ioc.run_for(5ms);
    REQUIRE(native.reads == 1);
    int completed{};
    auto callback = [&] {
        if (++completed == 2)
            service.stop();
    };
    fi_context2 context{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(native.queue,
                              loom::context_ptr<void>{&context},
                              &callback,
                              [](void* object, const loom::completion_event&) {
                                  (*static_cast<decltype(callback)*>(object))();
                              }));
    native.pending.push_back({.op_context = &context});
    native.signal();
    ioc.run_for(5ms);
    REQUIRE(completed == 1);
    REQUIRE(native.wait_calls == 3);
    REQUIRE(registration.bind(native.queue,
                              loom::context_ptr<void>{&context},
                              &callback,
                              [](void* object, const loom::completion_event&) {
                                  (*static_cast<decltype(callback)*>(object))();
                              }));
    native.pending.push_back({.op_context = &context});
    native.signal();
    ioc.run_for(100ms);
    service.stop();
    REQUIRE(completed == 2);
    REQUIRE(native.descriptor_queries == 1);
    REQUIRE(native.valid_wait_arguments);
    REQUIRE_FALSE(service.deregister_cq(native.queue));
    REQUIRE(::fcntl(native.descriptors[0], F_GETFD) >= 0);
}

TEST_CASE("Asio FD preparation drains work without waiting for a signal",
          "[asio][fabric_service][fd]") {
    const bool throw_first = GENERATE(false, true);
    ::asio::io_context ioc;
    fd_service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    struct handler_error {};
    int callbacks{};
    auto callback = [&] {
        ++callbacks;
        if (throw_first && callbacks == 1)
            throw handler_error{};
        if (callbacks == 3)
            service.stop();
    };
    std::array<fi_context2, 3> contexts{};
    std::array<loom::completion_registration, 3> registrations;
    for (std::size_t i = 0; i < contexts.size(); ++i)
        REQUIRE(registrations[i].bind(native.queue,
                                      loom::context_ptr<void>{&contexts[i]},
                                      &callback,
                                      [](void* object, const loom::completion_event&) {
                                          (*static_cast<decltype(callback)*>(object))();
                                      }));
    // Work appears during preparation and never signals the descriptor. The
    // provider returns EAGAIN until each bounded progress pass has drained it.
    native.before_wait = [&] {
        if (native.wait_calls == 1)
            for (auto& context : contexts)
                native.pending.push_back({.op_context = &context});
    };
    service.register_cq(native.queue, {.poll_interval = 1h, .max_completions_per_poll = 1});
    service.start();
    if (throw_first) {
        REQUIRE_THROWS_AS(ioc.run_one(), handler_error);
        REQUIRE(callbacks == 1);
    }
    ioc.run_for(100ms);
    service.stop();
    REQUIRE(callbacks == 3);
    REQUIRE(native.reads == 3);
    REQUIRE(native.wait_calls >= 2);
    REQUIRE_FALSE(service.deregister_cq(native.queue));
}

TEST_CASE("Asio FD readiness survives a throwing completion callback",
          "[asio][fabric_service][fd]") {
    ::asio::io_context ioc;
    fd_service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    struct handler_error {};
    int callbacks{};
    auto callback = [&] {
        if (++callbacks == 1)
            throw handler_error{};
        service.stop();
    };
    std::array<fi_context2, 2> contexts{};
    std::array<loom::completion_registration, 2> registrations;
    for (std::size_t i = 0; i < contexts.size(); ++i)
        REQUIRE(registrations[i].bind(native.queue,
                                      loom::context_ptr<void>{&contexts[i]},
                                      &callback,
                                      [](void* object, const loom::completion_event&) {
                                          (*static_cast<decltype(callback)*>(object))();
                                      }));
    service.register_cq(native.queue, {.poll_interval = 1h, .max_completions_per_poll = 1});
    service.start();
    for (auto& context : contexts)
        native.pending.push_back({.op_context = &context});
    native.signal();
    REQUIRE_THROWS_AS(ioc.run_one(), handler_error);
    ioc.run_for(100ms);
    service.stop();
    REQUIRE(callbacks == 2);
    REQUIRE_FALSE(service.deregister_cq(native.queue));
}

TEST_CASE("Asio FD queues pause on unclaimed events and resume when consumed",
          "[asio][fabric_service][fd]") {
    ::asio::io_context ioc;
    fd_service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(native.queue, {.poll_interval = 1h});
    service.start();
    native.pending.push_back({.data = 7});
    native.pending.push_back({.data = 11});
    native.signal();
    ioc.run_for(5ms);
    REQUIRE(native.reads == 1);
    const auto first = service.take_unhandled(native.queue);
    REQUIRE(first);
    REQUIRE(first->data == 7);
    ioc.restart();
    ioc.run_for(5ms);
    REQUIRE(native.reads == 2);
    const auto second = service.take_unhandled(native.queue);
    REQUIRE(second);
    REQUIRE(second->data == 11);
    ioc.restart();
    ioc.run_for(5ms);
    REQUIRE(native.reads == 2);
    service.stop();
    REQUIRE_FALSE(service.deregister_cq(native.queue));
}

TEST_CASE("Asio stale FD callbacks cannot read a replacement queue", "[asio][fabric_service][fd]") {
    ::asio::io_context ioc;
    auto original = std::make_unique<fd_service_queue>();
    fd_service_queue replacement;
    auto queue = std::move(original->queue);
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(queue, {.poll_interval = 1h});
    service.start();
    original->signal();
    service.stop();
    service.start();
    REQUIRE_FALSE(service.deregister_cq(queue));
    queue = {};
    original.reset();
    queue = std::move(replacement.queue);
    service.register_cq(queue, {.poll_interval = 1h});
    ioc.run_for(5ms);
    REQUIRE(replacement.reads == 0);
    replacement.pending.push_back({.data = 17});
    replacement.signal();
    ioc.run_for(5ms);
    service.stop();
    REQUIRE(replacement.reads == 1);
    const auto event = service.deregister_cq(queue);
    REQUIRE(event);
    REQUIRE(event->data == 17);
}

TEST_CASE("Asio falls back to timer progress when FD waits are unavailable",
          "[asio][fabric_service][fd]") {
    enum class fallback { disabled, wait_type, control, parent, trywait, wait_error, descriptor };
    const auto reason = GENERATE(fallback::disabled,
                                 fallback::wait_type,
                                 fallback::control,
                                 fallback::parent,
                                 fallback::trywait,
                                 fallback::wait_error,
                                 fallback::descriptor);
    ::asio::io_context ioc;
    fd_service_queue native;
    switch (reason) {
        case fallback::disabled:
            break;
        case fallback::wait_type:
            native.wait_object = FI_WAIT_MUTEX_COND;
            break;
        case fallback::control:
            native.ops.control = nullptr;
            break;
        case fallback::parent:
            native.implementation->parent_fabric = nullptr;
            break;
        case fallback::trywait:
            native.fabric_ops.trywait = nullptr;
            break;
        case fallback::wait_error:
            native.wait_status = -FI_EIO;
            break;
        case fallback::descriptor:
            ::close(std::exchange(native.descriptors[0], -1));
            break;
    }
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(native.queue,
                        {.poll_interval = 0us, .use_fd_polling = reason != fallback::disabled});
    native.pending.push_back({.data = 23});
    service.start();
    ioc.run_for(5ms);
    service.stop();
    REQUIRE(native.reads == 1);
    const auto event = service.deregister_cq(native.queue);
    REQUIRE(event);
    REQUIRE(event->data == 23);
    if (reason == fallback::disabled)
        REQUIRE(native.descriptor_queries == 0);
}

TEST_CASE("Asio timer progress leaves armed FD queues asleep", "[asio][fabric_service][fd]") {
    const bool fast_timer = GENERATE(false, true);
    ::asio::io_context ioc;
    fd_service_queue waiting;
    service_queue timer;
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(waiting.queue, {.poll_interval = 0us});
    service.register_cq(timer.queue, {.poll_interval = fast_timer ? 0us : 1h});
    timer.before_read = [&] {
        service.stop();
    };
    service.start();
    ioc.run_for(5ms);
    service.stop();
    REQUIRE(timer.reads == (fast_timer ? 1 : 0));
    REQUIRE(waiting.reads == 0);
    REQUIRE_FALSE(service.deregister_cq(waiting.queue));
    REQUIRE_FALSE(service.deregister_cq(timer.queue));
}

TEST_CASE("Asio FD callbacks can retire their queue during nested progress",
          "[asio][fabric_service][fd]") {
    const bool fork_child = GENERATE(false, true);
    ::asio::io_context ioc;
    auto original = std::make_unique<fd_service_queue>();
    fd_service_queue added;
    auto& service = loom::asio::fabric_service::use(ioc);
    bool completed{};
    auto complete = [&] {
        completed = true;
        service.stop();
    };
    auto replace = [&] {
        if (fork_child) {
            ioc.notify_fork(::asio::execution_context::fork_prepare);
            ioc.notify_fork(::asio::execution_context::fork_child);
        } else {
            REQUIRE_FALSE(service.deregister_cq(original->queue));
        }
        original.reset();
        service.register_cq(added.queue, {.poll_interval = 1h});
        service.start();
        REQUIRE(service.poll_once() == 1);
    };
    std::array<fi_context2, 2> contexts{};
    std::array<loom::completion_registration, 2> registrations;
    REQUIRE(registrations[0].bind(original->queue,
                                  loom::context_ptr<void>{&contexts[0]},
                                  &replace,
                                  [](void* object, const loom::completion_event&) {
                                      (*static_cast<decltype(replace)*>(object))();
                                  }));
    REQUIRE(registrations[1].bind(added.queue,
                                  loom::context_ptr<void>{&contexts[1]},
                                  &complete,
                                  [](void* object, const loom::completion_event&) {
                                      (*static_cast<decltype(complete)*>(object))();
                                  }));
    added.pending.push_back({.op_context = &contexts[1]});
    service.register_cq(original->queue, {.poll_interval = 1h});
    service.start();
    original->pending.push_back({.op_context = &contexts[0]});
    original->signal();
    ioc.run_for(100ms);
    service.stop();
    REQUIRE(completed);
    REQUIRE_FALSE(service.deregister_cq(added.queue));
}

TEST_CASE("Asio native wait strategies complete RDM transfers",
          "[asio][fabric_service][.provider]") {
    using namespace loom;
    const auto provider = GENERATE("sockets", "tcp;ofi_rxm");
    INFO(provider);
    fabric_hints hints{};
    hints.provider_name = provider;
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Requested RDM provider unavailable");
    auto fab = fabric::create(*info).value();
    auto dom = domain::create(fab, *info).value();
    auto cq = completion_queue::create(dom, {.wait_obj = true}).value();
    const bool has_descriptor = cq.get_wait_fd().has_value();
    CAPTURE(has_descriptor);
    auto av = address_vector::create(dom).value();
    std::array<endpoint, 2> endpoints;
    std::array<fabric_addr, 2> peers;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto ep = endpoint::create(dom, *info).value();
        REQUIRE(ep.bind_cq(cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
        REQUIRE(ep.bind_av(av));
        REQUIRE(ep.enable());
        peers[i] = fabric_addr{av.insert(ep.get_local_address().value()).value().value};
        endpoints[i] = std::move(ep);
    }
    ::asio::io_context ioc;
    loom::asio::endpoint sender{ioc.get_executor(), std::move(endpoints[0])};
    loom::asio::endpoint receiver{ioc.get_executor(), std::move(endpoints[1])};
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(cq, {.poll_interval = has_descriptor ? 1h : 100us});
    service.start();
    std::array<std::byte, 7> outgoing;
    outgoing.fill(std::byte{0x5a});
    std::array<std::byte, 16> incoming{};
    std::array<int, 2> calls{};
    std::array<std::error_code, 2> errors{};
    std::array<std::size_t, 2> sizes{};
    const auto handler = [&](std::size_t index) {
        return [&, index](std::error_code error, std::size_t bytes) {
            ++calls[index];
            errors[index] = error;
            sizes[index] = bytes;
            if (calls[0] && calls[1])
                service.stop();
        };
    };
    receiver.async_receive(::asio::buffer(incoming), handler(1));
    ::asio::steady_timer retry{ioc};
    std::function<void()> submit_send = [&] {
        sender.async_send_to(
            ::asio::buffer(outgoing), peers[1], [&](std::error_code error, std::size_t bytes) {
                if (error == errc::again) {
                    retry.expires_after(1ms);
                    retry.async_wait([&](std::error_code error) {
                        if (!error)
                            submit_send();
                    });
                    return;
                }
                handler(0)(error, bytes);
            });
    };
    submit_send();
    ioc.run_for(5s);
    service.stop();
    const bool completed = calls[0] && calls[1];
    if (!completed) {
        retry.cancel();
        std::error_code ignored;
        sender.cancel(ignored);
        receiver.cancel(ignored);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            service.poll_once();
            ioc.restart();
            ioc.poll();
            if (calls[0] && calls[1])
                break;
        }
        REQUIRE(sender.native_handle()->close());
        REQUIRE(receiver.native_handle()->close());
    }
    REQUIRE_FALSE(service.deregister_cq(cq));
    REQUIRE(completed);
    REQUIRE(calls == std::array{1, 1});
    REQUIRE_FALSE(errors[0]);
    REQUIRE_FALSE(errors[1]);
    REQUIRE(sizes == std::array{outgoing.size(), outgoing.size()});
    REQUIRE(std::ranges::equal(outgoing, std::span{incoming}.first(outgoing.size())));
}

TEST_CASE("Asio FD polling stays stopped across concurrent restarts",
          "[asio][fabric_service][fd]") {
    ::asio::io_context ioc;
    fd_service_queue native;
    auto& service = loom::asio::fabric_service::use(ioc);
    std::atomic<int> polls{};
    native.before_read = [&] {
        ++polls;
    };
    service.register_cq(native.queue, {.poll_interval = 1h});
    auto work = ::asio::make_work_guard(ioc);
    loom::test::joining_thread runner{[&] { ioc.run(); }};
    bool stayed_stopped = true;
    for (int iteration = 0; iteration < 64; ++iteration) {
        service.start();
        native.signal();
        std::this_thread::yield();
        service.stop();
        const auto stopped_at = polls.load();
        std::promise<void> drained;
        auto ready = drained.get_future();
        ::asio::post(ioc, [drained = std::move(drained)]() mutable { drained.set_value(); });
        ready.wait();
        stayed_stopped &= polls.load() == stopped_at;
    }
    work.reset();
    runner.join();
    REQUIRE(stayed_stopped);
    REQUIRE(native.valid_wait_arguments);
    REQUIRE_FALSE(service.deregister_cq(native.queue));
}
#endif
