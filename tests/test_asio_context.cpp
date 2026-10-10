// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_context.cpp
 * @brief Tests for loom::asio::asio_context.
 */

#include <loom/asio.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <asio/bind_allocator.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace std::literals;

TEST_CASE("completion honors the handler executor association", "[asio][context]") {
    ::asio::io_context fallback, associated;
    bool completed{};
    auto handler = ::asio::bind_executor(associated.get_executor(),
                                         [&](std::error_code, std::size_t) { completed = true; });
    auto* ctx = loom::asio::make_asio_context(std::move(handler), fallback.get_executor());
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
    explicit tracking_allocator(int* count) : deallocations(count) {}
    tracking_allocator(const tracking_allocator&) = default;
    tracking_allocator(tracking_allocator&& other) noexcept
        : deallocations(std::exchange(other.deallocations, nullptr)) {}
    template <typename U>
    tracking_allocator(const tracking_allocator<U>& other) : deallocations(other.deallocations) {}
    auto allocate(std::size_t count) -> T* { return std::allocator<T>{}.allocate(count); }
    void deallocate(T* pointer, std::size_t count) noexcept {
        if (deallocations) {
            ++*deallocations;
        }
        std::allocator<T>{}.deallocate(pointer, count);
    }
};
}  // namespace

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

TEST_CASE("dispatch_asio_completion with successful event", "[asio][context]") {
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

    loom::asio::dispatch_asio_completion(ctx->as_fi_context(), event);

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(bytes_received.load(std::memory_order_acquire) == 128);
}

TEST_CASE("dispatch_asio_completion with error event", "[asio][context]") {
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

    loom::asio::dispatch_asio_completion(ctx->as_fi_context(), event);

    ioc.run();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(received_ec == std::make_error_code(std::errc::connection_refused));
}
