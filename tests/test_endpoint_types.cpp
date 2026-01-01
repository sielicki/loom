// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/endpoint_types.hpp>

#include <variant>

#include "ut.hpp"

using namespace boost::ut;

suite endpoint_types_suite = [] {
    "endpoint_tag concept"_test = [] {
        static_assert(loom::endpoint_tag<loom::msg_endpoint_tag>);
        static_assert(loom::endpoint_tag<loom::rdm_endpoint_tag>);
        static_assert(loom::endpoint_tag<loom::dgram_endpoint_tag>);

        static_assert(!loom::endpoint_tag<int>);
        static_assert(!loom::endpoint_tag<void>);
    };

    "endpoint_type variant holds tags"_test = [] {
        loom::endpoint_type msg = loom::endpoint_types::msg;
        loom::endpoint_type rdm = loom::endpoint_types::rdm;
        loom::endpoint_type dgram = loom::endpoint_types::dgram;

        expect(std::holds_alternative<loom::msg_endpoint_tag>(msg));
        expect(std::holds_alternative<loom::rdm_endpoint_tag>(rdm));
        expect(std::holds_alternative<loom::dgram_endpoint_tag>(dgram));
    };

    "msg_endpoint_tag properties"_test = [] {
        using props = loom::endpoint_properties<loom::msg_endpoint_tag>;

        static_assert(props::is_reliable);
        static_assert(props::is_connection_oriented);
        static_assert(props::supports_rma);
    };

    "rdm_endpoint_tag properties"_test = [] {
        using props = loom::endpoint_properties<loom::rdm_endpoint_tag>;

        static_assert(props::is_reliable);
        static_assert(!props::is_connection_oriented);
        static_assert(props::supports_rma);
    };

    "dgram_endpoint_tag properties"_test = [] {
        using props = loom::endpoint_properties<loom::dgram_endpoint_tag>;

        static_assert(!props::is_reliable);
        static_assert(!props::is_connection_oriented);
        static_assert(!props::supports_rma);
    };

    "is_reliable function"_test = [] {
        expect(loom::is_reliable(loom::endpoint_types::msg));
        expect(loom::is_reliable(loom::endpoint_types::rdm));
        expect(!loom::is_reliable(loom::endpoint_types::dgram));
    };

    "is_reliable constexpr"_test = [] {
        constexpr bool msg_reliable = loom::is_reliable(loom::endpoint_types::msg);
        constexpr bool rdm_reliable = loom::is_reliable(loom::endpoint_types::rdm);
        constexpr bool dgram_reliable = loom::is_reliable(loom::endpoint_types::dgram);

        static_assert(msg_reliable);
        static_assert(rdm_reliable);
        static_assert(!dgram_reliable);
    };

    "is_connection_oriented function"_test = [] {
        expect(loom::is_connection_oriented(loom::endpoint_types::msg));
        expect(!loom::is_connection_oriented(loom::endpoint_types::rdm));
        expect(!loom::is_connection_oriented(loom::endpoint_types::dgram));
    };

    "is_connection_oriented constexpr"_test = [] {
        constexpr bool msg_conn = loom::is_connection_oriented(loom::endpoint_types::msg);
        constexpr bool rdm_conn = loom::is_connection_oriented(loom::endpoint_types::rdm);
        constexpr bool dgram_conn = loom::is_connection_oriented(loom::endpoint_types::dgram);

        static_assert(msg_conn);
        static_assert(!rdm_conn);
        static_assert(!dgram_conn);
    };

    "supports_rma function"_test = [] {
        expect(loom::supports_rma(loom::endpoint_types::msg));
        expect(loom::supports_rma(loom::endpoint_types::rdm));
        expect(!loom::supports_rma(loom::endpoint_types::dgram));
    };

    "supports_rma constexpr"_test = [] {
        constexpr bool msg_rma = loom::supports_rma(loom::endpoint_types::msg);
        constexpr bool rdm_rma = loom::supports_rma(loom::endpoint_types::rdm);
        constexpr bool dgram_rma = loom::supports_rma(loom::endpoint_types::dgram);

        static_assert(msg_rma);
        static_assert(rdm_rma);
        static_assert(!dgram_rma);
    };

    "type_name function"_test = [] {
        expect(std::string_view{loom::type_name(loom::endpoint_types::msg)} == "message");
        expect(std::string_view{loom::type_name(loom::endpoint_types::rdm)} == "reliable_datagram");
        expect(std::string_view{loom::type_name(loom::endpoint_types::dgram)} == "datagram");
    };

    "type_name constexpr"_test = [] {
        constexpr const char* msg_name = loom::type_name(loom::endpoint_types::msg);
        constexpr const char* rdm_name = loom::type_name(loom::endpoint_types::rdm);
        constexpr const char* dgram_name = loom::type_name(loom::endpoint_types::dgram);

        expect(std::string_view{msg_name} == "message");
        expect(std::string_view{rdm_name} == "reliable_datagram");
        expect(std::string_view{dgram_name} == "datagram");
    };

    "endpoint_tag static name"_test = [] {
        static_assert(std::string_view{loom::msg_endpoint_tag::name} == "message");
        static_assert(std::string_view{loom::rdm_endpoint_tag::name} == "reliable_datagram");
        static_assert(std::string_view{loom::dgram_endpoint_tag::name} == "datagram");
    };

    "endpoint_type comparison"_test = [] {
        loom::endpoint_type msg1 = loom::endpoint_types::msg;
        loom::endpoint_type msg2 = loom::endpoint_types::msg;
        loom::endpoint_type rdm = loom::endpoint_types::rdm;

        expect(msg1 == msg2);
        expect(msg1 != rdm);
    };

    "constexpr endpoint_types"_test = [] {
        constexpr auto msg = loom::endpoint_types::msg;
        constexpr auto rdm = loom::endpoint_types::rdm;
        constexpr auto dgram = loom::endpoint_types::dgram;

        static_assert(std::holds_alternative<loom::msg_endpoint_tag>(msg));
        static_assert(std::holds_alternative<loom::rdm_endpoint_tag>(rdm));
        static_assert(std::holds_alternative<loom::dgram_endpoint_tag>(dgram));
    };
};

auto main() -> int {}
