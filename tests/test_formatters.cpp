// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <format>
#include <string>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("format strong_type fabric_version", "[formatters]") {
    loom::fabric_version ver{0x12345678U};
    auto formatted = std::format("{}", ver);
    REQUIRE(formatted == "305419896");
}

TEST_CASE("format strong_type caps", "[formatters]") {
    loom::caps c{0x100ULL};
    auto formatted = std::format("{}", c);
    REQUIRE(formatted == "256");
}

TEST_CASE("format context_ptr", "[formatters]") {
    int value = 42;
    loom::context_ptr<int> ptr{&value};
    auto formatted = std::format("{}", ptr);
    REQUIRE_FALSE(formatted.empty());
}

TEST_CASE("format context_ptr nullptr", "[formatters]") {
    loom::context_ptr<void> null_ptr{nullptr};
    auto formatted = std::format("{}", null_ptr);
    REQUIRE(formatted.find('0') != std::string::npos);
}

TEST_CASE("format address_format enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::address_format::unspecified) == "unspecified");
    REQUIRE(std::format("{}", loom::address_format::inet) == "inet");
    REQUIRE(std::format("{}", loom::address_format::inet6) == "inet6");
    REQUIRE(std::format("{}", loom::address_format::ib) == "ib");
    REQUIRE(std::format("{}", loom::address_format::ethernet) == "ethernet");
}

TEST_CASE("format atomic_op enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::atomic_op::min) == "min");
    REQUIRE(std::format("{}", loom::atomic_op::max) == "max");
    REQUIRE(std::format("{}", loom::atomic_op::sum) == "sum");
    REQUIRE(std::format("{}", loom::atomic_op::cswap) == "cswap");
}

TEST_CASE("format atomic_datatype enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::atomic_datatype::int8) == "int8");
    REQUIRE(std::format("{}", loom::atomic_datatype::uint64) == "uint64");
    REQUIRE(std::format("{}", loom::atomic_datatype::float32) == "float32");
    REQUIRE(std::format("{}", loom::atomic_datatype::float64) == "float64");
}

TEST_CASE("format threading_mode enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::threading_mode::unspecified) == "unspecified");
    REQUIRE(std::format("{}", loom::threading_mode::safe) == "safe");
    REQUIRE(std::format("{}", loom::threading_mode::domain) == "domain");
}

TEST_CASE("format progress_mode enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::progress_mode::unspecified) == "unspecified");
    REQUIRE(std::format("{}", loom::progress_mode::auto_progress) == "auto");
    REQUIRE(std::format("{}", loom::progress_mode::manual) == "manual");
}

TEST_CASE("format errc enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::errc::success) == "success");
    REQUIRE(std::format("{}", loom::errc::again) == "again");
    REQUIRE(std::format("{}", loom::errc::busy) == "busy");
    REQUIRE(std::format("{}", loom::errc::timeout) == "timeout");
    REQUIRE(std::format("{}", loom::errc::not_supported) == "not_supported");
}

TEST_CASE("format hmem_iface enum", "[formatters]") {
    REQUIRE(std::format("{}", loom::hmem_iface::system) == "system");
    REQUIRE(std::format("{}", loom::hmem_iface::cuda) == "cuda");
    REQUIRE(std::format("{}", loom::hmem_iface::rocr) == "rocr");
    REQUIRE(std::format("{}", loom::hmem_iface::level_zero) == "level_zero");
}

TEST_CASE("format ipv4_address", "[formatters]") {
    loom::ipv4_address addr{
        std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted == "192.168.1.1");
}

TEST_CASE("format ipv4_address with port", "[formatters]") {
    loom::ipv4_address addr{
        std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 8080};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted == "10.0.0.1:8080");
}

TEST_CASE("format ipv6_address", "[formatters]") {
    std::array<std::uint16_t, 8> segs = {0, 0, 0, 0, 0, 0, 0, 1};
    loom::ipv6_address addr{std::span{segs}};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted == "0:0:0:0:0:0:0:1");
}

TEST_CASE("format ethernet_address", "[formatters]") {
    std::array<std::uint8_t, 6> mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    loom::ethernet_address addr{std::span{mac}};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted == "aa:bb:cc:dd:ee:ff");
}

TEST_CASE("format unspecified_address", "[formatters]") {
    loom::unspecified_address addr{};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted == "<unspecified>");
}

TEST_CASE("format address variant ipv4", "[formatters]") {
    loom::address addr = loom::ipv4_address{
        std::uint8_t{127}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted == "127.0.0.1");
}

TEST_CASE("format remote_memory", "[formatters]") {
    loom::remote_memory rm{0x1000, 0xABCD, 4096};
    auto formatted = std::format("{}", rm);
    REQUIRE(formatted.find("0x1000") != std::string::npos);
    REQUIRE(formatted.find("0xabcd") != std::string::npos);
    REQUIRE(formatted.find("4096") != std::string::npos);
}

TEST_CASE("format endpoint_type", "[formatters]") {
    auto formatted = std::format("{}", loom::endpoint_types::msg);
    REQUIRE(formatted == "message");

    formatted = std::format("{}", loom::endpoint_types::rdm);
    REQUIRE(formatted == "reliable_datagram");

    formatted = std::format("{}", loom::endpoint_types::dgram);
    REQUIRE(formatted == "datagram");
}

TEST_CASE("format endpoint tags", "[formatters]") {
    REQUIRE(std::format("{}", loom::msg_endpoint_tag{}) == "message");
    REQUIRE(std::format("{}", loom::rdm_endpoint_tag{}) == "reliable_datagram");
    REQUIRE(std::format("{}", loom::dgram_endpoint_tag{}) == "datagram");
}

TEST_CASE("format result success", "[formatters]") {
    loom::result<int> res{42};
    auto formatted = std::format("{}", res);
    REQUIRE(formatted.find("ok") != std::string::npos);
    REQUIRE(formatted.find("42") != std::string::npos);
}

TEST_CASE("format result error", "[formatters]") {
    loom::result<int> res = std::unexpected(loom::make_error_code(loom::errc::busy));
    auto formatted = std::format("{}", res);
    REQUIRE(formatted.find("error") != std::string::npos);
}

TEST_CASE("format void_result success", "[formatters]") {
    loom::void_result res{};
    auto formatted = std::format("{}", res);
    REQUIRE(formatted == "ok");
}

TEST_CASE("format std::error_code", "[formatters]") {
    auto ec = std::make_error_code(std::errc::invalid_argument);
    auto formatted = std::format("{}", ec);
    REQUIRE_FALSE(formatted.empty());
}

TEST_CASE("format ib_address", "[formatters]") {
    std::array<std::uint8_t, 16> gid = {0xFE,
                                        0x80,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x01};
    loom::ib_address addr{std::span{gid}, 1234, 5};
    auto formatted = std::format("{}", addr);
    REQUIRE(formatted.find("lid=5") != std::string::npos);
    REQUIRE(formatted.find("qpn=1234") != std::string::npos);
}
