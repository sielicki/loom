// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <vector>

#include "ut.hpp"

using namespace boost::ut;
using namespace loom;

suite atomic_suite = [] {
    "atomic_type concept"_test = [] {
        static_assert(atomic::atomic_type<int8_t>);
        static_assert(atomic::atomic_type<uint8_t>);
        static_assert(atomic::atomic_type<int16_t>);
        static_assert(atomic::atomic_type<uint16_t>);
        static_assert(atomic::atomic_type<int32_t>);
        static_assert(atomic::atomic_type<uint32_t>);
        static_assert(atomic::atomic_type<int64_t>);
        static_assert(atomic::atomic_type<uint64_t>);
        static_assert(atomic::atomic_type<float>);
        static_assert(atomic::atomic_type<double>);

        static_assert(!atomic::atomic_type<void*>);
        static_assert(!atomic::atomic_type<std::string>);
    };

    "get_datatype consteval"_test = [] {
        static_assert(atomic::get_datatype<int8_t>() == atomic::datatype::int8);
        static_assert(atomic::get_datatype<uint8_t>() == atomic::datatype::uint8);
        static_assert(atomic::get_datatype<int16_t>() == atomic::datatype::int16);
        static_assert(atomic::get_datatype<uint16_t>() == atomic::datatype::uint16);
        static_assert(atomic::get_datatype<int32_t>() == atomic::datatype::int32);
        static_assert(atomic::get_datatype<uint32_t>() == atomic::datatype::uint32);
        static_assert(atomic::get_datatype<int64_t>() == atomic::datatype::int64);
        static_assert(atomic::get_datatype<uint64_t>() == atomic::datatype::uint64);

        static_assert(atomic::get_datatype<float>() == atomic::datatype::float32);
        static_assert(atomic::get_datatype<double>() == atomic::datatype::double64);
    };

    "atomic operation enums"_test = [] {
        auto min_op = atomic::operation::min;
        auto max_op = atomic::operation::max;
        auto sum_op = atomic::operation::sum;
        auto prod_op = atomic::operation::prod;
        auto lor_op = atomic::operation::logical_or;
        auto land_op = atomic::operation::logical_and;
        auto bor_op = atomic::operation::bitwise_or;
        auto band_op = atomic::operation::bitwise_and;
        auto lxor_op = atomic::operation::logical_xor;
        auto bxor_op = atomic::operation::bitwise_xor;
        auto read_op = atomic::operation::atomic_read;
        auto write_op = atomic::operation::atomic_write;
        auto cas_op = atomic::operation::compare_swap;

        expect(min_op != max_op);
        expect(sum_op != prod_op);
        expect(lor_op != land_op);
        expect(bor_op != band_op);
        expect(read_op != write_op);
        expect(lxor_op != bxor_op);
        expect(cas_op != min_op);
    };

    "atomic datatype enums"_test = [] {
        auto i8 = atomic::datatype::int8;
        auto u8 = atomic::datatype::uint8;
        auto i16 = atomic::datatype::int16;
        auto u16 = atomic::datatype::uint16;
        auto i32 = atomic::datatype::int32;
        auto u32 = atomic::datatype::uint32;
        auto i64 = atomic::datatype::int64;
        auto u64 = atomic::datatype::uint64;
        auto f32 = atomic::datatype::float32;
        auto f64 = atomic::datatype::double64;
        auto fc = atomic::datatype::float_complex;
        auto dc = atomic::datatype::double_complex;
        [[maybe_unused]] auto ld = atomic::datatype::long_double;
        [[maybe_unused]] auto ldc = atomic::datatype::long_double_complex;
        auto i128 = atomic::datatype::int128;
        auto u128 = atomic::datatype::uint128;

        expect(i8 != u8);
        expect(i16 != u16);
        expect(i32 != u32);
        expect(i64 != u64);
        expect(f32 != f64);
        expect(fc != dc);
        expect(i128 != u128);
    };

    skip / "atomic API compilation"_test = [] {
        fabric_hints hints{};
        hints.ep_type = endpoint_types::rdm;
        hints.capabilities = capability::atomic;

        auto info_result = query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No atomic providers found";

        auto& info = *info_result;
        auto fab_result = fabric::create(info);
        expect(static_cast<bool>(fab_result)) << "Fabric creation failed";

        auto dom_result = domain::create(*fab_result, info);
        expect(static_cast<bool>(dom_result)) << "Domain creation failed";

        auto ep_result = endpoint::create(*dom_result, info);
        expect(static_cast<bool>(ep_result)) << "Endpoint creation failed";

        auto& ep = *ep_result;

        auto enable_result = ep.enable();
        expect(static_cast<bool>(enable_result)) << "Endpoint enable failed";

        int64_t value = 42;
        int64_t result_val = 0;
        remote_memory remote{0x1000, 0xABCD, sizeof(int64_t)};

        std::vector<std::byte> mr_buf(4096);
        auto mr_result = memory_region::register_memory(
            *dom_result, std::span{mr_buf}, mr_access_flags::read | mr_access_flags::write);

        expect(static_cast<bool>(mr_result)) << "Memory registration failed";

        auto& mr = *mr_result;

        [[maybe_unused]] auto add_result = atomic::add(ep, value, remote, nullptr);
        [[maybe_unused]] auto fetch_result =
            atomic::fetch_add(ep, value, &result_val, mr, remote, nullptr);
        [[maybe_unused]] auto cas_result =
            atomic::cas(ep, value, value + 1, &result_val, mr, remote, nullptr);
    };
};

auto main() -> int {}
