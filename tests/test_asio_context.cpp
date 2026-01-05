// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_context.cpp
 * @brief Tests for loom::asio::asio_context.
 */

#include <loom/asio.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <asio/io_context.hpp>

using namespace std::literals;

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
