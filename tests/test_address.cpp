// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/address.hpp>

#include <array>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ipv4_address construction from octets", "[address]") {
    loom::ipv4_address addr{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    auto octets = addr.octets();

    REQUIRE(octets[0] == 192);
    REQUIRE(octets[1] == 168);
    REQUIRE(octets[2] == 1);
    REQUIRE(octets[3] == 1);
    REQUIRE(addr.port() == 0);
}

TEST_CASE("ipv4_address construction with port", "[address]") {
    loom::ipv4_address addr{
        std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 8080};

    REQUIRE(addr.port() == 8080);
}

TEST_CASE("ipv4_address construction from uint32", "[address]") {
    loom::ipv4_address addr{0xC0A80101U, 443};
    auto octets = addr.octets();

    REQUIRE(octets[0] == 192);
    REQUIRE(octets[1] == 168);
    REQUIRE(octets[2] == 1);
    REQUIRE(octets[3] == 1);
    REQUIRE(addr.port() == 443);
}

TEST_CASE("ipv4_address to_uint32", "[address]") {
    loom::ipv4_address addr{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    REQUIRE(addr.to_uint32() == 0xC0A80101U);
}

TEST_CASE("ipv4_address is_loopback", "[address]") {
    REQUIRE(loom::ipv4_address{std::uint8_t{127}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                .is_loopback());
    REQUIRE(loom::ipv4_address{
        std::uint8_t{127}, std::uint8_t{255}, std::uint8_t{255}, std::uint8_t{255}}
                .is_loopback());
    REQUIRE_FALSE(
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}}
            .is_loopback());
}

TEST_CASE("ipv4_address is_private", "[address]") {
    REQUIRE(loom::ipv4_address{std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                .is_private());
    REQUIRE(
        loom::ipv4_address{std::uint8_t{172}, std::uint8_t{16}, std::uint8_t{0}, std::uint8_t{1}}
            .is_private());
    REQUIRE(loom::ipv4_address{
        std::uint8_t{172}, std::uint8_t{31}, std::uint8_t{255}, std::uint8_t{255}}
                .is_private());
    REQUIRE(
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{0}, std::uint8_t{1}}
            .is_private());
    REQUIRE_FALSE(
        loom::ipv4_address{std::uint8_t{8}, std::uint8_t{8}, std::uint8_t{8}, std::uint8_t{8}}
            .is_private());
    REQUIRE_FALSE(
        loom::ipv4_address{std::uint8_t{172}, std::uint8_t{32}, std::uint8_t{0}, std::uint8_t{1}}
            .is_private());
}

TEST_CASE("ipv4_address is_multicast", "[address]") {
    REQUIRE(loom::ipv4_address{std::uint8_t{224}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                .is_multicast());
    REQUIRE(loom::ipv4_address{
        std::uint8_t{239}, std::uint8_t{255}, std::uint8_t{255}, std::uint8_t{255}}
                .is_multicast());
    REQUIRE_FALSE(
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}}
            .is_multicast());
    REQUIRE_FALSE(
        loom::ipv4_address{std::uint8_t{240}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
            .is_multicast());
}

