// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/provider_atomic.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <rdma/fi_rma.h>

#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct atomic_transport {
    std::uint64_t remote_value{42};
    const std::uint64_t* operand{};
    const std::uint64_t* comparison{};
    std::uint64_t* result{};
    fi_op operation{};
    void* context{};
    int atomic_calls{};
    int rma_calls{};
    bool supported{true};

    auto post(const void* value, const void* compare, void* output, fi_op op, void* ctx)
        -> ssize_t {
        ++atomic_calls;
        if (!supported) {
            return -FI_EOPNOTSUPP;
        }
        operand = static_cast<const std::uint64_t*>(value);
        comparison = static_cast<const std::uint64_t*>(compare);
        result = static_cast<std::uint64_t*>(output);
        operation = op;
        context = ctx;
        return 0;
    }

    void complete() {
        if (result) {
            *result = remote_value;
        }
        if (comparison) {
            if (remote_value == *comparison) {
                remote_value = *operand;
            }
        } else if (operation == FI_SUM) {
            remote_value += *operand;
        } else if (operation == FI_PROD) {
            remote_value *= *operand;
        }
    }

    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_atomic atomics{
        .size = sizeof(fi_ops_atomic),
        .write = [](fid_ep* ep,
                    const void* value,
                    std::size_t,
                    void*,
                    fi_addr_t,
                    std::uint64_t,
                    std::uint64_t,
                    fi_datatype,
                    fi_op op,
                    void* ctx) -> ssize_t {
            return static_cast<atomic_transport*>(ep->fid.context)
                ->post(value, nullptr, nullptr, op, ctx);
        },
        .readwrite = [](fid_ep* ep,
                        const void* value,
                        std::size_t,
                        void*,
                        void* result,
                        void*,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op op,
                        void* ctx) -> ssize_t {
            return static_cast<atomic_transport*>(ep->fid.context)
                ->post(value, nullptr, result, op, ctx);
        },
        .compwrite = [](fid_ep* ep,
                        const void* value,
                        std::size_t,
                        void*,
                        const void* compare,
                        void*,
                        void* result,
                        void*,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op op,
                        void* ctx) -> ssize_t {
            return static_cast<atomic_transport*>(ep->fid.context)
                ->post(value, compare, result, op, ctx);
        },
    };
    fi_ops_rma rma{
        .size = sizeof(fi_ops_rma),
        .read = [](fid_ep* ep,
                   void*,
                   std::size_t,
                   void*,
                   fi_addr_t,
                   std::uint64_t,
                   std::uint64_t,
                   void*) -> ssize_t {
            ++static_cast<atomic_transport*>(ep->fid.context)->rma_calls;
            return -FI_EOPNOTSUPP;
        },
        .write = [](fid_ep* ep,
                    const void*,
                    std::size_t,
                    void*,
                    fi_addr_t,
                    std::uint64_t,
                    std::uint64_t,
                    void*) -> ssize_t {
            ++static_cast<atomic_transport*>(ep->fid.context)->rma_calls;
            return -FI_EOPNOTSUPP;
        },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .rma = &rma, .atomic = &atomics};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::msg,
                                                                       loom::address_format::inet)};
    }
};
}  // namespace

TEST_CASE("provider atomics retain provider atomicity guarantees", "[provider_atomic]") {
    const auto kind = GENERATE(0, 1, 2, 3);
    const auto provider = GENERATE(0, 1, 2);
    const auto legacy_staging = GENERATE(false, true);
    const auto supported = GENERATE(false, true);
    CAPTURE(kind, provider, legacy_staging, supported);
    atomic_transport native;
    native.supported = supported;
    auto ep = native.endpoint();
    loom::domain domain;
    loom::memory_region mr;
    std::array<std::byte, 64> staging{};
    std::uint64_t previous{};
    int user_context{};
    const loom::remote_memory remote{0x1000, 1, sizeof(previous)};
    auto run = [&]<typename Provider>() {
        loom::provider_atomic_context<Provider> context{domain};
        auto invoke = [&](const auto& operation) {
            auto submitted =
                legacy_staging ? operation.execute_staged(mr, staging) : context.execute(operation);
            REQUIRE(native.rma_calls == 0);
            REQUIRE(native.atomic_calls == 1);
            REQUIRE(static_cast<bool>(submitted) == supported);
            if (!supported) {
                REQUIRE(submitted.error() == loom::errc::not_supported);
                return;
            }
            REQUIRE(native.context == &user_context);
            REQUIRE(native.remote_value == 42);
            REQUIRE(previous == 0);
            native.complete();
            REQUIRE(native.remote_value == (kind < 2 ? 44 : 84));
            REQUIRE(previous == (kind == 0 ? 0 : 42));
        };
        switch (kind) {
            case 0:
                invoke(loom::atomic_add_op<std::uint64_t>{&ep, 2, nullptr, remote, &user_context});
                break;
            case 1:
                invoke(loom::fetch_add_op<std::uint64_t>{
                    &ep, 2, &previous, &mr, remote, &user_context});
                break;
            case 2:
                invoke(loom::compare_swap_op<std::uint64_t>{
                    &ep, 42, 84, &previous, &mr, remote, &user_context});
                break;
            case 3:
                invoke(loom::generic_fetch_op<std::uint64_t>{
                    &ep, loom::atomic::operation::prod, 2, &previous, &mr, remote, &user_context});
                break;
        }
    };
    switch (provider) {
        case 0:
            run.template operator()<loom::provider::verbs_tag>();
            break;
        case 1:
            run.template operator()<loom::provider::efa_tag>();
            break;
        case 2:
            run.template operator()<loom::provider::tcp_tag>();
            break;
    }
}

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
    static_assert(!ctx_type::requires_staging());
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
    static_assert(!ctx_type::requires_staging());
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
