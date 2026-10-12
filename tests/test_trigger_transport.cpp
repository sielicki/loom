// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <cstdlib>
#include <exception>
#include <limits>

#include <sys/wait.h>
#include <unistd.h>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct trigger_provider {
    int submissions{};
    int cancellations{};
    int cancel_result{};
    bool require_work_context{};
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
                const auto* work = static_cast<fi_deferred_work*>(argument);
                if (self.require_work_context) {
                    const auto* context = [work]() -> void* {
                        switch (work->op_type) {
                            case FI_OP_SEND:
                            case FI_OP_RECV:
                                return work->op.msg->msg.context;
                            case FI_OP_TSEND:
                            case FI_OP_TRECV:
                                return work->op.tagged->msg.context;
                            default:
                                return work->op.rma->msg.context;
                        }
                    }();
                    if (context != &work->context) {
                        return -FI_EINVAL;
                    }
                }
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
    loom::endpoint endpoint;
    explicit trigger_provider(fid_domain* origin = nullptr,
                              loom::endpoint_iov_limits limits = {64, 64, 64})
        : endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
              loom::get_default_resource(),
              &endpoint_handle,
              loom::endpoint_types::rdm,
              loom::address_format::inet,
              0UZ,
              0UZ,
              loom::caps{},
              0,
              0,
              origin ? origin : &domain_handle,
              limits)} {}
    loom::counter counter{loom::detail::make_pmr_unique<loom::detail::counter_impl>(
        loom::get_default_resource(), &counter_handle)};

    auto queue(int operation, loom::counter* trigger, loom::counter* completion)
        -> loom::result<loom::trigger::deferred_work> {
        using loom::trigger::deferred_work;
        const loom::trigger::threshold_condition condition{trigger, 7};
        const std::array remote{loom::rma::rma_iov{}};
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
                    domain, endpoint, condition, completion, {}, {}, remote);
            case 5:
                return deferred_work::create_write(
                    domain, endpoint, condition, completion, {}, {}, remote);
            case 6:
                return deferred_work::create_counter_set(domain, condition, counter, 1);
            default:
                return deferred_work::create_counter_add(domain, condition, counter, 1);
        }
    }
};

struct trigger_context : loom::triggered_context<trigger_context> {};
}  // namespace

TEST_CASE("deferred work rejects unavailable native control", "[trigger]") {
    const auto missing = GENERATE(0, 1, 2);
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    CAPTURE(missing, operation);
    trigger_provider native;
    const auto saved = native.ops;
    if (missing == 0)
        native.domain_handle.fid.ops = nullptr;
    else if (missing == 1)
        native.ops.control = nullptr;
    else
        native.ops.size = offsetof(fi_ops, control);
    const auto result = [&]() -> loom::void_result {
        if (operation == 8)
            return loom::trigger::flush_work(native.domain);
        if (operation == 9)
            return loom::trigger::flush_work(native.domain, native.counter);
        return native.queue(operation, &native.counter, nullptr).transform([](auto&&) {});
    }();
    native.ops = saved;
    native.domain_handle.fid.ops = &native.ops;
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::not_supported);
    REQUIRE(native.submissions == 0);
    REQUIRE(native.pending == nullptr);
}

TEST_CASE("unavailable native cancellation retains deferred work", "[trigger]") {
    const auto missing = GENERATE(0, 1, 2);
    trigger_provider native;
    auto work = native.queue(6, &native.counter, nullptr);
    REQUIRE(work);
    auto* pending = native.pending;
    const auto saved = native.ops;
    if (missing == 0)
        native.domain_handle.fid.ops = nullptr;
    else if (missing == 1)
        native.ops.control = nullptr;
    else
        native.ops.size = offsetof(fi_ops, control);
    const auto result = work->cancel();
    native.ops = saved;
    native.domain_handle.fid.ops = &native.ops;
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::not_supported);
    REQUIRE(work->is_pending());
    REQUIRE(native.pending == pending);
    REQUIRE(native.cancellations == 0);
    REQUIRE(work->cancel());
    REQUIRE_FALSE(work->is_pending());
    REQUIRE(native.pending == nullptr);
    REQUIRE(native.cancellations == 1);
}