TEST_CASE("ipv4_address comparison", "[address]") {
    loom::ipv4_address a{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    loom::ipv4_address b{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    loom::ipv4_address c{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{2}};

    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a < c);
}

TEST_CASE("ipv6_address construction", "[address]") {
    std::array<std::uint16_t, 8> segs = {0x2001, 0x0db8, 0, 0, 0, 0, 0, 1};
    loom::ipv6_address addr{std::span{segs}};
    auto result_segs = addr.segments();

    REQUIRE(result_segs[0] == 0x2001);
    REQUIRE(result_segs[1] == 0x0db8);
    REQUIRE(result_segs[7] == 1);
}

TEST_CASE("ipv6_address is_loopback", "[address]") {
    std::array<std::uint16_t, 8> loopback = {0, 0, 0, 0, 0, 0, 0, 1};
    loom::ipv6_address addr{std::span{loopback}};
    REQUIRE(addr.is_loopback());

    std::array<std::uint16_t, 8> not_loopback = {0x2001, 0, 0, 0, 0, 0, 0, 1};
    loom::ipv6_address addr2{std::span{not_loopback}};
    REQUIRE_FALSE(addr2.is_loopback());
}

TEST_CASE("ipv6_address is_multicast", "[address]") {
    std::array<std::uint16_t, 8> multicast = {0xFF02, 0, 0, 0, 0, 0, 0, 1};
    loom::ipv6_address addr{std::span{multicast}};
    REQUIRE(addr.is_multicast());
}

TEST_CASE("ib_address construction", "[address]") {
    std::array<std::uint8_t, 16> gid = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    loom::ib_address addr{std::span{gid}, 1234, 5};

    REQUIRE(addr.qpn() == 1234U);
    REQUIRE(addr.lid() == 5);

    auto result_gid = addr.gid();
    REQUIRE(result_gid[0] == 0xFE);
    REQUIRE(result_gid[1] == 0x80);
}

TEST_CASE("ethernet_address construction", "[address]") {
    std::array<std::uint8_t, 6> mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    loom::ethernet_address addr{std::span{mac}};

    auto result_mac = addr.mac();
    REQUIRE(result_mac[0] == 0x00);
    REQUIRE(result_mac[5] == 0x5E);
}

TEST_CASE("ethernet_address is_unicast", "[address]") {
    std::array<std::uint8_t, 6> unicast_mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    loom::ethernet_address unicast{std::span{unicast_mac}};
    REQUIRE(unicast.is_unicast());
    REQUIRE_FALSE(unicast.is_multicast());
}

TEST_CASE("ethernet_address is_multicast", "[address]") {
    std::array<std::uint8_t, 6> multicast_mac = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    loom::ethernet_address multicast{std::span{multicast_mac}};
    REQUIRE(multicast.is_multicast());
    REQUIRE_FALSE(multicast.is_unicast());
}

TEST_CASE("ethernet_address is_broadcast", "[address]") {
    std::array<std::uint8_t, 6> broadcast_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    loom::ethernet_address broadcast{std::span{broadcast_mac}};
    REQUIRE(broadcast.is_broadcast());
}

TEST_CASE("ethernet_address is_locally_administered", "[address]") {
    std::array<std::uint8_t, 6> local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    loom::ethernet_address local{std::span{local_mac}};
    REQUIRE(local.is_locally_administered());

    std::array<std::uint8_t, 6> global_mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    loom::ethernet_address global{std::span{global_mac}};
    REQUIRE_FALSE(global.is_locally_administered());
}

TEST_CASE("unspecified_address comparison", "[address]") {
    loom::unspecified_address a{};
    loom::unspecified_address b{};
    REQUIRE(a == b);
}

TEST_CASE("address variant holds ipv4", "[address]") {
    loom::address addr =
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    REQUIRE(std::holds_alternative<loom::ipv4_address>(addr));
}

TEST_CASE("address variant holds unspecified", "[address]") {
    loom::address addr = loom::unspecified_address{};
    REQUIRE(std::holds_alternative<loom::unspecified_address>(addr));
}

TEST_CASE("address_ops::size", "[address]") {
    loom::address ipv4_addr =
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    REQUIRE(loom::address_ops::size(ipv4_addr) == sizeof(loom::ipv4_address));

    loom::address ipv6_addr = [] {
        std::array<std::uint16_t, 8> segs{};
        return loom::ipv6_address{std::span{segs}};
    }();
    REQUIRE(loom::address_ops::size(ipv6_addr) == sizeof(loom::ipv6_address));
}

TEST_CASE("address_ops::is_rdma_capable", "[address]") {
    std::array<std::uint8_t, 16> gid{};
    loom::address ib_addr = loom::ib_address{std::span{gid}};
    REQUIRE(loom::address_ops::is_rdma_capable(ib_addr));

    loom::address ipv4_addr =
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    REQUIRE_FALSE(loom::address_ops::is_rdma_capable(ipv4_addr));
}

TEST_CASE("address_ops::is_network_protocol", "[address]") {
    loom::address ipv4_addr =
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    REQUIRE(loom::address_ops::is_network_protocol(ipv4_addr));

    std::array<std::uint16_t, 8> segs{};
    loom::address ipv6_addr = loom::ipv6_address{std::span{segs}};
    REQUIRE(loom::address_ops::is_network_protocol(ipv6_addr));

    std::array<std::uint8_t, 6> mac{};
    loom::address eth_addr = loom::ethernet_address{std::span{mac}};
    REQUIRE_FALSE(loom::address_ops::is_network_protocol(eth_addr));
}

TEST_CASE("addresses factory functions", "[address]") {
    auto ipv4 = loom::addresses::ipv4(
        std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 80);
    REQUIRE(std::holds_alternative<loom::ipv4_address>(ipv4));

    auto unspec = loom::addresses::unspecified();
    REQUIRE(std::holds_alternative<loom::unspecified_address>(unspec));
}

TEST_CASE("predefined addresses", "[address]") {
    REQUIRE(std::holds_alternative<loom::ipv4_address>(loom::addresses::localhost_v4));
    REQUIRE(std::holds_alternative<loom::ipv4_address>(loom::addresses::any_v4));
    REQUIRE(std::holds_alternative<loom::ipv4_address>(loom::addresses::broadcast_v4));
}

TEST_CASE("get_address_format", "[address]") {
    loom::address ipv4 =
        loom::ipv4_address{std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
    REQUIRE(loom::get_address_format(ipv4) == loom::address_format::inet);

    std::array<std::uint16_t, 8> segs{};
    loom::address ipv6 = loom::ipv6_address{std::span{segs}};
    REQUIRE(loom::get_address_format(ipv6) == loom::address_format::inet6);

    std::array<std::uint8_t, 16> gid{};
    loom::address ib = loom::ib_address{std::span{gid}};
    REQUIRE(loom::get_address_format(ib) == loom::address_format::ib);

    std::array<std::uint8_t, 6> mac{};
    loom::address eth = loom::ethernet_address{std::span{mac}};
    REQUIRE(loom::get_address_format(eth) == loom::address_format::ethernet);

    loom::address unspec = loom::unspecified_address{};
    REQUIRE(loom::get_address_format(unspec) == loom::address_format::unspecified);
}

TEST_CASE("get_port", "[address]") {
    loom::address ipv4 = loom::ipv4_address{
        std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}, 8080};
    REQUIRE(loom::get_port(ipv4) == 8080);

    loom::address unspec = loom::unspecified_address{};
    REQUIRE(loom::get_port(unspec) == 0);
}

TEST_CASE("trivially_serializable static asserts", "[address]") {
    static_assert(loom::trivially_serializable<loom::ipv4_address>);
    static_assert(loom::trivially_serializable<loom::ipv6_address>);
    static_assert(loom::trivially_serializable<loom::ib_address>);
    static_assert(loom::trivially_serializable<loom::ethernet_address>);
    static_assert(loom::trivially_serializable<loom::unspecified_address>);
}

TEST_CASE("address concepts", "[address]") {
    static_assert(loom::network_address<loom::ipv4_address>);
    static_assert(loom::network_address<loom::ipv6_address>);
    static_assert(loom::gid_address<loom::ib_address>);
    static_assert(loom::mac_address<loom::ethernet_address>);
    static_assert(loom::address_type<loom::unspecified_address>);
}

TEST_CASE("address_from_raw null data returns unspecified", "[address]") {
    auto result = loom::address_from_raw(nullptr, 100, loom::address_format::inet);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw zero length returns unspecified", "[address]") {
    int dummy = 0;
    auto result = loom::address_from_raw(&dummy, 0, loom::address_format::inet);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw unspecified format returns unspecified", "[address]") {
    int dummy = 0;
    auto result = loom::address_from_raw(&dummy, sizeof(dummy), loom::address_format::unspecified);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw inet too small returns unspecified", "[address]") {
    std::array<char, 4> small{};
    auto result = loom::address_from_raw(small.data(), small.size(), loom::address_format::inet);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw inet6 too small returns unspecified", "[address]") {
    std::array<char, 4> small{};
    auto result = loom::address_from_raw(small.data(), small.size(), loom::address_format::inet6);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw ib too small returns unspecified", "[address]") {
    std::array<char, 8> small{};
    auto result = loom::address_from_raw(small.data(), small.size(), loom::address_format::ib);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw ethernet too small returns unspecified", "[address]") {
    std::array<char, 4> small{};
    auto result =
        loom::address_from_raw(small.data(), small.size(), loom::address_format::ethernet);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(result));
}

