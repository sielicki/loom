// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace loom;

namespace {
struct atomic_provider {
    std::uint64_t remote_value{42};
    const void* operand{};
    const void* comparison{};
    void* operand_descriptor{};
    void* comparison_descriptor{};
    void* result_descriptor{};
    void* context{};
    bool query_supported{true};
    bool received_count{};

    static auto query(fid_ep* ep, fi_datatype, fi_op, std::size_t* count) -> int {
        auto& self = *static_cast<atomic_provider*>(ep->fid.context);
        self.received_count = count != nullptr;
        if (!count) {
            return -FI_EINVAL;
        }
        if (!self.query_supported) {
            return -FI_EOPNOTSUPP;
        }
        *count = 16;
        return 0;
    }

    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_atomic atomics{
        .size = sizeof(fi_ops_atomic),
        .compwrite = [](fid_ep* ep,
                        const void* buf,
                        std::size_t count,
                        void* desc,
                        const void* compare,
                        void* compare_desc,
                        void* result,
                        void* result_desc,
                        fi_addr_t destination,
                        std::uint64_t address,
                        std::uint64_t key,
                        fi_datatype datatype,
                        fi_op operation,
                        void* context) -> ssize_t {
            REQUIRE(count == 1);
            REQUIRE(datatype == FI_UINT64);
            REQUIRE(operation == FI_CSWAP);
            REQUIRE(destination == FI_ADDR_UNSPEC);
            REQUIRE(address == 0x1000);
            REQUIRE(key == 0x1234);
            auto& self = *static_cast<atomic_provider*>(ep->fid.context);
            self.operand = buf;
            self.comparison = compare;
            self.operand_descriptor = desc;
            self.comparison_descriptor = compare_desc;
            self.result_descriptor = result_desc;
            self.context = context;
            *static_cast<std::uint64_t*>(result) = self.remote_value;
            if (self.remote_value == *static_cast<const std::uint64_t*>(compare)) {
                self.remote_value = *static_cast<const std::uint64_t*>(buf);
            }
            return 0;
        },
        .writevalid = query,
        .readwritevalid = query,
        .compwritevalid = query,
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .atomic = &atomics};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{detail::make_pmr_unique<detail::endpoint_impl>(
            get_default_resource(), &handle, endpoint_types::msg, address_format::inet)};
    }
};
}  // namespace

TEST_CASE("atomic capability queries allow an omitted count output", "[atomic]") {
    const auto kind = GENERATE(0, 1, 2);
    const auto output = GENERATE(false, true);
    const auto supported = GENERATE(false, true);
    CAPTURE(kind, output, supported);
    atomic_provider native;
    native.query_supported = supported;
    auto ep = native.endpoint();
    std::size_t count{};
    constexpr std::array queries{
        &atomic::is_valid, &atomic::is_fetch_valid, &atomic::is_compare_valid};
    const auto op = kind == 2 ? atomic::operation::compare_swap : atomic::operation::sum;
    REQUIRE(queries[kind](ep, op, atomic::datatype::uint64, output ? &count : nullptr) ==
            supported);
    REQUIRE(native.received_count);
    if (output && supported) {
        REQUIRE(count == 16);
    }
}

