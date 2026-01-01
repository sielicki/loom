// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <format>
#include <string>

#include "ut.hpp"

using namespace boost::ut;

suite formatters_suite = [] {
    "format strong_type fabric_version"_test = [] {
        loom::fabric_version ver{0x12345678U};
        auto formatted = std::format("{}", ver);
        expect(formatted == "305419896") << "fabric_version should format as decimal";
    };

    "format strong_type caps"_test = [] {
        loom::caps c{0x100ULL};
        auto formatted = std::format("{}", c);
        expect(formatted == "256") << "caps should format as decimal";
    };

    "format context_ptr"_test = [] {
        int value = 42;
        loom::context_ptr<int> ptr{&value};
        auto formatted = std::format("{}", ptr);
        expect(!formatted.empty()) << "context_ptr should format as pointer address";
    };

    "format context_ptr nullptr"_test = [] {
        loom::context_ptr<void> null_ptr{nullptr};
        auto formatted = std::format("{}", null_ptr);
        expect(formatted.find('0') != std::string::npos) << "null context_ptr should contain 0";
    };

    "format address_format enum"_test = [] {
        expect(std::format("{}", loom::address_format::unspecified) == "unspecified");
        expect(std::format("{}", loom::address_format::inet) == "inet");
        expect(std::format("{}", loom::address_format::inet6) == "inet6");
        expect(std::format("{}", loom::address_format::ib) == "ib");
        expect(std::format("{}", loom::address_format::ethernet) == "ethernet");
    };

    "format atomic_op enum"_test = [] {
        expect(std::format("{}", loom::atomic_op::min) == "min");
        expect(std::format("{}", loom::atomic_op::max) == "max");
        expect(std::format("{}", loom::atomic_op::sum) == "sum");
        expect(std::format("{}", loom::atomic_op::cswap) == "cswap");
    };

    "format atomic_datatype enum"_test = [] {
        expect(std::format("{}", loom::atomic_datatype::int8) == "int8");
        expect(std::format("{}", loom::atomic_datatype::uint64) == "uint64");
        expect(std::format("{}", loom::atomic_datatype::float32) == "float32");
        expect(std::format("{}", loom::atomic_datatype::float64) == "float64");
    };

    "format threading_mode enum"_test = [] {
        expect(std::format("{}", loom::threading_mode::unspecified) == "unspecified");
        expect(std::format("{}", loom::threading_mode::safe) == "safe");
        expect(std::format("{}", loom::threading_mode::domain) == "domain");
    };

    "format progress_mode enum"_test = [] {
        expect(std::format("{}", loom::progress_mode::unspecified) == "unspecified");
        expect(std::format("{}", loom::progress_mode::auto_progress) == "auto");
        expect(std::format("{}", loom::progress_mode::manual) == "manual");
    };

    "format errc enum"_test = [] {
        expect(std::format("{}", loom::errc::success) == "success");
        expect(std::format("{}", loom::errc::again) == "again");
        expect(std::format("{}", loom::errc::busy) == "busy");
        expect(std::format("{}", loom::errc::timeout) == "timeout");
        expect(std::format("{}", loom::errc::not_supported) == "not_supported");
    };

    "format hmem_iface enum"_test = [] {
        expect(std::format("{}", loom::hmem_iface::system) == "system");
        expect(std::format("{}", loom::hmem_iface::cuda) == "cuda");
        expect(std::format("{}", loom::hmem_iface::rocr) == "rocr");
        expect(std::format("{}", loom::hmem_iface::level_zero) == "level_zero");
    };

    "format ipv4_address"_test = [] {
        loom::ipv4_address addr{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        auto formatted = std::format("{}", addr);
        expect(formatted == "192.168.1.1") << "got: " << formatted;
    };

    "format ipv4_address with port"_test = [] {
        loom::ipv4_address addr{
            std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 8080};
        auto formatted = std::format("{}", addr);
        expect(formatted == "10.0.0.1:8080") << "got: " << formatted;
    };

    "format ipv6_address"_test = [] {
        std::array<std::uint16_t, 8> segs = {0, 0, 0, 0, 0, 0, 0, 1};
        loom::ipv6_address addr{std::span{segs}};
        auto formatted = std::format("{}", addr);
        expect(formatted == "0:0:0:0:0:0:0:1") << "got: " << formatted;
    };

    "format ethernet_address"_test = [] {
        std::array<std::uint8_t, 6> mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        loom::ethernet_address addr{std::span{mac}};
        auto formatted = std::format("{}", addr);
        expect(formatted == "aa:bb:cc:dd:ee:ff") << "got: " << formatted;
    };

    "format unspecified_address"_test = [] {
        loom::unspecified_address addr{};
        auto formatted = std::format("{}", addr);
        expect(formatted == "<unspecified>") << "got: " << formatted;
    };

    "format address variant ipv4"_test = [] {
        loom::address addr = loom::ipv4_address{
            std::uint8_t{127}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}};
        auto formatted = std::format("{}", addr);
        expect(formatted == "127.0.0.1") << "got: " << formatted;
    };

    "format remote_memory"_test = [] {
        loom::remote_memory rm{0x1000, 0xABCD, 4096};
        auto formatted = std::format("{}", rm);
        expect(formatted.find("0x1000") != std::string::npos) << "should contain address";
        expect(formatted.find("0xabcd") != std::string::npos) << "should contain key";
        expect(formatted.find("4096") != std::string::npos) << "should contain length";
    };

    "format endpoint_type"_test = [] {
        auto formatted = std::format("{}", loom::endpoint_types::msg);
        expect(formatted == "message") << "got: " << formatted;

        formatted = std::format("{}", loom::endpoint_types::rdm);
        expect(formatted == "reliable_datagram") << "got: " << formatted;

        formatted = std::format("{}", loom::endpoint_types::dgram);
        expect(formatted == "datagram") << "got: " << formatted;
    };

    "format endpoint tags"_test = [] {
        expect(std::format("{}", loom::msg_endpoint_tag{}) == "message");
        expect(std::format("{}", loom::rdm_endpoint_tag{}) == "reliable_datagram");
        expect(std::format("{}", loom::dgram_endpoint_tag{}) == "datagram");
    };

    "format result success"_test = [] {
        loom::result<int> res{42};
        auto formatted = std::format("{}", res);
        expect(formatted.find("ok") != std::string::npos) << "should contain ok";
        expect(formatted.find("42") != std::string::npos) << "should contain value";
    };

    "format result error"_test = [] {
        loom::result<int> res = std::unexpected(loom::make_error_code(loom::errc::busy));
        auto formatted = std::format("{}", res);
        expect(formatted.find("error") != std::string::npos) << "should contain error";
    };

    "format void_result success"_test = [] {
        loom::void_result res{};
        auto formatted = std::format("{}", res);
        expect(formatted == "ok") << "got: " << formatted;
    };

    "format std::error_code"_test = [] {
        auto ec = std::make_error_code(std::errc::invalid_argument);
        auto formatted = std::format("{}", ec);
        expect(!formatted.empty()) << "error_code should format";
    };

    "format ib_address"_test = [] {
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
        expect(formatted.find("lid=5") != std::string::npos) << "should contain lid";
        expect(formatted.find("qpn=1234") != std::string::npos) << "should contain qpn";
    };
};

auto main() -> int {}
