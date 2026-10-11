// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/execution.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
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
    bool selective_completion{};
    std::uint64_t submitted_flags{};
    std::size_t completion_length{7};
    void* expected_result{};
    void* expected_result_descriptor{};
    std::uint64_t remote_value{42};
    void* local_descriptor{};
    fi_addr_t peer{};
    std::uint64_t remote_address{};
    std::uint64_t remote_key{};
    std::size_t submitted_bytes{};
    int submission_status{};
    void* submitted_context{};
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
                   std::size_t length,
                   void* descriptor,
                   fi_addr_t peer,
                   std::uint64_t address,
                   std::uint64_t key,
                   void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)
                ->post_rma(length, descriptor, peer, address, key, ctx);
        },
        .readmsg = [](fid_ep* ep, const fi_msg_rma* message, std::uint64_t flags) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post_rma_message(*message, flags);
        },
        .write = [](fid_ep* ep,
                    const void*,
                    std::size_t length,
                    void* descriptor,
                    fi_addr_t peer,
                    std::uint64_t address,
                    std::uint64_t key,
                    void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)
                ->post_rma(length, descriptor, peer, address, key, ctx);
        },
        .writemsg = [](fid_ep* ep, const fi_msg_rma* message, std::uint64_t flags) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post_rma_message(*message, flags);
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
                        const void* value,
                        std::size_t,
                        void*,
                        const void* compare,
                        void*,
                        void* result,
                        void* result_descriptor,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op,
                        void* ctx) -> ssize_t {
            auto& self = *static_cast<provider*>(ep->fid.context);
            const auto status = self.store_result(result, result_descriptor, ctx);
            if (status == 0 && self.remote_value == *static_cast<const std::uint64_t*>(compare)) {
                self.remote_value = *static_cast<const std::uint64_t*>(value);
            }
            return status;
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
            *static_cast<fi_cq_tagged_entry*>(output) = {.op_context = self.take(),
                                                         .len = self.completion_length};
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
        loom::address_format::unspecified,
        0UZ,
        0UZ,
        loom::caps{},
        FI_FENCE)};

    auto post(void* ctx, std::uint64_t flags = 0) -> ssize_t {
        // Exercise the provider's right to use all FI_CONTEXT2 storage.
        std::ranges::fill(static_cast<fi_context2*>(ctx)->internal, nullptr);
        if (!selective_completion || (flags & FI_COMPLETION))
            pending.push_back(ctx);
        return 0;
    }
    auto post_rma(std::size_t length,
                  void* descriptor,
                  fi_addr_t destination,
                  std::uint64_t address,
                  std::uint64_t key,
                  void* ctx,
                  std::uint64_t flags = 0) -> ssize_t {
        submitted_bytes = length;
        submitted_context = ctx;
        local_descriptor = descriptor;
        peer = destination;
        remote_address = address;
        remote_key = key;
        return submission_status ? submission_status : post(ctx, flags);
    }
    auto post_rma_message(const fi_msg_rma& message, std::uint64_t flags) -> ssize_t {
        submitted_flags = flags;
        REQUIRE(message.iov_count == 1);
        REQUIRE(message.rma_iov_count == 1);
        return post_rma(message.msg_iov[0].iov_len,
                        message.desc ? message.desc[0] : nullptr,
                        message.addr,
                        message.rma_iov[0].addr,
                        message.rma_iov[0].key,
                        message.context,
                        flags);
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

TEST_CASE("RMA senders preserve registered peer submissions", "[execution][progress]") {
    const auto write = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    const auto short_remote = GENERATE(false, true);
    provider fabric;
    fabric.selective_completion = GENERATE(false, true);
    CAPTURE(fabric.selective_completion);
    fabric.submission_status = status;
    std::array<std::byte, 16> storage{};
    fid_mr native_mr{.fid = {.ops = &fabric.ops}, .mem_desc = &native_mr};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &native_mr, storage.data(), storage.size())};
    const loom::fabric_addr peer{19};
    const loom::remote_memory remote{24, 0x123456789abcdef0ULL, short_remote ? 15u : 16u};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        if (short_remote) {
            REQUIRE(state.calls == 1);
            REQUIRE(state.error == loom::errc::invalid_argument);
            REQUIRE(fabric.submitted_bytes == 0);
        } else {
            REQUIRE(fabric.submitted_flags == (FI_COMPLETION | FI_FENCE));
            REQUIRE(fabric.submitted_bytes == storage.size());
            REQUIRE(fabric.peer == peer.get());
            REQUIRE(fabric.local_descriptor == &native_mr);
            REQUIRE(fabric.remote_address == remote.addr);
            REQUIRE(fabric.remote_key == remote.key);
            if (status) {
                REQUIRE(state.calls == 1);
                REQUIRE(state.error == loom::make_error_code_from_fi_errno(-status));
            } else {
                REQUIRE(state.calls == 0);
                REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
                REQUIRE(state.calls == 1);
                REQUIRE_FALSE(state.error);
                REQUIRE(state.bytes == (write ? 0 : storage.size()));
            }
        }
        REQUIRE(fabric.pending.empty());
    };
    const auto operations = loom::rma_async(fabric.ep);
    if (write)
        run(operations.write_to(storage, mr, peer, remote, &fabric.cq));
    else
        run(operations.read_from(storage, mr, peer, remote, &fabric.cq));
}

