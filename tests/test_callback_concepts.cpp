// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/concepts/callback.hpp>

#include <cstddef>
#include <functional>
#include <system_error>

#include "ut.hpp"

using namespace boost::ut;

suite callback_concepts_suite = [] {
    "completion_handler with lambda"_test = [] {
        auto handler = [](std::error_code) {
        };
        static_assert(loom::completion_handler<decltype(handler)>);
    };

    "completion_handler with capturing lambda"_test = [] {
        int captured = 0;
        auto handler = [&captured](std::error_code ec) {
            captured = ec.value();
        };
        static_assert(loom::completion_handler<decltype(handler)>);
    };

    "completion_handler with std::function"_test = [] {
        using handler_type = std::function<void(std::error_code)>;
        static_assert(loom::completion_handler<handler_type>);
    };

    "completion_handler with function pointer"_test = [] {
        using handler_type = void (*)(std::error_code);
        static_assert(loom::completion_handler<handler_type>);
    };

    "completion_handler rejects wrong signature"_test = [] {
        auto wrong_handler = []() {
        };
        static_assert(!loom::completion_handler<decltype(wrong_handler)>);

        auto wrong_args = [](int) {
        };
        static_assert(!loom::completion_handler<decltype(wrong_args)>);
    };

    "result_handler with lambda returning size"_test = [] {
        auto handler = [](std::error_code, std::size_t) {
        };
        static_assert(loom::result_handler<decltype(handler), std::size_t>);
    };

    "result_handler with various result types"_test = [] {
        auto int_handler = [](std::error_code, int) {
        };
        static_assert(loom::result_handler<decltype(int_handler), int>);

        auto double_handler = [](std::error_code, double) {
        };
        static_assert(loom::result_handler<decltype(double_handler), double>);

        auto ptr_handler = [](std::error_code, void*) {
        };
        static_assert(loom::result_handler<decltype(ptr_handler), void*>);
    };

    "result_handler with std::function"_test = [] {
        using handler_type = std::function<void(std::error_code, std::size_t)>;
        static_assert(loom::result_handler<handler_type, std::size_t>);
    };

    "result_handler rejects wrong signature"_test = [] {
        auto wrong_handler = [](std::error_code) {
        };
        static_assert(!loom::result_handler<decltype(wrong_handler), int>);

        auto no_error = [](int) {
        };
        static_assert(!loom::result_handler<decltype(no_error), int>);
    };

    "handler with mutable lambda"_test = [] {
        auto mutable_handler = [counter = 0](std::error_code) mutable {
            ++counter;
        };
        static_assert(loom::completion_handler<decltype(mutable_handler)>);
    };

    "handler callable multiple times"_test = [] {
        int call_count = 0;
        auto handler = [&call_count](std::error_code) {
            ++call_count;
        };

        handler(std::error_code{});
        handler(std::make_error_code(std::errc::invalid_argument));

        expect(call_count == 2) << "handler should be callable multiple times";
    };

    "result_handler receives correct values"_test = [] {
        std::error_code received_ec;
        std::size_t received_size = 0;

        auto handler = [&](std::error_code ec, std::size_t size) {
            received_ec = ec;
            received_size = size;
        };

        handler(std::make_error_code(std::errc::io_error), 42);

        expect(received_ec == std::make_error_code(std::errc::io_error));
        expect(received_size == 42UL);
    };
};

auto main() -> int {}
