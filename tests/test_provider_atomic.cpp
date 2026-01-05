// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/provider_atomic.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("fetch_add_op has execute_native and execute_staged", "[provider_atomic]") {
    using op_type = loom::fetch_add_op<std::uint64_t>;

    static_assert(requires(op_type op) { op.execute_native(); });
    static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
        op.execute_staged(mr, buf);
    });
}

TEST_CASE("atomic_add_op has execute_native and execute_staged", "[provider_atomic]") {
    using op_type = loom::atomic_add_op<std::uint64_t>;

    static_assert(requires(op_type op) { op.execute_native(); });
    static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
        op.execute_staged(mr, buf);
    });
}

TEST_CASE("compare_swap_op has execute_native and execute_staged", "[provider_atomic]") {
    using op_type = loom::compare_swap_op<std::uint64_t>;

    static_assert(requires(op_type op) { op.execute_native(); });
    static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
        op.execute_staged(mr, buf);
    });
}

TEST_CASE("generic_fetch_op has execute_native and execute_staged", "[provider_atomic]") {
    using op_type = loom::generic_fetch_op<std::uint64_t>;

    static_assert(requires(op_type op) { op.execute_native(); });
    static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
        op.execute_staged(mr, buf);
    });
}

TEST_CASE("provider_atomic_context requires_staging for verbs", "[provider_atomic]") {
    using ctx_type = loom::provider_atomic_context<loom::provider::verbs_tag>;
    static_assert(!ctx_type::requires_staging());
}

TEST_CASE("provider_atomic_context requires_staging for efa", "[provider_atomic]") {
    using ctx_type = loom::provider_atomic_context<loom::provider::efa_tag>;
    static_assert(ctx_type::requires_staging());
}

TEST_CASE("provider_atomic_context requires_staging for slingshot", "[provider_atomic]") {
    using ctx_type = loom::provider_atomic_context<loom::provider::slingshot_tag>;
    static_assert(!ctx_type::requires_staging());
}

TEST_CASE("provider_atomic_context requires_staging for shm", "[provider_atomic]") {
    using ctx_type = loom::provider_atomic_context<loom::provider::shm_tag>;
    static_assert(!ctx_type::requires_staging());
}

TEST_CASE("provider_atomic_context requires_staging for tcp", "[provider_atomic]") {
    using ctx_type = loom::provider_atomic_context<loom::provider::tcp_tag>;
    static_assert(ctx_type::requires_staging());
}

TEST_CASE("perform_local_atomic_op sum", "[provider_atomic]") {
    auto result = loom::detail::perform_local_atomic_op(loom::atomic::operation::sum, 10, 5);
    REQUIRE(result == 15);
}

TEST_CASE("perform_local_atomic_op min", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::min, 10, 5) == 5);
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::min, 3, 8) == 3);
}

TEST_CASE("perform_local_atomic_op max", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::max, 10, 5) == 10);
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::max, 3, 8) == 8);
}

TEST_CASE("perform_local_atomic_op prod", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::prod, 6, 7) == 42);
}

TEST_CASE("perform_local_atomic_op bitwise_or", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(
                loom::atomic::operation::bitwise_or, 0b1010, 0b0101) == 0b1111);
}

TEST_CASE("perform_local_atomic_op bitwise_and", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(
                loom::atomic::operation::bitwise_and, 0b1110, 0b0111) == 0b0110);
}

TEST_CASE("perform_local_atomic_op bitwise_xor", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(
                loom::atomic::operation::bitwise_xor, 0b1010, 0b1100) == 0b0110);
}

TEST_CASE("perform_local_atomic_op atomic_write", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::atomic_write, 100, 42) ==
            42);
}

TEST_CASE("perform_local_atomic_op atomic_read", "[provider_atomic]") {
    REQUIRE(loom::detail::perform_local_atomic_op(loom::atomic::operation::atomic_read, 100, 42) ==
            100);
}

TEST_CASE("operation types work with various atomic types", "[provider_atomic]") {
    static_assert(requires { loom::fetch_add_op<std::int32_t>{}; });
    static_assert(requires { loom::fetch_add_op<std::int64_t>{}; });
    static_assert(requires { loom::fetch_add_op<std::uint32_t>{}; });
    static_assert(requires { loom::fetch_add_op<std::uint64_t>{}; });
    static_assert(requires { loom::fetch_add_op<float>{}; });
    static_assert(requires { loom::fetch_add_op<double>{}; });

    static_assert(requires { loom::compare_swap_op<std::int32_t>{}; });
    static_assert(requires { loom::compare_swap_op<std::uint64_t>{}; });
}

TEST_CASE("native_atomic_provider concept", "[provider_atomic]") {
    static_assert(loom::native_atomic_provider<loom::provider::verbs_tag>);
    static_assert(loom::native_atomic_provider<loom::provider::slingshot_tag>);
    static_assert(loom::native_atomic_provider<loom::provider::shm_tag>);
    static_assert(!loom::native_atomic_provider<loom::provider::efa_tag>);
    static_assert(!loom::native_atomic_provider<loom::provider::tcp_tag>);
}

TEST_CASE("staged_atomic_provider concept", "[provider_atomic]") {
    static_assert(!loom::staged_atomic_provider<loom::provider::verbs_tag>);
    static_assert(!loom::staged_atomic_provider<loom::provider::slingshot_tag>);
    static_assert(!loom::staged_atomic_provider<loom::provider::shm_tag>);
    static_assert(loom::staged_atomic_provider<loom::provider::efa_tag>);
    static_assert(loom::staged_atomic_provider<loom::provider::tcp_tag>);
}
