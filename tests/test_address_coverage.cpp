// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
// Simple coverage test without boost::ut to verify coverage is working
#include <loom/core/address.hpp>
#include <loom/core/error.hpp>

#include <cassert>
#include <cstring>
#include <iostream>

#include <arpa/inet.h>
#include <netinet/in.h>

int main() {
    int failed = 0;

    // Test address_from_raw with null data
    {
        auto result = loom::address_from_raw(nullptr, 100, loom::address_format::inet);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: null data should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw with zero length
    {
        int dummy = 0;
        auto result = loom::address_from_raw(&dummy, 0, loom::address_format::inet);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: zero length should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw with unspecified format
    {
        int dummy = 0;
        auto result =
            loom::address_from_raw(&dummy, sizeof(dummy), loom::address_format::unspecified);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: unspecified format should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw inet too small
    {
        char small[4] = {};
        auto result = loom::address_from_raw(small, sizeof(small), loom::address_format::inet);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: inet too small should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw inet
    {
        ::sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(8080);
        sa.sin_addr.s_addr = htonl(0xC0A80101);  // 192.168.1.1

        auto result = loom::address_from_raw(&sa, sizeof(sa), loom::address_format::inet);
        if (!std::holds_alternative<loom::ipv4_address>(result)) {
            std::cerr << "FAIL: inet should parse to ipv4_address\n";
            ++failed;
        } else {
            const auto& ipv4 = std::get<loom::ipv4_address>(result);
            auto octets = ipv4.octets();
            if (octets[0] != 192 || octets[1] != 168 || octets[2] != 1 || octets[3] != 1) {
                std::cerr << "FAIL: ipv4 octets mismatch\n";
                ++failed;
            }
            if (ipv4.port() != 8080) {
                std::cerr << "FAIL: ipv4 port mismatch\n";
                ++failed;
            }
        }
    }

    // Test address_from_raw inet6 too small
    {
        char small[4] = {};
        auto result = loom::address_from_raw(small, sizeof(small), loom::address_format::inet6);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: inet6 too small should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw inet6
    {
        ::sockaddr_in6 sa6{};
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(9000);
        sa6.sin6_addr.s6_addr[0] = 0x20;
        sa6.sin6_addr.s6_addr[1] = 0x01;
        sa6.sin6_addr.s6_addr[2] = 0x0d;
        sa6.sin6_addr.s6_addr[3] = 0xb8;
        sa6.sin6_addr.s6_addr[15] = 0x01;

        auto result = loom::address_from_raw(&sa6, sizeof(sa6), loom::address_format::inet6);
        if (!std::holds_alternative<loom::ipv6_address>(result)) {
            std::cerr << "FAIL: inet6 should parse to ipv6_address\n";
            ++failed;
        } else {
            const auto& ipv6 = std::get<loom::ipv6_address>(result);
            auto segs = ipv6.segments();
            if (segs[0] != 0x2001 || segs[1] != 0x0db8) {
                std::cerr << "FAIL: ipv6 segments mismatch\n";
                ++failed;
            }
            if (ipv6.port() != 9000) {
                std::cerr << "FAIL: ipv6 port mismatch\n";
                ++failed;
            }
        }
    }

    // Test address_from_raw ib too small
    {
        char small[8] = {};
        auto result = loom::address_from_raw(small, sizeof(small), loom::address_format::ib);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: ib too small should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw ib
    {
        std::array<std::uint8_t, 16> raw_gid = {0xFE,
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

        auto result =
            loom::address_from_raw(raw_gid.data(), raw_gid.size(), loom::address_format::ib);
        if (!std::holds_alternative<loom::ib_address>(result)) {
            std::cerr << "FAIL: ib should parse to ib_address\n";
            ++failed;
        } else {
            const auto& ib = std::get<loom::ib_address>(result);
            auto gid = ib.gid();
            if (gid[0] != 0xFE || gid[1] != 0x80) {
                std::cerr << "FAIL: ib gid mismatch\n";
                ++failed;
            }
        }
    }

    // Test address_from_raw ethernet too small
    {
        char small[4] = {};
        auto result = loom::address_from_raw(small, sizeof(small), loom::address_format::ethernet);
        if (!std::holds_alternative<loom::unspecified_address>(result)) {
            std::cerr << "FAIL: ethernet too small should return unspecified\n";
            ++failed;
        }
    }

    // Test address_from_raw ethernet
    {
        std::array<std::uint8_t, 6> raw_mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};

        auto result =
            loom::address_from_raw(raw_mac.data(), raw_mac.size(), loom::address_format::ethernet);
        if (!std::holds_alternative<loom::ethernet_address>(result)) {
            std::cerr << "FAIL: ethernet should parse to ethernet_address\n";
            ++failed;
        } else {
            const auto& eth = std::get<loom::ethernet_address>(result);
            auto mac = eth.mac();
            if (mac[0] != 0x00 || mac[5] != 0x5E) {
                std::cerr << "FAIL: ethernet mac mismatch\n";
                ++failed;
            }
        }
    }

    // Test error_category::message
    {
        auto ec = loom::make_error_code(loom::errc::invalid_argument);
        auto msg = ec.message();
        if (msg.empty()) {
            std::cerr << "FAIL: error message should not be empty\n";
            ++failed;
        }
    }

    // Test error_category::message for various error codes
    {
        auto test_msg = [&](loom::errc e) {
            auto ec = loom::make_error_code(e);
            auto msg = ec.message();
            if (msg.empty()) {
                std::cerr << "FAIL: error message for " << static_cast<int>(e)
                          << " should not be empty\n";
                ++failed;
            }
        };
        test_msg(loom::errc::success);
        test_msg(loom::errc::no_data);
        test_msg(loom::errc::message_too_long);
        test_msg(loom::errc::no_space);
        test_msg(loom::errc::again);
        test_msg(loom::errc::io_error);
        test_msg(loom::errc::not_supported);
        test_msg(loom::errc::busy);
        test_msg(loom::errc::canceled);
        test_msg(loom::errc::no_memory);
        test_msg(loom::errc::already);
        test_msg(loom::errc::bad_flags);
        test_msg(loom::errc::no_entry);
        test_msg(loom::errc::not_connected);
        test_msg(loom::errc::address_in_use);
        test_msg(loom::errc::connection_refused);
        test_msg(loom::errc::address_not_available);
        test_msg(loom::errc::timeout);
    }

    if (failed == 0) {
        std::cout << "All tests passed\n";
        return 0;
    } else {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
}
