// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/error.hpp>
#include <loom/core/result.hpp>

#include "ut.hpp"

using namespace boost::ut;

suite error_suite = [] {
    "error_code creation"_test = [] {
        auto ec = loom::make_error_code(loom::errc::invalid_argument);

        expect(ec.value() == static_cast<int>(loom::errc::invalid_argument))
            << "error code value mismatch";
        expect(ec.category() == loom::loom_category()) << "error code category mismatch";
    };

    "error_code message"_test = [] {
        auto ec = loom::make_error_code(loom::errc::invalid_argument);
        auto msg = ec.message();

        expect(!msg.empty()) << "error message should not be empty";
    };

    "result success"_test = [] {
        loom::result<int> success_result = loom::make_success(42);

        expect(static_cast<bool>(success_result)) << "success result should be truthy";
        expect(*success_result == 42) << "success result value mismatch";
    };

    "result error"_test = [] {
        loom::result<int> error_result = loom::make_error_result<int>(loom::errc::no_memory);

        expect(!static_cast<bool>(error_result)) << "error result should be falsy";
        expect(error_result.error().value() == static_cast<int>(loom::errc::no_memory))
            << "error result error code mismatch";
    };

    "void_result success"_test = [] {
        loom::void_result void_success = loom::make_success();

        expect(static_cast<bool>(void_success)) << "void success result should be truthy";
    };

    "void_result error"_test = [] {
        loom::void_result void_error = loom::make_error_result<void>(loom::errc::busy);

        expect(!static_cast<bool>(void_error)) << "void error result should be falsy";
    };
};

auto main() -> int {}