TEST_CASE("deferred RMA enforces negotiated IOV counts", "[trigger][iov]") {
    using namespace loom;
    const auto write = GENERATE(false, true);
    const auto tx_limit = GENERATE(0UZ, 2UZ, 17UZ);
    const auto remote_limit = GENERATE(0UZ, 3UZ, 17UZ);
    const auto local_count = GENERATE(0UZ, 1UZ, 2UZ, 3UZ, 16UZ, 17UZ, 18UZ);
    const auto remote_count = GENERATE(0UZ, 1UZ, 2UZ, 3UZ, 4UZ, 16UZ, 17UZ, 18UZ);
    CAPTURE(write, tx_limit, remote_limit, local_count, remote_count);
    trigger_provider native{nullptr, {tx_limit, 64, remote_limit}};
    std::array<iovec, 18> local{};
    std::array<rma::rma_iov, 18> remote{};
    const auto local_iov = std::span{local}.first(local_count);
    const auto remote_iov = std::span{remote}.first(remote_count);
    const trigger::threshold_condition condition{&native.counter, 7};
    auto work =
        write ? trigger::deferred_work::create_write(
                    native.domain, native.endpoint, condition, nullptr, local_iov, {}, remote_iov)
              : trigger::deferred_work::create_read(
                    native.domain, native.endpoint, condition, nullptr, local_iov, {}, remote_iov);
    const bool valid = remote_count != 0 && local_count <= tx_limit && remote_count <= remote_limit;
    CHECK(native.submissions == (valid ? 1 : 0));
    if (valid) {
        REQUIRE(work);
        REQUIRE(native.pending);
        CHECK(native.pending->op.rma->msg.iov_count == local_count);
        CHECK(native.pending->op.rma->msg.rma_iov_count == remote_count);
        REQUIRE(work->cancel());
    } else {
        REQUIRE_FALSE(work);
        CHECK(work.error() == errc::invalid_argument);
        CHECK(native.pending == nullptr);
        CHECK(native.cancellations == 0);
    }
}

TEST_CASE("deferred RMA validates vector ranges before queuing work", "[trigger][bounds]") {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    constexpr auto address_max = std::numeric_limits<std::uint64_t>::max();
    struct transfer {
        std::array<std::size_t, 2> local, remote;
        std::uint64_t last_address;
        bool valid;
    };
    constexpr std::array cases{
        transfer{{3, 5}, {1, 7}, 0x1000, true},
        transfer{{0, 0}, {0, 0}, address_max, true},
        transfer{{3, 5}, {1, 6}, 0x1000, false},
        transfer{{3, 5}, {1, 8}, 0x1000, false},
        transfer{{maximum, 1}, {0, 0}, 0, false},
        transfer{{0, 0}, {maximum, 1}, 0, false},
        transfer{{maximum, 1}, {maximum, 1}, 0, false},
        transfer{{3, 5}, {1, 7}, address_max - 6, false},
    };
    const auto write = GENERATE(false, true);
    for (const auto& test : cases) {
        CAPTURE(write, test.local, test.remote, test.last_address);
        trigger_provider native;
        std::byte storage{};
        const std::array local{iovec{&storage, test.local[0]}, iovec{&storage, test.local[1]}};
        const std::array remote{loom::rma::rma_iov{0, test.remote[0], 1},
                                loom::rma::rma_iov{test.last_address, test.remote[1], 2}};
        const loom::trigger::threshold_condition trigger{&native.counter, 7};
        auto work = write
                        ? loom::trigger::deferred_work::create_write(
                              native.domain, native.endpoint, trigger, nullptr, local, {}, remote)
                        : loom::trigger::deferred_work::create_read(
                              native.domain, native.endpoint, trigger, nullptr, local, {}, remote);
        REQUIRE(static_cast<bool>(work) == test.valid);
        if (test.valid) {
            REQUIRE(native.submissions == 1);
            REQUIRE(work->cancel());
        } else {
            REQUIRE(work.error() == loom::errc::invalid_argument);
            REQUIRE(native.submissions == 0);
            REQUIRE(native.pending == nullptr);
            REQUIRE(native.cancellations == 0);
        }
    }
}

