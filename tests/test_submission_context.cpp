// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/submission_context.hpp>

#include <coroutine>
#include <future>
#include <thread>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("callback_receiver satisfies full_receiver", "[submission_context]") {
    static_assert(loom::full_receiver<loom::callback_receiver>);
}

TEST_CASE("coroutine_receiver satisfies full_receiver", "[submission_context]") {
    static_assert(loom::full_receiver<loom::coroutine_receiver>);
}

TEST_CASE("promise_receiver satisfies full_receiver", "[submission_context]") {
    static_assert(loom::full_receiver<loom::promise_receiver>);
}

TEST_CASE("submission_context with callback_receiver", "[submission_context]") {
    bool called = false;
    std::size_t received_bytes = 0;

    auto* ctx = loom::make_callback_context([&](loom::completion_event& event) {
        called = true;
        received_bytes = event.bytes_transferred;
    });

    loom::completion_event event{};
    event.bytes_transferred = 42;

    loom::dispatch_completion(ctx, event);

    REQUIRE(called);
    REQUIRE(received_bytes == 42UL);

    delete ctx;
}

TEST_CASE("submission_context with promise_receiver", "[submission_context]") {
    auto [ctx, future] = loom::make_promise_context();

    std::thread worker([ctx] {
        loom::completion_event event{};
        event.bytes_transferred = 100;
        loom::dispatch_completion(ctx, event);
        delete ctx;
    });

    auto result = future.get();
    REQUIRE(result.bytes_transferred == 100UL);

    worker.join();
}

TEST_CASE("submission_context set_error path", "[submission_context]") {
    bool error_called = false;
    std::error_code received_ec;

    auto* ctx =
        new loom::default_submission_context(loom::callback_receiver([](loom::completion_event&) {},
                                                                     [&](std::error_code ec) {
                                                                         error_called = true;
                                                                         received_ec = ec;
                                                                     }));

    loom::completion_event event{};
    event.error = std::make_error_code(std::errc::io_error);

    loom::dispatch_completion(ctx, event);

    REQUIRE(error_called);
    REQUIRE(received_ec == std::make_error_code(std::errc::io_error));

    delete ctx;
}

TEST_CASE("submission_context set_stopped path", "[submission_context]") {
    bool stopped_called = false;

    auto* ctx = new loom::default_submission_context(loom::callback_receiver(
        [](loom::completion_event&) {}, [](std::error_code) {}, [&] { stopped_called = true; }));

    loom::dispatch_stopped(ctx);

    REQUIRE(stopped_called);

    delete ctx;
}

TEST_CASE("as_context returns valid pointer", "[submission_context]") {
    auto* ctx = loom::make_callback_context([](loom::completion_event&) {});

    auto* raw = ctx->as_context();
    REQUIRE(raw != nullptr);

    auto* recovered = loom::default_submission_context::from_context(raw);
    REQUIRE(recovered == ctx);

    delete ctx;
}

TEST_CASE("handler_index returns correct variant index", "[submission_context]") {
    auto* callback_ctx = loom::make_callback_context([](loom::completion_event&) {});
    REQUIRE(callback_ctx->handler_index() == 0UL);
    delete callback_ctx;

    loom::completion_event result{};
    auto* coro_ctx = loom::make_coroutine_context(std::noop_coroutine(), &result);
    REQUIRE(coro_ctx->handler_index() == 1UL);
    delete coro_ctx;

    auto [promise_ctx, future] = loom::make_promise_context();
    REQUIRE(promise_ctx->handler_index() == 2UL);
    delete promise_ctx;
}

TEST_CASE("custom receiver type", "[submission_context]") {
    struct custom_receiver {
        int* counter;

        void set_value(loom::completion_event&) { ++(*counter); }
        void set_error(std::error_code) { *counter += 10; }
        void set_stopped() { *counter += 100; }
    };

    static_assert(loom::full_receiver<custom_receiver>);

    using custom_context = loom::submission_context<custom_receiver>;

    int counter = 0;
    auto* ctx = new custom_context(custom_receiver{&counter});

    loom::completion_event event{};
    ctx->set_value(event);
    REQUIRE(counter == 1);

    ctx->set_error(std::error_code{});
    REQUIRE(counter == 11);

    ctx->set_stopped();
    REQUIRE(counter == 111);

    delete ctx;
}

TEST_CASE("multiple receiver types in variant", "[submission_context]") {
    struct receiver_a {
        int* value;
        void set_value(loom::completion_event&) { *value = 1; }
        void set_error(std::error_code) { *value = -1; }
        void set_stopped() { *value = 0; }
    };

    struct receiver_b {
        int* value;
        void set_value(loom::completion_event&) { *value = 2; }
        void set_error(std::error_code) { *value = -2; }
        void set_stopped() { *value = 0; }
    };

    static_assert(loom::full_receiver<receiver_a>);
    static_assert(loom::full_receiver<receiver_b>);

    using multi_context = loom::submission_context<receiver_a, receiver_b>;

    int value = 0;

    auto* ctx_a = new multi_context(receiver_a{&value});
    loom::completion_event event{};
    ctx_a->set_value(event);
    REQUIRE(value == 1);
    delete ctx_a;

    auto* ctx_b = new multi_context(receiver_b{&value});
    ctx_b->set_value(event);
    REQUIRE(value == 2);
    delete ctx_b;
}

TEST_CASE("promise_receiver throws on error", "[submission_context]") {
    auto [ctx, future] = loom::make_promise_context();

    std::thread worker([ctx] {
        ctx->set_error(std::make_error_code(std::errc::io_error));
        delete ctx;
    });

    bool caught_exception = false;
    try {
        future.get();
    } catch (const std::system_error& e) {
        caught_exception = true;
        REQUIRE(e.code() == std::make_error_code(std::errc::io_error));
    }

    REQUIRE(caught_exception);

    worker.join();
}

TEST_CASE("promise_receiver throws on stopped", "[submission_context]") {
    auto [ctx, future] = loom::make_promise_context();

    std::thread worker([ctx] {
        ctx->set_stopped();
        delete ctx;
    });

    bool caught_exception = false;
    try {
        future.get();
    } catch (const std::system_error& e) {
        caught_exception = true;
        REQUIRE(e.code() == std::make_error_code(std::errc::operation_canceled));
    }

    REQUIRE(caught_exception);

    worker.join();
}
