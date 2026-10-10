// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/execution.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct completion_state {
    int calls{};
    std::size_t bytes{};
    std::error_code error;
};

struct empty_env {};

struct receiver {
    using receiver_concept = loom::execution::receiver_tag;
    completion_state* state;
    void set_value() noexcept { ++state->calls; }
    void set_value(std::size_t bytes) noexcept {
        state->bytes = bytes;
        ++state->calls;
    }
    void set_error(std::error_code error) noexcept {
        state->error = error;
        ++state->calls;
    }
    void set_stopped() noexcept {
        state->error = loom::errc::canceled;
        ++state->calls;
    }
    auto get_env() const noexcept { return empty_env{}; }
};

struct provider {
    std::vector<void*> pending;
    bool fail{};
    std::size_t completion_length{7};
    void* expected_result{};
    void* expected_result_descriptor{};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recv = [](fid_ep* ep, void*, std::size_t, void*, fi_addr_t, void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
        },
        .send = [](fid_ep* ep, const void*, std::size_t, void*, fi_addr_t, void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
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
                   void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
        },
        .write = [](fid_ep* ep,
                    const void*,
                    std::size_t,
                    void*,
                    fi_addr_t,
                    std::uint64_t,
                    std::uint64_t,
                    void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
        },
    };
    fi_ops_atomic atomics{
        .size = sizeof(fi_ops_atomic),
        .readwrite = [](fid_ep* ep,
                        const void*,
                        std::size_t,
                        void*,
                        void* result,
                        void* result_descriptor,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op,
                        void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)
                ->store_result(result, result_descriptor, ctx);
        },
        .compwrite = [](fid_ep* ep,
                        const void*,
                        std::size_t,
                        void*,
                        const void*,
                        void*,
                        void* result,
                        void* result_descriptor,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op,
                        void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)
                ->store_result(result, result_descriptor, ctx);
        },
    };
    fi_ops_cq completions{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* output, std::size_t) -> ssize_t {
            auto& self = *static_cast<provider*>(cq->fid.context);
            if (self.pending.empty())
                return -FI_EAGAIN;
            if (self.fail)
                return -FI_EAVAIL;
            *static_cast<fi_cq_tagged_entry*>(output) = {
                .op_context = self.take(), .len = self.completion_length};
            return 1;
        },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* output, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<provider*>(cq->fid.context);
            *output = {.op_context = self.take(), .err = FI_ECANCELED};
            self.fail = false;
            return 1;
        },
    };
    fid_cq cq_handle{.fid = {.context = this, .ops = &ops}, .ops = &completions};
    fid_ep ep_handle{
        .fid = {.context = this, .ops = &ops}, .msg = &messages, .rma = &rma, .atomic = &atomics};
    loom::completion_queue cq{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &cq_handle)};
    loom::endpoint ep{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
        loom::get_default_resource(),
        &ep_handle,
        loom::endpoint_types::rdm,
        loom::address_format::unspecified)};

    auto post(void* ctx) -> ssize_t {
        // Exercise the provider's right to use all FI_CONTEXT2 storage.
        std::ranges::fill(static_cast<fi_context2*>(ctx)->internal, nullptr);
        pending.push_back(ctx);
        return 0;
    }
    auto store_result(void* result, void* descriptor, void* ctx) -> ssize_t {
        if (expected_result &&
            (result != expected_result || descriptor != expected_result_descriptor)) {
            return -FI_EINVAL;
        }
        *static_cast<std::uint64_t*>(result) = 42;
        return post(ctx);
    }
    auto take() -> void* {
        auto* ctx = pending.front();
        pending.erase(pending.begin());
        return ctx;
    }
};
}  // namespace

TEST_CASE("execution progress completes composed fabric operations", "[execution][progress]") {
    provider fabric;
    completion_state state;
    std::array<std::byte, 16> outgoing{}, incoming{};
    auto operation = loom::execution::connect(
        loom::execution::when_all(loom::async(fabric.ep).send(outgoing, &fabric.cq),
                                  loom::async(fabric.ep).recv(incoming, &fabric.cq)),
        receiver{&state});
    loom::execution::start(operation);
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 2);
    REQUIRE(state.calls == 1);
    REQUIRE(state.bytes == 7);
    REQUIRE_FALSE(state.error);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 0);
}