TEST_CASE("RMA senders complete sockets transfers with selective completion",
          "[execution][progress][rma][loopback]") {
    using namespace loom;
    const auto write = GENERATE(false, true);
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::rma;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto* native_info = static_cast<fi_info*>(info->internal_ptr());
    // The frontend must add FI_COMPLETION without losing delivery semantics.
    native_info->tx_attr->op_flags = write ? FI_DELIVERY_COMPLETE : 0;
    auto fab = fabric::create(*info);
    REQUIRE(fab);
    auto dom = domain::create(*fab, *info);
    REQUIRE(dom);
    auto cq = completion_queue::create(*dom);
    REQUIRE(cq);
    auto av = address_vector::create(*dom);
    REQUIRE(av);
    auto ep = endpoint::create(*dom, *info);
    REQUIRE(ep);
    REQUIRE(ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
    REQUIRE(ep->bind_av(*av));
    REQUIRE(ep->enable());
    auto address = ep->get_local_address();
    REQUIRE(address);
    auto peer = av->insert(*address);
    REQUIRE(peer);
    std::array<std::byte, 8> local{}, target{};
    local.fill(std::byte{23});
    target.fill(std::byte{42});
    auto local_mr =
        memory_region::register_memory(*dom, local, mr_access_flags::read | mr_access_flags::write);
    REQUIRE(local_mr);
    auto target_mr = memory_region::register_memory(
        *dom, target, mr_access_flags::remote_read | mr_access_flags::remote_write);
    REQUIRE(target_mr);
    const auto remote = remote_memory::from_mr(*target_mr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = execution::connect(std::move(sender), receiver{&state});
        execution::start(operation);
        while (state.calls == 0 && std::chrono::steady_clock::now() < deadline) {
            const auto progress = execution::poll_completions(*cq);
            if (!progress || progress->unhandled) {
                REQUIRE(ep->close());
                FAIL("Unexpected completion while progressing RMA");
            }
            std::this_thread::yield();
        }
        if (state.calls == 0) {
            // Retire native work before a failing test destroys borrowed storage.
            REQUIRE(ep->close());
            FAIL("Selective-completion RMA timed out");
        }
        REQUIRE(state.calls == 1);
    };
    do {
        state = {};
        auto operations = rma_async(*ep);
        if (write)
            run(operations.write_to(local, *local_mr, fabric_addr{peer->value}, remote, &*cq));
        else
            run(operations.read_from(local, *local_mr, fabric_addr{peer->value}, remote, &*cq));
        if (state.error != errc::again)
            break;
        (void)cq->poll();
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE_FALSE(state.error);
    REQUIRE(state.bytes == (write ? 0 : local.size()));
    REQUIRE(local == target);
}

TEST_CASE("compare swap senders compare before replacing remote data", "[execution][progress]") {
    const auto expected = GENERATE(std::uint64_t{42}, std::uint64_t{84});
    provider fabric;
    completion_state state;
    auto operation = loom::execution::connect(
        loom::rma_async(fabric.ep).compare_swap<std::uint64_t>(
            loom::rma_addr{0x1000}, expected, 84, loom::mr_key{1}, nullptr, &fabric.cq),
        receiver{&state});
    loom::execution::start(operation);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
    REQUIRE(state.calls == 1);
    REQUIRE_FALSE(state.error);
    REQUIRE(state.bytes == 42);
    REQUIRE(fabric.remote_value == (expected == 42 ? 84 : 42));
}

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
    REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 2);
    REQUIRE(state.calls == 1);
    REQUIRE(state.bytes == 7);
    REQUIRE_FALSE(state.error);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 0);
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
    REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
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
    REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 2);
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
        REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
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
        REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
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

