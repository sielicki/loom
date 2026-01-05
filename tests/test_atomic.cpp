// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace loom;

TEST_CASE("atomic_type concept", "[atomic]") {
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
}

TEST_CASE("get_datatype consteval", "[atomic]") {
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
}

TEST_CASE("atomic operation enums", "[atomic]") {
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

    REQUIRE(min_op != max_op);
    REQUIRE(sum_op != prod_op);
    REQUIRE(lor_op != land_op);
    REQUIRE(bor_op != band_op);
    REQUIRE(read_op != write_op);
    REQUIRE(lxor_op != bxor_op);
    REQUIRE(cas_op != min_op);
}

TEST_CASE("atomic datatype enums", "[atomic]") {
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

    REQUIRE(i8 != u8);
    REQUIRE(i16 != u16);
    REQUIRE(i32 != u32);
    REQUIRE(i64 != u64);
    REQUIRE(f32 != f64);
    REQUIRE(fc != dc);
    REQUIRE(i128 != u128);
}

TEST_CASE("atomic API compilation", "[atomic][.provider]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::atomic;

    auto info_result = query_fabric(hints);
    if (!info_result) {
        SKIP("No atomic providers found");
    }

    auto& info = *info_result;
    auto fab_result = fabric::create(info);
    REQUIRE(static_cast<bool>(fab_result));

    auto dom_result = domain::create(*fab_result, info);
    REQUIRE(static_cast<bool>(dom_result));

    auto ep_result = endpoint::create(*dom_result, info);
    REQUIRE(static_cast<bool>(ep_result));

    auto& ep = *ep_result;

    auto enable_result = ep.enable();
    REQUIRE(static_cast<bool>(enable_result));

    int64_t value = 42;
    int64_t result_val = 0;
    remote_memory remote{0x1000, 0xABCD, sizeof(int64_t)};

    std::vector<std::byte> mr_buf(4096);
    auto mr_result = memory_region::register_memory(
        *dom_result, std::span{mr_buf}, mr_access_flags::read | mr_access_flags::write);

    REQUIRE(static_cast<bool>(mr_result));

    auto& mr = *mr_result;

    [[maybe_unused]] auto add_result = atomic::add(ep, value, remote, nullptr);
    [[maybe_unused]] auto fetch_result =
        atomic::fetch_add(ep, value, &result_val, mr, remote, nullptr);
    [[maybe_unused]] auto cas_result =
        atomic::cas(ep, value, value + 1, &result_val, mr, remote, nullptr);
}
