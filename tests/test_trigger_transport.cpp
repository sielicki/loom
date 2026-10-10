// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <cstdlib>
#include <exception>

#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct trigger_provider {
    int submissions{};
    int cancellations{};
    int cancel_result{};
    fi_deferred_work* pending{};
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
        .control =
            [](fid* domain, int command, void* argument) {
                auto& self = *static_cast<trigger_provider*>(domain->context);
                if (command == FI_CANCEL_WORK) {
                    ++self.cancellations;
                    if (self.cancel_result != 0) {
                        return self.cancel_result;
                    }
                    self.pending = nullptr;
                    return 0;
                }
                REQUIRE(command == FI_QUEUE_WORK);
                ++self.submissions;
                self.pending = static_cast<fi_deferred_work*>(argument);
                return 0;
            },
    };
    static auto submit(fid_ep* ep) -> ssize_t {
        ++static_cast<trigger_provider*>(ep->fid.context)->submissions;
        return 0;
    }
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvmsg = [](fid_ep* ep, const fi_msg*, std::uint64_t) { return submit(ep); },
        .sendmsg = [](fid_ep* ep, const fi_msg*, std::uint64_t) { return submit(ep); },
    };
    fi_ops_tagged tags{
        .size = sizeof(fi_ops_tagged),
        .recvmsg = [](fid_ep* ep, const fi_msg_tagged*, std::uint64_t) { return submit(ep); },
        .sendmsg = [](fid_ep* ep, const fi_msg_tagged*, std::uint64_t) { return submit(ep); },
    };
    fi_ops_rma rma{
        .size = sizeof(fi_ops_rma),
        .readmsg = [](fid_ep* ep, const fi_msg_rma*, std::uint64_t) { return submit(ep); },
        .writemsg = [](fid_ep* ep, const fi_msg_rma*, std::uint64_t) { return submit(ep); },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &ops}};
    fid_ep endpoint_handle{
        .fid = {.context = this, .ops = &ops}, .msg = &messages, .rma = &rma, .tagged = &tags};
    fid_cntr counter_handle{.fid = {.context = this, .ops = &ops}};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &domain_handle)};
    loom::endpoint endpoint{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &endpoint_handle,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet)};
    loom::counter counter{loom::detail::make_pmr_unique<loom::detail::counter_impl>(
        loom::get_default_resource(), &counter_handle)};

    auto queue(int operation, loom::counter* trigger, loom::counter* completion)
        -> loom::result<loom::trigger::deferred_work> {
        using loom::trigger::deferred_work;
        const loom::trigger::threshold_condition condition{trigger, 7};
        switch (operation) {
            case 0:
                return deferred_work::create_send(domain, endpoint, condition, completion, {});
            case 1:
                return deferred_work::create_recv(domain, endpoint, condition, completion, {});
            case 2:
                return deferred_work::create_tagged_send(
                    domain, endpoint, condition, completion, {});
            case 3:
                return deferred_work::create_tagged_recv(
                    domain, endpoint, condition, completion, {});
            case 4:
                return deferred_work::create_read(
                    domain, endpoint, condition, completion, {}, {}, {});
            case 5:
                return deferred_work::create_write(
                    domain, endpoint, condition, completion, {}, {}, {});
            case 6:
                return deferred_work::create_counter_set(domain, condition, counter, 1);
            default:
                return deferred_work::create_counter_add(domain, condition, counter, 1);
        }
    }
};

struct trigger_context : loom::triggered_context<trigger_context> {};
}  // namespace

TEST_CASE("pending work cannot be freed after cancellation fails", "[trigger]") {
    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::set_terminate([] { std::_Exit(71); });
        {
            trigger_provider native;
            native.cancel_result = -FI_ENOSYS;
            auto work = native.queue(6, &native.counter, nullptr);
            if (!work) {
                std::_Exit(72);
            }
        }
        std::_Exit(0);
    }
    int status{};
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 71);
}

