// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/provider_atomic.hpp>

#include "ut.hpp"

using namespace boost::ut;

suite provider_atomic_suite = [] {
    "fetch_add_op has execute_native and execute_staged"_test = [] {
        using op_type = loom::fetch_add_op<std::uint64_t>;

        static_assert(requires(op_type op) { op.execute_native(); });
        static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
            op.execute_staged(mr, buf);
        });
    };

    "atomic_add_op has execute_native and execute_staged"_test = [] {
        using op_type = loom::atomic_add_op<std::uint64_t>;

        static_assert(requires(op_type op) { op.execute_native(); });
        static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
            op.execute_staged(mr, buf);
        });
    };

    "compare_swap_op has execute_native and execute_staged"_test = [] {
        using op_type = loom::compare_swap_op<std::uint64_t>;

        static_assert(requires(op_type op) { op.execute_native(); });
        static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
            op.execute_staged(mr, buf);
        });
    };

    "generic_fetch_op has execute_native and execute_staged"_test = [] {
        using op_type = loom::generic_fetch_op<std::uint64_t>;

        static_assert(requires(op_type op) { op.execute_native(); });
        static_assert(requires(op_type op, loom::memory_region& mr, std::span<std::byte> buf) {
            op.execute_staged(mr, buf);
        });
    };

    "provider_atomic_context requires_staging for verbs"_test = [] {
        using ctx_type = loom::provider_atomic_context<loom::provider::verbs_tag>;
        static_assert(!ctx_type::requires_staging());
    };

    "provider_atomic_context requires_staging for efa"_test = [] {
        using ctx_type = loom::provider_atomic_context<loom::provider::efa_tag>;
        static_assert(ctx_type::requires_staging());
    };

    "provider_atomic_context requires_staging for slingshot"_test = [] {
        using ctx_type = loom::provider_atomic_context<loom::provider::slingshot_tag>;
        static_assert(!ctx_type::requires_staging());
    };

    "provider_atomic_context requires_staging for shm"_test = [] {
        using ctx_type = loom::provider_atomic_context<loom::provider::shm_tag>;
        static_assert(!ctx_type::requires_staging());
    };

    "provider_atomic_context requires_staging for tcp"_test = [] {
        using ctx_type = loom::provider_atomic_context<loom::provider::tcp_tag>;
        static_assert(ctx_type::requires_staging());
    };

    "perform_local_atomic_op sum"_test = [] {
        auto result = loom::detail::perform_local_atomic_op(loom::atomic::operation::sum, 10, 5);
        expect(result == 15);
    };

    "perform_local_atomic_op min"_test = [] {
        expect(loom::detail::perform_local_atomic_op(loom::atomic::operation::min, 10, 5) == 5);
        expect(loom::detail::perform_local_atomic_op(loom::atomic::operation::min, 3, 8) == 3);
    };

    "perform_local_atomic_op max"_test = [] {
        expect(loom::detail::perform_local_atomic_op(loom::atomic::operation::max, 10, 5) == 10);
        expect(loom::detail::perform_local_atomic_op(loom::atomic::operation::max, 3, 8) == 8);
    };

    "perform_local_atomic_op prod"_test = [] {
        expect(loom::detail::perform_local_atomic_op(loom::atomic::operation::prod, 6, 7) == 42);
    };

    "perform_local_atomic_op bitwise_or"_test = [] {
        expect(loom::detail::perform_local_atomic_op(
                   loom::atomic::operation::bitwise_or, 0b1010, 0b0101) == 0b1111);
    };

    "perform_local_atomic_op bitwise_and"_test = [] {
        expect(loom::detail::perform_local_atomic_op(
                   loom::atomic::operation::bitwise_and, 0b1110, 0b0111) == 0b0110);
    };

    "perform_local_atomic_op bitwise_xor"_test = [] {
        expect(loom::detail::perform_local_atomic_op(
                   loom::atomic::operation::bitwise_xor, 0b1010, 0b1100) == 0b0110);
    };

    "perform_local_atomic_op atomic_write"_test = [] {
        expect(loom::detail::perform_local_atomic_op(
                   loom::atomic::operation::atomic_write, 100, 42) == 42);
    };

    "perform_local_atomic_op atomic_read"_test = [] {
        expect(loom::detail::perform_local_atomic_op(
                   loom::atomic::operation::atomic_read, 100, 42) == 100);
    };

    "operation types work with various atomic types"_test = [] {
        static_assert(requires { loom::fetch_add_op<std::int32_t>{}; });
        static_assert(requires { loom::fetch_add_op<std::int64_t>{}; });
        static_assert(requires { loom::fetch_add_op<std::uint32_t>{}; });
        static_assert(requires { loom::fetch_add_op<std::uint64_t>{}; });
        static_assert(requires { loom::fetch_add_op<float>{}; });
        static_assert(requires { loom::fetch_add_op<double>{}; });

        static_assert(requires { loom::compare_swap_op<std::int32_t>{}; });
        static_assert(requires { loom::compare_swap_op<std::uint64_t>{}; });
    };

    "native_atomic_provider concept"_test = [] {
        static_assert(loom::native_atomic_provider<loom::provider::verbs_tag>);
        static_assert(loom::native_atomic_provider<loom::provider::slingshot_tag>);
        static_assert(loom::native_atomic_provider<loom::provider::shm_tag>);
        static_assert(!loom::native_atomic_provider<loom::provider::efa_tag>);
        static_assert(!loom::native_atomic_provider<loom::provider::tcp_tag>);
    };

    "staged_atomic_provider concept"_test = [] {
        static_assert(!loom::staged_atomic_provider<loom::provider::verbs_tag>);
        static_assert(!loom::staged_atomic_provider<loom::provider::slingshot_tag>);
        static_assert(!loom::staged_atomic_provider<loom::provider::shm_tag>);
        static_assert(loom::staged_atomic_provider<loom::provider::efa_tag>);
        static_assert(loom::staged_atomic_provider<loom::provider::tcp_tag>);
    };
};

auto main() -> int {}
