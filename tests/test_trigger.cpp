// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/trigger.hpp>
#include <loom/loom.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("event_type enum values", "[trigger]") {
    static_assert(static_cast<std::uint32_t>(loom::trigger::event_type::threshold) ==
                  FI_TRIGGER_THRESHOLD);
    static_assert(static_cast<std::uint32_t>(loom::trigger::event_type::xpu) == FI_TRIGGER_XPU);
}

TEST_CASE("op_type enum values", "[trigger]") {
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::recv) == FI_OP_RECV);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::send) == FI_OP_SEND);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::tagged_recv) ==
                  FI_OP_TRECV);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::tagged_send) ==
                  FI_OP_TSEND);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::read) == FI_OP_READ);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::write) == FI_OP_WRITE);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::atomic) == FI_OP_ATOMIC);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::fetch_atomic) ==
                  FI_OP_FETCH_ATOMIC);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::compare_atomic) ==
                  FI_OP_COMPARE_ATOMIC);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::counter_set) ==
                  FI_OP_CNTR_SET);
    static_assert(static_cast<std::uint32_t>(loom::trigger::op_type::counter_add) ==
                  FI_OP_CNTR_ADD);
}

TEST_CASE("threshold_condition construction", "[trigger]") {
    loom::trigger::threshold_condition cond{};
    REQUIRE(cond.cntr == nullptr);
    REQUIRE(cond.threshold == 0);

    cond.threshold = 5;
    REQUIRE(cond.threshold == 5);
}

namespace {

struct my_triggered_context : loom::triggered_context<my_triggered_context> {
    int request_id{0};
    void* user_data{nullptr};
};

}  // namespace

TEST_CASE("triggered_context size", "[trigger]") {
    static_assert(sizeof(my_triggered_context) >= sizeof(::fi_triggered_context2));
}

TEST_CASE("triggered_context raw pointer round-trip", "[trigger]") {
    my_triggered_context ctx;
    ctx.request_id = 123;
    ctx.user_data = reinterpret_cast<void*>(0xDEADBEEF);

    void* raw = ctx.raw();
    REQUIRE(raw == static_cast<void*>(&ctx));

    auto* recovered = my_triggered_context::from_raw(raw);
    REQUIRE(recovered == &ctx);
    REQUIRE(recovered->request_id == 123);
    REQUIRE(recovered->user_data == reinterpret_cast<void*>(0xDEADBEEF));
}

TEST_CASE("triggered_context fi_context_ptr", "[trigger]") {
    my_triggered_context ctx;
    auto* fi_ptr = ctx.fi_context_ptr();
    REQUIRE(fi_ptr == &ctx.trig_ctx);
    REQUIRE(static_cast<void*>(fi_ptr) == static_cast<void*>(&ctx));
}

TEST_CASE("structured_triggered_context concept satisfaction", "[trigger]") {
    static_assert(loom::structured_triggered_context<my_triggered_context>);
}

TEST_CASE("deferred_work default construction", "[trigger]") {
    loom::trigger::deferred_work work;
    REQUIRE_FALSE(work.impl_valid());
    REQUIRE_FALSE(work.is_pending());
    REQUIRE(work.impl_internal_ptr() == nullptr);
}

TEST_CASE("deferred_work move semantics", "[trigger]") {
    loom::trigger::deferred_work work1;
    loom::trigger::deferred_work work2 = std::move(work1);
    REQUIRE_FALSE(work2.impl_valid());
}
