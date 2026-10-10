// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/execution.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {
struct completion_state {
    int calls{};
    std::size_t bytes{};
    std::error_code error;
};

struct receiver {
    using receiver_concept = stdexec::receiver_t;
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
    auto get_env() const noexcept { return stdexec::env{}; }
};

struct provider {
    std::vector<void*> pending;
    bool fail{};
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
    fi_ops_cq completions{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* output, std::size_t) -> ssize_t {
            auto& self = *static_cast<provider*>(cq->fid.context);
            if (self.pending.empty())
                return -FI_EAGAIN;
            if (self.fail)
                return -FI_EAVAIL;
            *static_cast<fi_cq_tagged_entry*>(output) = {.op_context = self.take(), .len = 7};
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
    fid_ep ep_handle{.fid = {.context = this, .ops = &ops}, .msg = &messages};
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
    auto take() -> void* {
        auto* ctx = pending.front();
        pending.erase(pending.begin());
        return ctx;
    }
};
}  // namespace

TEST_CASE("execution progress completes composed fabric operations", "[stdexec][progress]") {
    provider fabric;
    completion_state state;
    std::array<std::byte, 16> outgoing{}, incoming{};
    auto operation =
        stdexec::connect(stdexec::when_all(loom::async(fabric.ep).send(outgoing, &fabric.cq),
                                           loom::async(fabric.ep).recv(incoming, &fabric.cq)),
                         receiver{&state});
    stdexec::start(operation);
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 2);
    REQUIRE(state.calls == 1);
    REQUIRE(state.bytes == 7);
    REQUIRE_FALSE(state.error);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 0);
}

TEST_CASE("execution progress delivers operation failures", "[stdexec][progress]") {
    provider fabric;
    fabric.fail = true;
    completion_state state;
    std::array<std::byte, 16> data{};
    auto operation =
        stdexec::connect(loom::async(fabric.ep).send(data, &fabric.cq), receiver{&state});
    stdexec::start(operation);
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value() == 1);
    REQUIRE(state.calls == 1);
    REQUIRE(state.error == loom::errc::canceled);
}