TEST_CASE("triggered RMA enforces remote bounds before changing its context", "[trigger][bounds]") {
    const auto write = GENERATE(false, true);
    enum class remote_range { valid, short_extent, overflow };
    const auto range =
        GENERATE(remote_range::valid, remote_range::short_extent, remote_range::overflow);
    const auto short_local = GENERATE(false, true);
    const auto foreign = GENERATE(false, true);
    const auto disabled = GENERATE(false, true);
    loom::test::mock_domain domain{disabled ? FI_MR_ENDPOINT : 0};
    trigger_provider native{foreign ? nullptr : &domain.handle};
    std::array<std::byte, 8> buffer{};
    auto memory = loom::memory_region::register_memory(domain.dom,
                                                       std::span{buffer}.first(short_local ? 7 : 8),
                                                       loom::mr_access_flags::read |
                                                           loom::mr_access_flags::write)
                      .value();
    const loom::remote_memory remote{
        range == remote_range::overflow ? std::numeric_limits<std::uint64_t>::max() - 7 : 0x1000,
        7,
        range == remote_range::short_extent ? 7u : 8u};
    trigger_context context;
    context.set_threshold_trigger(&native.counter_handle, 99);
    const loom::trigger::threshold_condition condition{&native.counter, 7};
    const auto result =
        write ? loom::trigger::write(native.endpoint, buffer, memory, remote, &context, condition)
              : loom::trigger::read(native.endpoint, buffer, memory, remote, &context, condition);
    const bool valid = range == remote_range::valid && !short_local && !foreign && !disabled;
    REQUIRE(result.has_value() == valid);
    if (!valid)
        CHECK(result.error() == loom::errc::invalid_argument);
    CHECK(native.submissions == (valid ? 1 : 0));
    CHECK(context.trig_ctx.trigger.threshold.cntr == &native.counter_handle);
    CHECK(context.trig_ctx.trigger.threshold.threshold == (valid ? 7 : 99));
}

TEST_CASE("deferred transfers default to their work context", "[trigger]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    trigger_provider native;
    native.require_work_context = true;
    auto work = native.queue(operation, &native.counter, &native.counter);
    REQUIRE(work);
    REQUIRE(native.submissions == 1);
    auto moved = std::move(*work);
    REQUIRE(moved.internal_ptr() == native.pending);
    REQUIRE(moved.cancel());
}

TEST_CASE("deferred messages preserve explicit operation contexts", "[trigger]") {
    trigger_provider native;
    int context{};
    const auto work = loom::trigger::deferred_work::create_send(
        native.domain,
        native.endpoint,
        {&native.counter, 1},
        nullptr,
        loom::msg::send_message<void*>{.context = &context});
    REQUIRE(work);
    REQUIRE(native.pending->op.msg->msg.context == &context);
}

TEST_CASE("pending work cannot be freed after cancellation fails", "[trigger]") {
    const auto missing_control = GENERATE(false, true);
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
            if (missing_control)
                native.ops.control = nullptr;
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

TEST_CASE("triggered submissions reject unavailable resources", "[trigger]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    enum class unavailable { counter, endpoint, table, entry, layout };
    const auto missing = GENERATE(unavailable::counter,
                                  unavailable::endpoint,
                                  unavailable::table,
                                  unavailable::entry,
                                  unavailable::layout);
    CAPTURE(operation, missing);
    loom::test::mock_domain domain;
    trigger_provider native{&domain.handle};
    loom::counter empty;
    switch (missing) {
        case unavailable::counter:
            break;
        case unavailable::endpoint:
            native.endpoint = {};
            break;
        case unavailable::table:
            native.endpoint_handle.msg = nullptr;
            native.endpoint_handle.tagged = nullptr;
            native.endpoint_handle.rma = nullptr;
            break;
        case unavailable::entry:
            native.messages = {.size = sizeof(fi_ops_msg)};
            native.tags = {.size = sizeof(fi_ops_tagged)};
            native.rma = {.size = sizeof(fi_ops_rma)};
            break;
        case unavailable::layout:
            native.messages.size = sizeof(std::size_t);
            native.tags.size = sizeof(std::size_t);
            native.rma.size = sizeof(std::size_t);
            break;
    }
    trigger_context context;
    context.set_threshold_trigger(&native.counter_handle, 99);
    const loom::trigger::threshold_condition condition{
        missing == unavailable::counter ? &empty : &native.counter, 7};
    const auto result = [&]() -> loom::void_result {
        using namespace loom;
        std::array<std::byte, 1> buffer{};
        auto memory = memory_region::register_memory(
                          domain.dom, buffer, mr_access_flags::read | mr_access_flags::write)
                          .value();
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
                return trigger::read(
                    native.endpoint, std::span{buffer}.first(0), memory, {}, &context, condition);
            default:
                return trigger::write(
                    native.endpoint, std::span{buffer}.first(0), memory, {}, &context, condition);
        }
    }();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == (missing == unavailable::counter || missing == unavailable::endpoint
                                   ? loom::errc::invalid_argument
                                   : loom::errc::not_supported));
    REQUIRE(native.submissions == 0);
    REQUIRE(context.trig_ctx.trigger.threshold.cntr == &native.counter_handle);
    REQUIRE(context.trig_ctx.trigger.threshold.threshold == 99);
}
