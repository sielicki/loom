// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/endpoint_types.hpp>

#include <variant>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("endpoint_tag concept", "[endpoint_types]") {
    static_assert(loom::endpoint_tag<loom::msg_endpoint_tag>);
    static_assert(loom::endpoint_tag<loom::rdm_endpoint_tag>);
    static_assert(loom::endpoint_tag<loom::dgram_endpoint_tag>);

    static_assert(!loom::endpoint_tag<int>);
    static_assert(!loom::endpoint_tag<void>);
}

TEST_CASE("endpoint_type variant holds tags", "[endpoint_types]") {
    loom::endpoint_type msg = loom::endpoint_types::msg;
    loom::endpoint_type rdm = loom::endpoint_types::rdm;
    loom::endpoint_type dgram = loom::endpoint_types::dgram;

    REQUIRE(std::holds_alternative<loom::msg_endpoint_tag>(msg));
    REQUIRE(std::holds_alternative<loom::rdm_endpoint_tag>(rdm));
    REQUIRE(std::holds_alternative<loom::dgram_endpoint_tag>(dgram));
}

TEST_CASE("msg_endpoint_tag properties", "[endpoint_types]") {
    using props = loom::endpoint_properties<loom::msg_endpoint_tag>;

    static_assert(props::is_reliable);
    static_assert(props::is_connection_oriented);
    static_assert(props::supports_rma);
}

TEST_CASE("rdm_endpoint_tag properties", "[endpoint_types]") {
    using props = loom::endpoint_properties<loom::rdm_endpoint_tag>;

    static_assert(props::is_reliable);
    static_assert(!props::is_connection_oriented);
    static_assert(props::supports_rma);
}

TEST_CASE("dgram_endpoint_tag properties", "[endpoint_types]") {
    using props = loom::endpoint_properties<loom::dgram_endpoint_tag>;

    static_assert(!props::is_reliable);
    static_assert(!props::is_connection_oriented);
    static_assert(!props::supports_rma);
}

TEST_CASE("is_reliable function", "[endpoint_types]") {
    REQUIRE(loom::is_reliable(loom::endpoint_types::msg));
    REQUIRE(loom::is_reliable(loom::endpoint_types::rdm));
    REQUIRE_FALSE(loom::is_reliable(loom::endpoint_types::dgram));
}

TEST_CASE("is_reliable constexpr", "[endpoint_types]") {
    constexpr bool msg_reliable = loom::is_reliable(loom::endpoint_types::msg);
    constexpr bool rdm_reliable = loom::is_reliable(loom::endpoint_types::rdm);
    constexpr bool dgram_reliable = loom::is_reliable(loom::endpoint_types::dgram);

    static_assert(msg_reliable);
    static_assert(rdm_reliable);
    static_assert(!dgram_reliable);
}

TEST_CASE("is_connection_oriented function", "[endpoint_types]") {
    REQUIRE(loom::is_connection_oriented(loom::endpoint_types::msg));
    REQUIRE_FALSE(loom::is_connection_oriented(loom::endpoint_types::rdm));
    REQUIRE_FALSE(loom::is_connection_oriented(loom::endpoint_types::dgram));
}

TEST_CASE("is_connection_oriented constexpr", "[endpoint_types]") {
    constexpr bool msg_conn = loom::is_connection_oriented(loom::endpoint_types::msg);
    constexpr bool rdm_conn = loom::is_connection_oriented(loom::endpoint_types::rdm);
    constexpr bool dgram_conn = loom::is_connection_oriented(loom::endpoint_types::dgram);

    static_assert(msg_conn);
    static_assert(!rdm_conn);
    static_assert(!dgram_conn);
}

TEST_CASE("supports_rma function", "[endpoint_types]") {
    REQUIRE(loom::supports_rma(loom::endpoint_types::msg));
    REQUIRE(loom::supports_rma(loom::endpoint_types::rdm));
    REQUIRE_FALSE(loom::supports_rma(loom::endpoint_types::dgram));
}

TEST_CASE("supports_rma constexpr", "[endpoint_types]") {
    constexpr bool msg_rma = loom::supports_rma(loom::endpoint_types::msg);
    constexpr bool rdm_rma = loom::supports_rma(loom::endpoint_types::rdm);
    constexpr bool dgram_rma = loom::supports_rma(loom::endpoint_types::dgram);

    static_assert(msg_rma);
    static_assert(rdm_rma);
    static_assert(!dgram_rma);
}

TEST_CASE("type_name function", "[endpoint_types]") {
    REQUIRE(std::string_view{loom::type_name(loom::endpoint_types::msg)} == "message");
    REQUIRE(std::string_view{loom::type_name(loom::endpoint_types::rdm)} == "reliable_datagram");
    REQUIRE(std::string_view{loom::type_name(loom::endpoint_types::dgram)} == "datagram");
}

TEST_CASE("type_name constexpr", "[endpoint_types]") {
    constexpr const char* msg_name = loom::type_name(loom::endpoint_types::msg);
    constexpr const char* rdm_name = loom::type_name(loom::endpoint_types::rdm);
    constexpr const char* dgram_name = loom::type_name(loom::endpoint_types::dgram);

    REQUIRE(std::string_view{msg_name} == "message");
    REQUIRE(std::string_view{rdm_name} == "reliable_datagram");
    REQUIRE(std::string_view{dgram_name} == "datagram");
}

TEST_CASE("endpoint_tag static name", "[endpoint_types]") {
    static_assert(std::string_view{loom::msg_endpoint_tag::name} == "message");
    static_assert(std::string_view{loom::rdm_endpoint_tag::name} == "reliable_datagram");
    static_assert(std::string_view{loom::dgram_endpoint_tag::name} == "datagram");
}

TEST_CASE("endpoint_type comparison", "[endpoint_types]") {
    loom::endpoint_type msg1 = loom::endpoint_types::msg;
    loom::endpoint_type msg2 = loom::endpoint_types::msg;
    loom::endpoint_type rdm = loom::endpoint_types::rdm;

    REQUIRE(msg1 == msg2);
    REQUIRE(msg1 != rdm);
}

TEST_CASE("constexpr endpoint_types", "[endpoint_types]") {
    constexpr auto msg = loom::endpoint_types::msg;
    constexpr auto rdm = loom::endpoint_types::rdm;
    constexpr auto dgram = loom::endpoint_types::dgram;

    static_assert(std::holds_alternative<loom::msg_endpoint_tag>(msg));
    static_assert(std::holds_alternative<loom::rdm_endpoint_tag>(rdm));
    static_assert(std::holds_alternative<loom::dgram_endpoint_tag>(dgram));
}