TEST_CASE("failed cancellation preserves deferred work until completion", "[trigger]") {
    trigger_provider native;
    {
        auto work = native.queue(6, &native.counter, nullptr);
        REQUIRE(work);
        auto* pending = native.pending;
        native.cancel_result = -FI_ENOSYS;
        REQUIRE_FALSE(work->cancel());
        REQUIRE(work->is_pending());
        REQUIRE(native.pending == pending);
        REQUIRE(pending->op.cntr->cntr == &native.counter_handle);
        REQUIRE(pending->op.cntr->value == 1);

        // Simulate the provider completing the operation and releasing the descriptor.
        native.pending = nullptr;
        work->mark_completed();
        REQUIRE_FALSE(work->is_pending());
    }
    REQUIRE(native.cancellations == 1);
}

TEST_CASE("successful cancellation retires deferred work", "[trigger]") {
    trigger_provider native;
    {
        auto work = native.queue(7, &native.counter, nullptr);
        REQUIRE(work);
        REQUIRE(work->cancel());
        REQUIRE_FALSE(work->is_pending());
        REQUIRE(native.pending == nullptr);
    }
    REQUIRE(native.cancellations == 1);
}

TEST_CASE("acknowledged completion survives deferred work moves", "[trigger]") {
    trigger_provider native;
    {
        auto work = native.queue(6, &native.counter, nullptr);
        REQUIRE(work);
        native.pending = nullptr;
        work->mark_completed();
        auto moved = std::move(*work);
        REQUIRE_FALSE(moved.is_pending());
        REQUIRE_FALSE(work->is_pending());
    }
    REQUIRE(native.cancellations == 0);
}

TEST_CASE("deferred work rejects invalid trigger counters", "[trigger]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const bool missing = GENERATE(false, true);
    CAPTURE(operation, missing);
    trigger_provider native;
    loom::counter empty;
    const auto result = native.queue(operation, missing ? nullptr : &empty, &native.counter);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(native.submissions == 0);
}

TEST_CASE("deferred work validates optional completion counters", "[trigger]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(operation);
    trigger_provider native;
    loom::counter empty;
    SECTION("empty wrapper") {
        const auto result = native.queue(operation, &native.counter, &empty);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == loom::errc::invalid_argument);
        REQUIRE(native.submissions == 0);
    }
    SECTION("omitted counter") {
        const auto result = native.queue(operation, &native.counter, nullptr);
        REQUIRE(result);
        REQUIRE(native.pending->completion_cntr == nullptr);
        REQUIRE(native.pending->triggering_cntr == &native.counter_handle);
    }
    SECTION("live counter") {
        const auto result = native.queue(operation, &native.counter, &native.counter);
        REQUIRE(result);
        REQUIRE(native.pending->completion_cntr == &native.counter_handle);
        REQUIRE(native.pending->threshold == 7);
    }
}

TEST_CASE("triggered submissions reject empty counter handles", "[trigger]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(operation);
    trigger_provider native;
    loom::counter empty;
    trigger_context context;
    context.set_threshold_trigger(&native.counter_handle, 99);
    const loom::trigger::threshold_condition condition{&empty, 7};
    const auto result = [&]() -> loom::void_result {
        using namespace loom;
        memory_region memory;
        switch (operation) {
            case 0:
                return trigger::sendmsg(native.endpoint,
                                        msg::send_message<trigger_context*>{.context = &context},
                                        condition);
            case 1:
                return trigger::recvmsg(native.endpoint,
                                        msg::recv_message<trigger_context*>{.context = &context},
                                        condition);
            case 2:
                return trigger::tagged_sendmsg(
                    native.endpoint,
                    msg::tagged_send_message<trigger_context*>{.context = &context},
                    condition);
            case 3:
                return trigger::tagged_recvmsg(
                    native.endpoint,
                    msg::tagged_recv_message<trigger_context*>{.context = &context},
                    condition);
            case 4:
                return trigger::read(native.endpoint, {}, memory, {}, &context, condition);
            default:
                return trigger::write(native.endpoint, {}, memory, {}, &context, condition);
        }
    }();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(native.submissions == 0);
    REQUIRE(context.trig_ctx.trigger.threshold.cntr == &native.counter_handle);
    REQUIRE(context.trig_ctx.trigger.threshold.threshold == 99);
}