TEST_CASE("execution progress delivers operation failures", "[execution][progress]") {
    provider fabric;
    fabric.fail = true;
    completion_state state;
    std::array<std::byte, 16> data{};
    auto operation =
        loom::execution::connect(loom::async(fabric.ep).send(data, &fabric.cq), receiver{&state});
    loom::execution::start(operation);
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 1);
    REQUIRE(state.calls == 1);
    REQUIRE(state.error == loom::errc::canceled);
}

TEST_CASE("execution progress delivers RMA completions", "[execution][progress]") {
    provider fabric;
    fabric.completion_length = 0;
    completion_state state;
    std::array<std::byte, 16> data{};
    auto operations = loom::rma_async(fabric.ep);
    auto operation = loom::execution::connect(
        loom::execution::when_all(
            operations.read(data, loom::rma_addr{0x1000}, loom::mr_key{1}, &fabric.cq),
            operations.write(data, loom::rma_addr{0x1000}, loom::mr_key{1}, &fabric.cq)),
        receiver{&state});
    static_assert(loom::execution::operation_state<decltype(operation)>);
    loom::execution::start(operation);
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 2);
    REQUIRE(state.calls == 1);
    REQUIRE(state.bytes == data.size());
}

TEST_CASE("execution progress delivers atomic results", "[execution][progress]") {
    provider fabric;
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        static_assert(loom::execution::operation_state<decltype(operation)>);
        loom::execution::start(operation);
        REQUIRE(state.calls == 0);
        REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 1);
        REQUIRE(state.calls == 1);
        REQUIRE(state.bytes == 42);
        REQUIRE_FALSE(state.error);
    };
    SECTION("fetch add") {
        run(loom::rma_async(fabric.ep).fetch_add<std::uint64_t>(
            loom::rma_addr{0x1000}, 1, loom::mr_key{1}, nullptr, &fabric.cq));
    }
    SECTION("compare swap") {
        run(loom::rma_async(fabric.ep).compare_swap<std::uint64_t>(
            loom::rma_addr{0x1000}, 42, 43, loom::mr_key{1}, nullptr, &fabric.cq));
    }
}

TEST_CASE("atomic senders store results in the supplied registration", "[execution][progress]") {
    const auto compare_swap = GENERATE(false, true);
    provider fabric;
    completion_state state;
    std::uint64_t result{};
    int descriptor{};
    fid_mr native_mr{.fid = {.ops = &fabric.ops}, .mem_desc = &descriptor};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &native_mr, &result, sizeof(result))};
    fabric.expected_result = &result;
    fabric.expected_result_descriptor = &descriptor;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        REQUIRE(state.calls == 0);
        REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 1);
        REQUIRE(state.calls == 1);
        REQUIRE_FALSE(state.error);
        REQUIRE(state.bytes == 42);
        REQUIRE(result == 42);
    };
    if (compare_swap) {
        run(loom::rma_async(fabric.ep).compare_swap<std::uint64_t>(
            loom::rma_addr{0x1000}, 42, 43, loom::mr_key{1}, &mr, &fabric.cq));
    } else {
        run(loom::rma_async(fabric.ep).fetch_add<std::uint64_t>(
            loom::rma_addr{0x1000}, 1, loom::mr_key{1}, &mr, &fabric.cq));
    }
}

TEST_CASE("atomic senders reject invalid result registrations", "[execution][progress]") {
    const auto compare_swap = GENERATE(false, true);
    const auto invalid = GENERATE(0, 1, 2, 3);
    CAPTURE(compare_swap, invalid);
    provider fabric;
    completion_state state;
    alignas(std::uint64_t) std::array<std::byte, 16> storage{};
    fid_mr native_mr{.fid = {.ops = &fabric.ops}};
    void* address = invalid == 1 ? nullptr : storage.data() + (invalid == 2 ? 1 : 0);
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        invalid == 0 ? nullptr : &native_mr,
        address,
        invalid == 3 ? 1 : storage.size())};
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        REQUIRE(state.calls == 1);
        REQUIRE(state.error == loom::errc::invalid_argument);
        REQUIRE(fabric.pending.empty());
    };
    if (compare_swap) {
        run(loom::rma_async(fabric.ep).compare_swap<std::uint64_t>(
            loom::rma_addr{0x1000}, 42, 43, loom::mr_key{1}, &mr, &fabric.cq));
    } else {
        run(loom::rma_async(fabric.ep).fetch_add<std::uint64_t>(
            loom::rma_addr{0x1000}, 1, loom::mr_key{1}, &mr, &fabric.cq));
    }
}
