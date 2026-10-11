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
        return impl;
    }()};
};
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
            std::array<std::jthread, 8> threads;
            for (std::size_t i = 0; i < threads.size(); ++i) {
                threads[i] = std::jthread{[&, i] {
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
    std::jthread poller{[&] { count = service.poll_once(); }};
    reading.wait();
    std::promise<std::optional<loom::completion_event>> result;
    auto removed = result.get_future();
    std::jthread remover{[&] {
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
