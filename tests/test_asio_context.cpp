// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_context.cpp
 * @brief Tests for loom::asio::asio_context.
 */

#include <loom/asio.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>

#include <atomic>
#include <chrono>
#include <list>
#include <thread>

#include <asio/bind_allocator.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace std::literals;

TEST_CASE("completion honors the handler executor association", "[asio][context]") {
    ::asio::io_context fallback, associated;
    bool completed{};
    auto handler = ::asio::bind_executor(associated.get_executor(),
                                         [&](std::error_code, std::size_t) { completed = true; });
    auto* ctx = loom::asio::make_asio_context(std::move(handler), fallback.get_executor());
    associated.poll();
    REQUIRE_FALSE(associated.stopped());
    ctx->complete(loom::completion_event{});
    fallback.poll();
    REQUIRE_FALSE(completed);
    associated.run();
    REQUIRE(completed);
}

TEST_CASE("immediate errors defer handler invocation", "[asio][context]") {
    ::asio::io_context ioc;
    bool completed{};
    ::asio::post(ioc, [&] {
        auto* ctx = loom::asio::make_asio_context(
            [&](std::error_code, std::size_t) { completed = true; }, ioc.get_executor());
        ctx->fail(std::make_error_code(std::errc::bad_file_descriptor));
        REQUIRE_FALSE(completed);
    });
    ioc.run();
    REQUIRE(completed);
}

namespace {
template <typename T>
struct tracking_allocator {
    using value_type = T;
    int* deallocations{};
    int* allocations_remaining{};
    explicit tracking_allocator(int* count, int* remaining = nullptr)
        : deallocations(count), allocations_remaining(remaining) {}
    tracking_allocator(const tracking_allocator&) = default;
    tracking_allocator(tracking_allocator&& other) noexcept
        : deallocations(std::exchange(other.deallocations, nullptr)),
          allocations_remaining(other.allocations_remaining) {}
    template <typename U>
    tracking_allocator(const tracking_allocator<U>& other)
        : deallocations(other.deallocations), allocations_remaining(other.allocations_remaining) {}
    auto allocate(std::size_t count) -> T* {
        if (allocations_remaining && (*allocations_remaining)-- == 0) {
            throw std::bad_alloc{};
        }
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        if (deallocations) {
            ++*deallocations;
        }
        std::allocator<T>{}.deallocate(pointer, count);
    }
};
}  // namespace

TEST_CASE("buffer metadata uses the handler allocator and cleans up on failure",
          "[asio][context]") {
    ::asio::io_context ioc;
    ::asio::cancellation_signal cancellation;
    loom::endpoint ep;
    int deallocations{};
    const auto allowed = GENERATE(1, 2);
    const auto count = GENERATE(5UZ, 17UZ, 64UZ);
    int budget{allowed};
    std::array<std::byte, 8> storage{};
    std::array<::asio::const_buffer, 64> buffers;
    buffers.fill(::asio::buffer(storage));
    auto handler = ::asio::bind_allocator(
        tracking_allocator<std::byte>{&deallocations, &budget},
        ::asio::bind_cancellation_slot(cancellation.slot(), [](std::error_code, std::size_t) {
            FAIL("Initiation did not finish");
        }));
    REQUIRE_THROWS_AS(
        loom::asio::make_buffered_asio_context<false>(handler,
                                                      ioc.get_executor(),
                                                      &ep,
                                                      std::span{buffers}.first(count),
                                                      loom::mr_descriptor{storage.data()}),
        std::bad_alloc);
    REQUIRE(deallocations == allowed);
    REQUIRE_FALSE(cancellation.slot().has_handler());
    REQUIRE(ioc.poll() == 0);
    REQUIRE(ioc.stopped());
}

TEST_CASE("Asio metadata uses inline storage and bounded allocator calls", "[asio][context]") {
    const auto count = GENERATE(0UZ, 1UZ, 2UZ, 4UZ, 5UZ, 16UZ, 17UZ, 64UZ);
    const auto registered = GENERATE(false, true);
    const int expected_allocations = 1 + (count > 4 ? registered ? 2 : 1 : 0);
    int budget = expected_allocations;
    int deallocations{};
    ::asio::io_context ioc;
    loom::endpoint ep;
    std::array<std::byte, 64> storage{};
    std::array<::asio::const_buffer, 64> buffers;
    for (std::size_t i = 0; i < buffers.size(); ++i)
        buffers[i] = {storage.data() + i, 1};
    auto handler = ::asio::bind_allocator(
        tracking_allocator<std::byte>{&deallocations, &budget},
        [](std::error_code, std::size_t) { FAIL("Unsubmitted context invoked its handler"); });
    auto context = loom::asio::make_buffered_asio_context<false>(
        handler,
        ioc.get_executor(),
        &ep,
        std::span{buffers}.first(count),
        registered ? std::optional{loom::mr_descriptor{storage.data()}} : std::nullopt);
    REQUIRE(budget == 0);
    const auto prepared = context->buffers();
    REQUIRE(prepared.iov.size() == count);
    REQUIRE(prepared.descriptors.size() == (registered ? count : 0));
    REQUIRE(prepared.bytes == count);
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(prepared.iov[i].iov_base == storage.data() + i);
        CHECK(prepared.iov[i].iov_len == 1);
        if (registered)
            CHECK(prepared.descriptors[i].raw() == storage.data());
    }
    context.reset();
    REQUIRE(deallocations == expected_allocations);
    REQUIRE(ioc.poll() == 0);
    REQUIRE(ioc.stopped());
}