TEST_CASE("address_from_raw inet parses sockaddr_in", "[address]") {
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8080);
    sa.sin_addr.s_addr = htonl(0xC0A80101);  // 192.168.1.1

    auto result = loom::address_from_raw(&sa, sizeof(sa), loom::address_format::inet);
    REQUIRE(std::holds_alternative<loom::ipv4_address>(result));

    const auto& ipv4 = std::get<loom::ipv4_address>(result);
    auto octets = ipv4.octets();
    REQUIRE(octets[0] == 192);
    REQUIRE(octets[1] == 168);
    REQUIRE(octets[2] == 1);
    REQUIRE(octets[3] == 1);
    REQUIRE(ipv4.port() == 8080);
}

TEST_CASE("address_from_raw inet parses loopback", "[address]") {
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1

    auto result = loom::address_from_raw(&sa, sizeof(sa), loom::address_format::inet);
    REQUIRE(std::holds_alternative<loom::ipv4_address>(result));

    const auto& ipv4 = std::get<loom::ipv4_address>(result);
    REQUIRE(ipv4.is_loopback());
    REQUIRE(ipv4.port() == 443);
}

TEST_CASE("address_from_raw inet6 parses sockaddr_in6", "[address]") {
    ::sockaddr_in6 sa6{};
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(9000);
    sa6.sin6_addr.s6_addr[0] = 0x20;
    sa6.sin6_addr.s6_addr[1] = 0x01;
    sa6.sin6_addr.s6_addr[2] = 0x0d;
    sa6.sin6_addr.s6_addr[3] = 0xb8;
    sa6.sin6_addr.s6_addr[15] = 0x01;

    auto result = loom::address_from_raw(&sa6, sizeof(sa6), loom::address_format::inet6);
    REQUIRE(std::holds_alternative<loom::ipv6_address>(result));

    const auto& ipv6 = std::get<loom::ipv6_address>(result);
    auto segs = ipv6.segments();
    REQUIRE(segs[0] == 0x2001);
    REQUIRE(segs[1] == 0x0db8);
    REQUIRE(segs[7] == 0x0001);
    REQUIRE(ipv6.port() == 9000);
}

