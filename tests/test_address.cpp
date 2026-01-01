// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/address.hpp>

#include <array>

#include "ut.hpp"

using namespace boost::ut;

suite address_suite = [] {
    "ipv4_address construction from octets"_test = [] {
        loom::ipv4_address addr{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        auto octets = addr.octets();

        expect(octets[0] == 192);
        expect(octets[1] == 168);
        expect(octets[2] == 1);
        expect(octets[3] == 1);
        expect(addr.port() == 0);
    };

    "ipv4_address construction with port"_test = [] {
        loom::ipv4_address addr{
            std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 8080};

        expect(addr.port() == 8080);
    };

    "ipv4_address construction from uint32"_test = [] {
        loom::ipv4_address addr{0xC0A80101U, 443};
        auto octets = addr.octets();

        expect(octets[0] == 192);
        expect(octets[1] == 168);
        expect(octets[2] == 1);
        expect(octets[3] == 1);
        expect(addr.port() == 443);
    };

    "ipv4_address to_uint32"_test = [] {
        loom::ipv4_address addr{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        expect(addr.to_uint32() == 0xC0A80101U);
    };

    "ipv4_address is_loopback"_test = [] {
        expect(
            loom::ipv4_address{std::uint8_t{127}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                .is_loopback());
        expect(loom::ipv4_address{
            std::uint8_t{127}, std::uint8_t{255}, std::uint8_t{255}, std::uint8_t{255}}
                   .is_loopback());
        expect(!loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}}
                    .is_loopback());
    };

    "ipv4_address is_private"_test = [] {
        expect(
            loom::ipv4_address{std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                .is_private());
        expect(loom::ipv4_address{
            std::uint8_t{172}, std::uint8_t{16}, std::uint8_t{0}, std::uint8_t{1}}
                   .is_private());
        expect(loom::ipv4_address{
            std::uint8_t{172}, std::uint8_t{31}, std::uint8_t{255}, std::uint8_t{255}}
                   .is_private());
        expect(loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{0}, std::uint8_t{1}}
                   .is_private());
        expect(
            !loom::ipv4_address{std::uint8_t{8}, std::uint8_t{8}, std::uint8_t{8}, std::uint8_t{8}}
                 .is_private());
        expect(!loom::ipv4_address{
            std::uint8_t{172}, std::uint8_t{32}, std::uint8_t{0}, std::uint8_t{1}}
                    .is_private());
    };

    "ipv4_address is_multicast"_test = [] {
        expect(
            loom::ipv4_address{std::uint8_t{224}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                .is_multicast());
        expect(loom::ipv4_address{
            std::uint8_t{239}, std::uint8_t{255}, std::uint8_t{255}, std::uint8_t{255}}
                   .is_multicast());
        expect(!loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}}
                    .is_multicast());
        expect(!loom::ipv4_address{
            std::uint8_t{240}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}}
                    .is_multicast());
    };

    "ipv4_address comparison"_test = [] {
        loom::ipv4_address a{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        loom::ipv4_address b{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        loom::ipv4_address c{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{2}};

        expect(a == b);
        expect(a != c);
        expect(a < c);
    };

    "ipv6_address construction"_test = [] {
        std::array<std::uint16_t, 8> segs = {0x2001, 0x0db8, 0, 0, 0, 0, 0, 1};
        loom::ipv6_address addr{std::span{segs}};
        auto result_segs = addr.segments();

        expect(result_segs[0] == 0x2001);
        expect(result_segs[1] == 0x0db8);
        expect(result_segs[7] == 1);
    };

    "ipv6_address is_loopback"_test = [] {
        std::array<std::uint16_t, 8> loopback = {0, 0, 0, 0, 0, 0, 0, 1};
        loom::ipv6_address addr{std::span{loopback}};
        expect(addr.is_loopback());

        std::array<std::uint16_t, 8> not_loopback = {0x2001, 0, 0, 0, 0, 0, 0, 1};
        loom::ipv6_address addr2{std::span{not_loopback}};
        expect(!addr2.is_loopback());
    };

    "ipv6_address is_multicast"_test = [] {
        std::array<std::uint16_t, 8> multicast = {0xFF02, 0, 0, 0, 0, 0, 0, 1};
        loom::ipv6_address addr{std::span{multicast}};
        expect(addr.is_multicast());
    };

    "ib_address construction"_test = [] {
        std::array<std::uint8_t, 16> gid = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        loom::ib_address addr{std::span{gid}, 1234, 5};

        expect(addr.qpn() == 1234U);
        expect(addr.lid() == 5);

        auto result_gid = addr.gid();
        expect(result_gid[0] == 0xFE);
        expect(result_gid[1] == 0x80);
    };

    "ethernet_address construction"_test = [] {
        std::array<std::uint8_t, 6> mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
        loom::ethernet_address addr{std::span{mac}};

        auto result_mac = addr.mac();
        expect(result_mac[0] == 0x00);
        expect(result_mac[5] == 0x5E);
    };

    "ethernet_address is_unicast"_test = [] {
        std::array<std::uint8_t, 6> unicast_mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
        loom::ethernet_address unicast{std::span{unicast_mac}};
        expect(unicast.is_unicast());
        expect(!unicast.is_multicast());
    };

    "ethernet_address is_multicast"_test = [] {
        std::array<std::uint8_t, 6> multicast_mac = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
        loom::ethernet_address multicast{std::span{multicast_mac}};
        expect(multicast.is_multicast());
        expect(!multicast.is_unicast());
    };

    "ethernet_address is_broadcast"_test = [] {
        std::array<std::uint8_t, 6> broadcast_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        loom::ethernet_address broadcast{std::span{broadcast_mac}};
        expect(broadcast.is_broadcast());
    };

    "ethernet_address is_locally_administered"_test = [] {
        std::array<std::uint8_t, 6> local_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        loom::ethernet_address local{std::span{local_mac}};
        expect(local.is_locally_administered());

        std::array<std::uint8_t, 6> global_mac = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
        loom::ethernet_address global{std::span{global_mac}};
        expect(!global.is_locally_administered());
    };

    "unspecified_address comparison"_test = [] {
        loom::unspecified_address a{};
        loom::unspecified_address b{};
        expect(a == b);
    };

    "address variant holds ipv4"_test = [] {
        loom::address addr = loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        expect(std::holds_alternative<loom::ipv4_address>(addr));
    };

    "address variant holds unspecified"_test = [] {
        loom::address addr = loom::unspecified_address{};
        expect(std::holds_alternative<loom::unspecified_address>(addr));
    };

    "address_ops::size"_test = [] {
        loom::address ipv4_addr = loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        expect(loom::address_ops::size(ipv4_addr) == sizeof(loom::ipv4_address));

        loom::address ipv6_addr = [] {
            std::array<std::uint16_t, 8> segs{};
            return loom::ipv6_address{std::span{segs}};
        }();
        expect(loom::address_ops::size(ipv6_addr) == sizeof(loom::ipv6_address));
    };

    "address_ops::is_rdma_capable"_test = [] {
        std::array<std::uint8_t, 16> gid{};
        loom::address ib_addr = loom::ib_address{std::span{gid}};
        expect(loom::address_ops::is_rdma_capable(ib_addr));

        loom::address ipv4_addr = loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        expect(!loom::address_ops::is_rdma_capable(ipv4_addr));
    };

    "address_ops::is_network_protocol"_test = [] {
        loom::address ipv4_addr = loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        expect(loom::address_ops::is_network_protocol(ipv4_addr));

        std::array<std::uint16_t, 8> segs{};
        loom::address ipv6_addr = loom::ipv6_address{std::span{segs}};
        expect(loom::address_ops::is_network_protocol(ipv6_addr));

        std::array<std::uint8_t, 6> mac{};
        loom::address eth_addr = loom::ethernet_address{std::span{mac}};
        expect(!loom::address_ops::is_network_protocol(eth_addr));
    };

    "addresses factory functions"_test = [] {
        auto ipv4 = loom::addresses::ipv4(
            std::uint8_t{10}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 80);
        expect(std::holds_alternative<loom::ipv4_address>(ipv4));

        auto unspec = loom::addresses::unspecified();
        expect(std::holds_alternative<loom::unspecified_address>(unspec));
    };

    "predefined addresses"_test = [] {
        expect(std::holds_alternative<loom::ipv4_address>(loom::addresses::localhost_v4));
        expect(std::holds_alternative<loom::ipv4_address>(loom::addresses::any_v4));
        expect(std::holds_alternative<loom::ipv4_address>(loom::addresses::broadcast_v4));
    };

    "get_address_format"_test = [] {
        loom::address ipv4 = loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}};
        expect(loom::get_address_format(ipv4) == loom::address_format::inet);

        std::array<std::uint16_t, 8> segs{};
        loom::address ipv6 = loom::ipv6_address{std::span{segs}};
        expect(loom::get_address_format(ipv6) == loom::address_format::inet6);

        std::array<std::uint8_t, 16> gid{};
        loom::address ib = loom::ib_address{std::span{gid}};
        expect(loom::get_address_format(ib) == loom::address_format::ib);

        std::array<std::uint8_t, 6> mac{};
        loom::address eth = loom::ethernet_address{std::span{mac}};
        expect(loom::get_address_format(eth) == loom::address_format::ethernet);

        loom::address unspec = loom::unspecified_address{};
        expect(loom::get_address_format(unspec) == loom::address_format::unspecified);
    };

    "get_port"_test = [] {
        loom::address ipv4 = loom::ipv4_address{
            std::uint8_t{192}, std::uint8_t{168}, std::uint8_t{1}, std::uint8_t{1}, 8080};
        expect(loom::get_port(ipv4) == 8080);

        loom::address unspec = loom::unspecified_address{};
        expect(loom::get_port(unspec) == 0);
    };

    "trivially_serializable static asserts"_test = [] {
        static_assert(loom::trivially_serializable<loom::ipv4_address>);
        static_assert(loom::trivially_serializable<loom::ipv6_address>);
        static_assert(loom::trivially_serializable<loom::ib_address>);
        static_assert(loom::trivially_serializable<loom::ethernet_address>);
        static_assert(loom::trivially_serializable<loom::unspecified_address>);
    };

    "address concepts"_test = [] {
        static_assert(loom::network_address<loom::ipv4_address>);
        static_assert(loom::network_address<loom::ipv6_address>);
        static_assert(loom::gid_address<loom::ib_address>);
        static_assert(loom::mac_address<loom::ethernet_address>);
        static_assert(loom::address_type<loom::unspecified_address>);
    };
};

auto main() -> int {}
