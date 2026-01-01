// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/submission_context.hpp>

#include <coroutine>
#include <future>
#include <thread>

#include "ut.hpp"

using namespace boost::ut;

suite submission_context_suite = [] {
    "callback_receiver satisfies full_receiver"_test = [] {
        static_assert(loom::full_receiver<loom::callback_receiver>);
    };

    "coroutine_receiver satisfies full_receiver"_test = [] {
        static_assert(loom::full_receiver<loom::coroutine_receiver>);
    };

    "promise_receiver satisfies full_receiver"_test = [] {
        static_assert(loom::full_receiver<loom::promise_receiver>);
    };

    "submission_context with callback_receiver"_test = [] {
        bool called = false;
        std::size_t received_bytes = 0;

        auto* ctx = loom::make_callback_context([&](loom::completion_event& event) {
            called = true;
            received_bytes = event.bytes_transferred;
        });

        loom::completion_event event{};
        event.bytes_transferred = 42;

        loom::dispatch_completion(ctx, event);

        expect(called) << "callback should have been invoked";
        expect(received_bytes == 42UL) << "should receive correct bytes";

        delete ctx;
    };

    "submission_context with promise_receiver"_test = [] {
        auto [ctx, future] = loom::make_promise_context();

        std::thread worker([ctx] {
            loom::completion_event event{};
            event.bytes_transferred = 100;
            loom::dispatch_completion(ctx, event);
            delete ctx;
        });

        auto result = future.get();
        expect(result.bytes_transferred == 100UL);

        worker.join();
    };

    "submission_context set_error path"_test = [] {
        bool error_called = false;
        std::error_code received_ec;

        auto* ctx = new loom::default_submission_context(
            loom::callback_receiver([](loom::completion_event&) {},
                                    [&](std::error_code ec) {
                                        error_called = true;
                                        received_ec = ec;
                                    }));

        loom::completion_event event{};
        event.error = std::make_error_code(std::errc::io_error);

        loom::dispatch_completion(ctx, event);

        expect(error_called) << "error handler should have been invoked";
        expect(received_ec == std::make_error_code(std::errc::io_error));

        delete ctx;
    };

    "submission_context set_stopped path"_test = [] {
        bool stopped_called = false;

        auto* ctx = new loom::default_submission_context(
            loom::callback_receiver([](loom::completion_event&) {},
                                    [](std::error_code) {},
                                    [&] { stopped_called = true; }));

        loom::dispatch_stopped(ctx);

        expect(stopped_called) << "stopped handler should have been invoked";

        delete ctx;
    };

    "as_context returns valid pointer"_test = [] {
        auto* ctx = loom::make_callback_context([](loom::completion_event&) {});

        auto* raw = ctx->as_context();
        expect(raw != nullptr);

        auto* recovered = loom::default_submission_context::from_context(raw);
        expect(recovered == ctx);

        delete ctx;
    };

    "handler_index returns correct variant index"_test = [] {
        auto* callback_ctx = loom::make_callback_context([](loom::completion_event&) {});
        expect(callback_ctx->handler_index() == 0UL);
        delete callback_ctx;

        loom::completion_event result{};
        auto* coro_ctx = loom::make_coroutine_context(std::noop_coroutine(), &result);
        expect(coro_ctx->handler_index() == 1UL);
        delete coro_ctx;

        auto [promise_ctx, future] = loom::make_promise_context();
        expect(promise_ctx->handler_index() == 2UL);
        delete promise_ctx;
    };

    "custom receiver type"_test = [] {
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
        expect(counter == 1);

        ctx->set_error(std::error_code{});
        expect(counter == 11);

        ctx->set_stopped();
        expect(counter == 111);

        delete ctx;
    };

    "multiple receiver types in variant"_test = [] {
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
        expect(value == 1);
        delete ctx_a;

        auto* ctx_b = new multi_context(receiver_b{&value});
        ctx_b->set_value(event);
        expect(value == 2);
        delete ctx_b;
    };

    "promise_receiver throws on error"_test = [] {
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
            expect(e.code() == std::make_error_code(std::errc::io_error));
        }

        expect(caught_exception) << "should throw system_error on error path";

        worker.join();
    };

    "promise_receiver throws on stopped"_test = [] {
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
            expect(e.code() == std::make_error_code(std::errc::operation_canceled));
        }

        expect(caught_exception) << "should throw system_error on stopped path";

        worker.join();
    };
};

auto main() -> int {}