TEST_CASE("execution polling returns foreign completions intact", "[execution][progress]") {
    provider fabric;
    std::array<std::byte, 16> data{};
    fi_context2 foreign{};
    fabric.pending.push_back(&foreign);
    completion_state state;
    auto operation =
        loom::execution::connect(loom::async(fabric.ep).send(data, &fabric.cq), receiver{&state});
    loom::execution::start(operation);
    fabric.pending.push_back(nullptr);
    auto first = loom::execution::poll_completions(fabric.cq).value();
    REQUIRE(first.completed == 0);
    REQUIRE(first.unhandled);
    REQUIRE(first.unhandled->context.get() == &foreign);
    REQUIRE(first.unhandled->bytes_transferred == fabric.completion_length);
    REQUIRE(state.calls == 0);
    auto second = loom::execution::poll_completions(fabric.cq).value();
    REQUIRE(second.completed == 1);
    REQUIRE(second.unhandled);
    REQUIRE_FALSE(second.unhandled->context);
    REQUIRE(second.unhandled->bytes_transferred == fabric.completion_length);
    REQUIRE(state.calls == 1);
    REQUIRE_FALSE(state.error);
    auto third = loom::execution::poll_completions(fabric.cq).value();
    REQUIRE(third.completed == 0);
    REQUIRE_FALSE(third.unhandled);
}

TEST_CASE("unattributed CQ errors do not complete or discard senders", "[execution][progress]") {
    provider fabric;
    std::array<std::byte, 16> data{};
    fabric.pending.push_back(nullptr);
    fabric.fail = true;
    completion_state state;
    auto operation =
        loom::execution::connect(loom::async(fabric.ep).send(data, &fabric.cq), receiver{&state});
    loom::execution::start(operation);
    auto first = loom::execution::poll_completions(fabric.cq).value();
    REQUIRE(first.completed == 0);
    REQUIRE(first.unhandled);
    REQUIRE_FALSE(first.unhandled->context);
    REQUIRE(first.unhandled->error == loom::errc::canceled);
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
    REQUIRE(state.calls == 1);
    REQUIRE_FALSE(state.error);
}

TEST_CASE("raw CQ readers can dispatch registered sender completions", "[execution][progress]") {
    provider fabric;
    std::array<std::byte, 16> data{};
    completion_state state;
    auto operation =
        loom::execution::connect(loom::async(fabric.ep).recv(data, &fabric.cq), receiver{&state});
    loom::execution::start(operation);
    const auto event = fabric.cq.poll();
    REQUIRE(event);
    REQUIRE(state.calls == 0);
    REQUIRE(fabric.cq.dispatch(*event));
    REQUIRE(state.calls == 1);
    REQUIRE(state.bytes == fabric.completion_length);
    REQUIRE_FALSE(fabric.cq.dispatch(*event));
    REQUIRE(state.calls == 1);
    auto moved = std::move(fabric.cq);
    REQUIRE_FALSE(moved.dispatch(*event));
}

TEST_CASE("sender rejection detaches its completion registration", "[execution][progress]") {
    provider fabric;
    fabric.submission_status = -FI_EAGAIN;
    completion_state state;
    std::array<std::byte, 16> data{};
    auto operation = loom::execution::connect(
        loom::rma_async(fabric.ep).write(data, loom::rma_addr{0}, loom::mr_key{1}, &fabric.cq),
        receiver{&state});
    loom::execution::start(operation);
    REQUIRE(state.calls == 1);
    REQUIRE(state.error == loom::errc::again);
    REQUIRE(fabric.pending.empty());
    // No callback remains registered after the failed start.
    loom::completion_event event{};
    event.context = loom::context_ptr<void>{fabric.submitted_context};
    REQUIRE(event.context);
    REQUIRE_FALSE(fabric.cq.dispatch(event));
    REQUIRE(state.calls == 1);
}