TEST_CASE("atomic compare and swap preserves operand roles", "[atomic]") {
    auto expected = GENERATE(std::uint64_t{42}, std::uint64_t{84});
    const auto typed = GENERATE(false, true);
    CAPTURE(expected, typed);
    atomic_provider native;
    auto ep = native.endpoint();
    const std::uint64_t desired{84};
    std::uint64_t previous{};
    fid_mr compare_handle{.fid = {.ops = &native.ops}, .mem_desc = &compare_handle};
    fid_mr result_handle{.fid = {.ops = &native.ops}, .mem_desc = &result_handle};
    memory_region compare_mr{detail::make_pmr_unique<detail::memory_region_impl>(
        get_default_resource(), &compare_handle, &expected, sizeof(expected))};
    memory_region result_mr{detail::make_pmr_unique<detail::memory_region_impl>(
        get_default_resource(), &result_handle, &previous, sizeof(previous))};
    const remote_memory remote{0x1000, 0x1234, sizeof(previous)};
    fi_context2 context{};
    const auto result =
        typed ? atomic::cas(ep, expected, desired, &previous, result_mr, remote, &context)
              : atomic::compare_swap(ep,
                                     atomic::datatype::uint64,
                                     &expected,
                                     &desired,
                                     &previous,
                                     1,
                                     &compare_mr,
                                     &result_mr,
                                     remote,
                                     &context);
    REQUIRE(result);
    REQUIRE(previous == 42);
    REQUIRE(native.remote_value == (expected == 42 ? desired : 42));
    REQUIRE(native.operand == &desired);
    REQUIRE(native.comparison == &expected);
    REQUIRE(native.operand_descriptor == nullptr);
    REQUIRE(native.comparison_descriptor == (typed ? nullptr : &compare_handle));
    REQUIRE(native.result_descriptor == &result_handle);
    REQUIRE(native.context == &context);
}

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

TEST_CASE("atomic operations reject empty endpoints", "[atomic]") {
    endpoint ep;
    memory_region mr;
    std::uint64_t value{1};
    std::uint64_t replacement{2};
    std::uint64_t previous{};
    const remote_memory remote{0x1000, 0x1234, sizeof(value)};
    const auto op = atomic::operation::sum;
    const auto type = atomic::datatype::uint64;
    const std::array results{
        atomic::execute(ep, op, type, &value, 1, nullptr, remote),
        atomic::inject(ep, op, type, &value, 1, remote),
        atomic::fetch(ep, op, type, &value, &previous, 1, nullptr, nullptr, remote),
        atomic::compare_swap(
            ep, type, &value, &replacement, &previous, 1, nullptr, nullptr, remote),
        atomic::add(ep, value, remote),
        atomic::fetch_add(ep, value, &previous, mr, remote),
        atomic::cas(ep, value, replacement, &previous, mr, remote),
    };
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == errc::invalid_argument);
    }
    REQUIRE_FALSE(atomic::is_valid(ep, op, type));
    REQUIRE_FALSE(atomic::is_fetch_valid(ep, op, type));
    REQUIRE_FALSE(atomic::is_compare_valid(ep, atomic::operation::compare_swap, type));
}

TEST_CASE("atomic operations reject unavailable provider entry points", "[atomic]") {
    const auto missing_table = GENERATE(false, true);
    atomic_provider native;
    fi_ops_atomic unsupported{.size = sizeof(fi_ops_atomic)};
    native.handle.atomic = missing_table ? nullptr : &unsupported;
    auto ep = native.endpoint();
    memory_region mr;
    const std::uint64_t value{1};
    const std::uint64_t replacement{2};
    std::uint64_t previous{};
    const remote_memory remote{0x1000, 0x1234, sizeof(value)};
    const auto op = atomic::operation::sum;
    const auto type = atomic::datatype::uint64;
    const std::array results{
        atomic::execute(ep, op, type, &value, 1, nullptr, remote),
        atomic::inject(ep, op, type, &value, 1, remote),
        atomic::fetch(ep, op, type, &value, &previous, 1, nullptr, nullptr, remote),
        atomic::compare_swap(
            ep, type, &value, &replacement, &previous, 1, nullptr, nullptr, remote),
        atomic::add(ep, value, remote),
        atomic::fetch_add(ep, value, &previous, mr, remote),
        atomic::cas(ep, value, replacement, &previous, mr, remote),
    };
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == errc::not_supported);
    }
    REQUIRE_FALSE(atomic::is_valid(ep, op, type));
    REQUIRE_FALSE(atomic::is_fetch_valid(ep, op, type));
    REQUIRE_FALSE(atomic::is_compare_valid(ep, atomic::operation::compare_swap, type));
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
