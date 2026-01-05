// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/error.hpp>
#include <loom/core/result.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("error_code creation", "[error]") {
    auto ec = loom::make_error_code(loom::errc::invalid_argument);

    REQUIRE(ec.value() == static_cast<int>(loom::errc::invalid_argument));
    REQUIRE(ec.category() == loom::loom_category());
}

TEST_CASE("error_code message", "[error]") {
    auto ec = loom::make_error_code(loom::errc::invalid_argument);
    auto msg = ec.message();

    REQUIRE_FALSE(msg.empty());
}

TEST_CASE("error_code message for various error codes", "[error]") {
    auto test_error = [](loom::errc e) {
        auto ec = loom::make_error_code(e);
        auto msg = ec.message();
        REQUIRE_FALSE(msg.empty());
    };

    test_error(loom::errc::success);
    test_error(loom::errc::no_data);
    test_error(loom::errc::message_too_long);
    test_error(loom::errc::no_space);
    test_error(loom::errc::again);
    test_error(loom::errc::io_error);
    test_error(loom::errc::not_supported);
    test_error(loom::errc::busy);
    test_error(loom::errc::canceled);
    test_error(loom::errc::no_memory);
    test_error(loom::errc::already);
    test_error(loom::errc::bad_flags);
    test_error(loom::errc::no_entry);
    test_error(loom::errc::not_connected);
    test_error(loom::errc::address_in_use);
    test_error(loom::errc::connection_refused);
    test_error(loom::errc::address_not_available);
    test_error(loom::errc::timeout);
}

TEST_CASE("error_category name", "[error]") {
    const auto& cat = loom::loom_category();
    REQUIRE(std::string_view(cat.name()) == "loom");
}

TEST_CASE("error_category default_error_condition", "[error]") {
    const auto& cat = loom::loom_category();
    auto cond = cat.default_error_condition(static_cast<int>(loom::errc::no_memory));
    REQUIRE(cond.value() == static_cast<int>(loom::errc::no_memory));
}

TEST_CASE("make_error_code_from_fi_errno", "[error]") {
    auto ec = loom::make_error_code_from_fi_errno(-12);
    REQUIRE(ec.value() == -12);
    REQUIRE(ec.category() == loom::loom_category());
    REQUIRE_FALSE(ec.message().empty());
}

TEST_CASE("result success", "[error]") {
    loom::result<int> success_result = loom::make_success(42);

    REQUIRE(static_cast<bool>(success_result));
    REQUIRE(*success_result == 42);
}

TEST_CASE("result error", "[error]") {
    loom::result<int> error_result = loom::make_error_result<int>(loom::errc::no_memory);

    REQUIRE_FALSE(static_cast<bool>(error_result));
    REQUIRE(error_result.error().value() == static_cast<int>(loom::errc::no_memory));
}

TEST_CASE("void_result success", "[error]") {
    loom::void_result void_success = loom::make_success();

    REQUIRE(static_cast<bool>(void_success));
}

TEST_CASE("void_result error", "[error]") {
    loom::void_result void_error = loom::make_error_result<void>(loom::errc::busy);

    REQUIRE_FALSE(static_cast<bool>(void_error));
}