TEST_CASE("Asio metadata preserves appended buffers across inline capacity", "[asio][context]") {
    const auto initial = GENERATE(1UZ, 4UZ, 5UZ);
    std::array<std::byte, 9> storage{};
    std::list<::asio::const_buffer> first;
    for (std::size_t i = 0; i < initial; ++i)
        first.emplace_back(storage.data() + i, 1);
    loom::asio::detail::asio_buffer_storage metadata;
    metadata.prepare_buffers<false>(first, std::nullopt);
    const std::array registered{::asio::const_buffer{storage.data() + initial, 1},
                                ::asio::const_buffer{storage.data() + initial + 1, 1}};
    metadata.prepare_buffers<false>(registered, loom::mr_descriptor{storage.data()});
    const std::array last{::asio::const_buffer{storage.data() + initial + 2, 1},
                          ::asio::const_buffer{storage.data() + initial + 3, 1}};
    metadata.prepare_buffers<false>(last, std::nullopt);
    const auto buffers = metadata.buffers();
    REQUIRE(buffers.bytes == initial + 4);
    REQUIRE(buffers.iov.size() == initial + 4);
    REQUIRE(buffers.descriptors.size() == initial + 4);
    for (std::size_t i = 0; i < initial + 4; ++i) {
        CHECK(buffers.iov[i].iov_base == storage.data() + i);
        CHECK(buffers.iov[i].iov_len == 1);
        CHECK(buffers.descriptors[i].raw() ==
              (i == initial || i == initial + 1 ? storage.data() : nullptr));
    }
}

TEST_CASE("completion preserves allocator state and clears cancellation", "[asio][context]") {
    ::asio::io_context ioc;
    ::asio::cancellation_signal cancellation;
    loom::endpoint ep;
    int deallocations{};
    int completions{};
    auto handler = ::asio::bind_allocator(
        tracking_allocator<std::byte>{&deallocations},
        ::asio::bind_cancellation_slot(cancellation.slot(),
                                       [&](std::error_code, std::size_t) { ++completions; }));
    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor(), &ep);
    REQUIRE(cancellation.slot().has_handler());
    SECTION("success") {
        ctx->complete(loom::completion_event{});
    }
    SECTION("error") {
        ctx->fail(std::make_error_code(std::errc::io_error));
    }
    SECTION("stopped") {
        ctx->cancel();
    }
    REQUIRE_FALSE(cancellation.slot().has_handler());
    REQUIRE(deallocations == 1);
    cancellation.emit(::asio::cancellation_type::terminal);
    ioc.run();
    REQUIRE(completions == 1);
}

TEST_CASE("asio_context_erased from_fi_context nullptr", "[asio][context]") {
    auto* result = loom::asio::asio_context_erased::from_fi_context(nullptr);
    REQUIRE(result == nullptr);
}

TEST_CASE("asio_context creation and handler invocation", "[asio][context]") {
    ::asio::io_context ioc;
    std::atomic<bool> handler_called{false};
    std::atomic<std::size_t> bytes_received{0};
    std::error_code received_ec;

    auto handler = [&](std::error_code ec, std::size_t n) {
        received_ec = ec;
        bytes_received.store(n, std::memory_order_release);
        handler_called.store(true, std::memory_order_release);
    };

    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor());
    REQUIRE(ctx != nullptr);

    loom::completion_event event;
    event.bytes_transferred = 42;
    event.error = {};

    ctx->complete(event);

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(bytes_received.load(std::memory_order_acquire) == 42);
    REQUIRE(!received_ec);
}

TEST_CASE("Asio preserves partial receive lengths on failure", "[asio][context]") {
    ::asio::io_context ioc;
    bool completed{};
    auto* ctx = loom::asio::make_asio_context(
        [&](std::error_code error, std::size_t bytes) {
            REQUIRE(error == loom::make_error_code_from_fi_errno(FI_ETRUNC));
            REQUIRE(bytes == 3);
            completed = true;
        },
        ioc.get_executor());
    loom::completion_event event;
    event.bytes_transferred = 3;
    event.error = loom::make_error_code_from_fi_errno(FI_ETRUNC);
    ctx->complete(event);
    ioc.run();
    REQUIRE(completed);
}