TEST_CASE("address_from_raw inet6 parses loopback", "[address]") {
    ::sockaddr_in6 sa6{};
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(0);
    sa6.sin6_addr.s6_addr[15] = 0x01;

    auto result = loom::address_from_raw(&sa6, sizeof(sa6), loom::address_format::inet6);
    REQUIRE(std::holds_alternative<loom::ipv6_address>(result));

    const auto& ipv6 = std::get<loom::ipv6_address>(result);
    REQUIRE(ipv6.is_loopback());
}

TEST_CASE("address_from_raw ib parses raw gid", "[address]") {
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

    auto result = loom::address_from_raw(raw_gid.data(), raw_gid.size(), loom::address_format::ib);
    REQUIRE(std::holds_alternative<loom::ib_address>(result));

    const auto& ib = std::get<loom::ib_address>(result);
    auto gid = ib.gid();
    REQUIRE(gid[0] == 0xFE);
    REQUIRE(gid[1] == 0x80);
    REQUIRE(gid[15] == 0x01);
    REQUIRE(ib.qpn() == 0U);
    REQUIRE(ib.lid() == 0);
}

TEST_CASE("address_from_raw ethernet parses raw mac", "[address]") {
    std::array<std::uint8_t, 6> raw_mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};

    auto result =
        loom::address_from_raw(raw_mac.data(), raw_mac.size(), loom::address_format::ethernet);
    REQUIRE(std::holds_alternative<loom::ethernet_address>(result));

    const auto& eth = std::get<loom::ethernet_address>(result);
    auto mac = eth.mac();
    REQUIRE(mac[0] == 0x00);
    REQUIRE(mac[1] == 0x1A);
    REQUIRE(mac[5] == 0x5E);
    REQUIRE(eth.is_unicast());
}

TEST_CASE("address_from_raw ethernet parses broadcast mac", "[address]") {
    std::array<std::uint8_t, 6> broadcast_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    auto result = loom::address_from_raw(
        broadcast_mac.data(), broadcast_mac.size(), loom::address_format::ethernet);
    REQUIRE(std::holds_alternative<loom::ethernet_address>(result));

    const auto& eth = std::get<loom::ethernet_address>(result);
    REQUIRE(eth.is_broadcast());
}

TEST_CASE("address_from_raw ethernet parses multicast mac", "[address]") {
    std::array<std::uint8_t, 6> multicast_mac = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};

    auto result = loom::address_from_raw(
        multicast_mac.data(), multicast_mac.size(), loom::address_format::ethernet);
    REQUIRE(std::holds_alternative<loom::ethernet_address>(result));

    const auto& eth = std::get<loom::ethernet_address>(result);
    REQUIRE(eth.is_multicast());
}
