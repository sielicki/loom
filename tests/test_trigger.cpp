// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/trigger.hpp>
#include <loom/loom.hpp>

#include "ut.hpp"

using namespace boost::ut;
using namespace boost::ut::literals;

suite trigger_types_suite = [] {
    "event_type enum values"_test = [] {
        static_assert(static_cast<std::uint32_t>(loom::trigger::event_type::threshold) ==
                      FI_TRIGGER_THRESHOLD);
        static_assert(static_cast<std::uint32_t>(loom::trigger::event_type::xpu) == FI_TRIGGER_XPU);
    };

    "op_type enum values"_test = [] {
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
    };

    "threshold_condition construction"_test = [] {
        loom::trigger::threshold_condition cond{};
        expect(cond.cntr == nullptr);
        expect(cond.threshold == 0_u);

        cond.threshold = 5;
        expect(cond.threshold == 5_u);
    };
};

namespace {

struct my_triggered_context : loom::triggered_context<my_triggered_context> {
    int request_id{0};
    void* user_data{nullptr};
};

}  // namespace

suite triggered_context_suite = [] {
    "triggered_context size"_test = [] {
        static_assert(sizeof(my_triggered_context) >= sizeof(::fi_triggered_context2));
    };

    "triggered_context raw pointer round-trip"_test = [] {
        my_triggered_context ctx;
        ctx.request_id = 123;
        ctx.user_data = reinterpret_cast<void*>(0xDEADBEEF);

        void* raw = ctx.raw();
        expect(raw == static_cast<void*>(&ctx));

        auto* recovered = my_triggered_context::from_raw(raw);
        expect(recovered == &ctx);
        expect(recovered->request_id == 123_i);
        expect(recovered->user_data == reinterpret_cast<void*>(0xDEADBEEF));
    };

    "triggered_context fi_context_ptr"_test = [] {
        my_triggered_context ctx;
        auto* fi_ptr = ctx.fi_context_ptr();
        expect(fi_ptr == &ctx.trig_ctx);
        expect(static_cast<void*>(fi_ptr) == static_cast<void*>(&ctx));
    };

    "structured_triggered_context concept satisfaction"_test = [] {
        static_assert(loom::structured_triggered_context<my_triggered_context>);
    };
};

suite deferred_work_suite = [] {
    "deferred_work default construction"_test = [] {
        loom::trigger::deferred_work work;
        expect(!work.impl_valid());
        expect(!work.is_pending());
        expect(work.impl_internal_ptr() == nullptr);
    };

    "deferred_work move semantics"_test = [] {
        loom::trigger::deferred_work work1;
        loom::trigger::deferred_work work2 = std::move(work1);
        expect(!work2.impl_valid());
    };
};

auto main() -> int {
    return 0;
}