TEST_CASE("Asio transfer failure does not report a successful send", "[asio][context]") {
    ::asio::io_context ioc;
    bool completed{};
    auto* ctx = loom::asio::make_asio_context(
        [&](std::error_code error, std::size_t bytes) {
            REQUIRE(error == loom::errc::io_error);
            REQUIRE(bytes == 0);
            completed = true;
        },
        ioc.get_executor());
    ctx->set_transfer_size(42);
    loom::completion_event event;
    event.error = loom::make_error_code(loom::errc::io_error);
    ctx->complete(event);
    ioc.run();
    REQUIRE(completed);
}

TEST_CASE("asio_context failure handler", "[asio][context]") {
    ::asio::io_context ioc;
    std::atomic<bool> handler_called{false};
    std::error_code received_ec;

    auto handler = [&](std::error_code ec, std::size_t) {
        received_ec = ec;
        handler_called.store(true, std::memory_order_release);
    };

    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor());
    REQUIRE(ctx != nullptr);

    auto test_ec = std::make_error_code(std::errc::io_error);
    ctx->fail(test_ec);

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(received_ec == test_ec);
}

TEST_CASE("asio_context cancel handler", "[asio][context]") {
    ::asio::io_context ioc;
    std::atomic<bool> handler_called{false};
    std::error_code received_ec;

    auto handler = [&](std::error_code ec, std::size_t) {
        received_ec = ec;
        handler_called.store(true, std::memory_order_release);
    };

    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor());
    REQUIRE(ctx != nullptr);

    ctx->cancel();

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(received_ec == std::make_error_code(std::errc::operation_canceled));
}

TEST_CASE("asio_context fi_context round trip", "[asio][context]") {
    ::asio::io_context ioc;

    auto handler = [](std::error_code, std::size_t) {
    };
    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor());
    REQUIRE(ctx != nullptr);

    void* fi_ctx = ctx->as_fi_context();
    REQUIRE(fi_ctx != nullptr);

    auto* recovered = loom::asio::asio_context_erased::from_fi_context(fi_ctx);
    REQUIRE(recovered == ctx);

    ctx->cancel();
    ioc.run();
}

TEST_CASE("Asio context completes with a successful event", "[asio][context]") {
    ::asio::io_context ioc;
    std::atomic<bool> handler_called{false};
    std::atomic<std::size_t> bytes_received{0};

    auto handler = [&](std::error_code ec, std::size_t n) {
        if (!ec) {
            bytes_received.store(n, std::memory_order_release);
        }
        handler_called.store(true, std::memory_order_release);
    };

    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor());

    loom::completion_event event;
    event.bytes_transferred = 128;
    event.error = {};
    event.context = loom::context_ptr<void>{ctx->as_fi_context()};

    ctx->complete(event);

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(bytes_received.load(std::memory_order_acquire) == 128);
}

TEST_CASE("Asio context completes with an error event", "[asio][context]") {
    ::asio::io_context ioc;
    std::atomic<bool> handler_called{false};
    std::error_code received_ec;

    auto handler = [&](std::error_code ec, std::size_t) {
        received_ec = ec;
        handler_called.store(true, std::memory_order_release);
    };

    auto* ctx = loom::asio::make_asio_context(std::move(handler), ioc.get_executor());

    loom::completion_event event;
    event.bytes_transferred = 0;
    event.error = std::make_error_code(std::errc::connection_refused);
    event.context = loom::context_ptr<void>{ctx->as_fi_context()};

    ctx->complete(event);

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(received_ec == std::make_error_code(std::errc::connection_refused));
}

TEST_CASE("registered Asio contexts detach before inline handlers throw", "[asio][context]") {
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fid_cq handle{.fid = {.ops = &ops}};
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &handle)};
    ::asio::io_context ioc;
    struct handler_error {};
    int completions{};
    auto* ctx = loom::asio::make_asio_context(
        [&](std::error_code error, std::size_t bytes) {
            REQUIRE_FALSE(error);
            REQUIRE(bytes == 7);
            ++completions;
            throw handler_error{};
        },
        ioc.get_executor());
    REQUIRE(ctx->bind_completion(queue));
    loom::completion_event event;
    event.context = loom::context_ptr<void>{ctx->as_fi_context()};
    event.bytes_transferred = 7;
    ::asio::post(ioc, [&] { REQUIRE(queue.dispatch(event)); });
    REQUIRE_THROWS_AS(ioc.run(), handler_error);
    REQUIRE(completions == 1);
    REQUIRE_FALSE(queue.dispatch(event));
    REQUIRE(ioc.run() == 0);
}
