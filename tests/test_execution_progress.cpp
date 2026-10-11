// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/execution.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
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
    fid_domain domain_handle{};
    struct pending_atomic {
        const std::uint64_t* input;
        const std::uint64_t* comparison;
        std::uint64_t* output;
        void* context;
    };
    std::vector<pending_atomic> atomic_pending;
    std::vector<void*> pending;
    std::vector<void*> deferred;
    bool fail{};
    bool allocation_failure{};
    bool selective_completion{};
    fi_addr_t expected_message_peer{FI_ADDR_UNSPEC};
    void* expected_message_descriptor{};
    std::uint64_t submitted_flags{};
    std::size_t completion_length{7};
    void* expected_result{};
    void* expected_result_descriptor{};
    const void* expected_atomic_input{};
    void* expected_atomic_input_descriptor{};
    const void* expected_atomic_comparison{};
    void* expected_atomic_comparison_descriptor{};
    fi_addr_t expected_atomic_peer{FI_ADDR_UNSPEC};
    std::uint64_t remote_value{42};
    void* local_descriptor{};
    fi_addr_t peer{};
    std::uint64_t remote_address{};
    std::uint64_t remote_key{};
    std::size_t submitted_bytes{};
    int submission_status{};
    void* submitted_context{};
    fi_ops ops{.size = sizeof(fi_ops),
               .close = [](fid*) { return 0; },
               .bind = [](fid*, fid*, std::uint64_t) { return 0; }};
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recv = [](fid_ep* ep, void*, std::size_t, void*, fi_addr_t, void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
        },
        .recvmsg = [](fid_ep* ep, const fi_msg* message, std::uint64_t flags) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post_message(*message, flags);
        },
        .send = [](fid_ep* ep, const void*, std::size_t, void*, fi_addr_t, void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
        },
        .sendmsg = [](fid_ep* ep, const fi_msg* message, std::uint64_t flags) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post_message(*message, flags);
        },
    };
    fi_ops_tagged tagged{
        .size = sizeof(fi_ops_tagged),
        .recv = [](fid_ep* ep,
                   void*,
                   std::size_t,
                   void*,
                   fi_addr_t,
                   std::uint64_t,
                   std::uint64_t,
                   void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)->post(ctx);
        },
        .recvmsg = [](fid_ep* ep, const fi_msg_tagged* message, std::uint64_t flags) -> ssize_t {
            REQUIRE(message->tag == 42);
            REQUIRE(message->ignore == 0x80);
            return static_cast<provider*>(ep->fid.context)->post_message(*message, flags);
        },
        .send = [](fid_ep* ep, const void*, std::size_t, void*, fi_addr_t, std::uint64_t, void* ctx)
            -> ssize_t { return static_cast<provider*>(ep->fid.context)->post(ctx); },
        .sendmsg = [](fid_ep* ep, const fi_msg_tagged* message, std::uint64_t flags) -> ssize_t {
            REQUIRE(message->tag == 42);
            return static_cast<provider*>(ep->fid.context)->post_message(*message, flags);
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
                        const void* input,
                        std::size_t,
                        void*,
                        void* result,
                        void* descriptor,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op,
                        void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)
                ->post_atomic(input, nullptr, result, descriptor, ctx);
        },
        .readwritemsg = [](fid_ep* ep,
                           const fi_msg_atomic* message,
                           fi_ioc* results,
                           void** descriptors,
                           std::size_t count,
                           std::uint64_t flags) -> ssize_t {
            REQUIRE(count == 1);
            return static_cast<provider*>(ep->fid.context)
                ->post_atomic_message(
                    *message, nullptr, results[0], descriptors ? descriptors[0] : nullptr, flags);
        },
        .compwrite = [](fid_ep* ep,
                        const void* input,
                        std::size_t,
                        void*,
                        const void* compare,
                        void*,
                        void* result,
                        void* descriptor,
                        fi_addr_t,
                        std::uint64_t,
                        std::uint64_t,
                        fi_datatype,
                        fi_op,
                        void* ctx) -> ssize_t {
            return static_cast<provider*>(ep->fid.context)
                ->post_atomic(input, compare, result, descriptor, ctx);
        },
        .compwritemsg = [](fid_ep* ep,
                           const fi_msg_atomic* message,
                           const fi_ioc* compare,
                           void** compare_descriptors,
                           std::size_t compare_count,
                           fi_ioc* results,
                           void** descriptors,
                           std::size_t count,
                           std::uint64_t flags) -> ssize_t {
            REQUIRE(compare_count == 1);
            REQUIRE(compare[0].count == 1);
            REQUIRE(count == 1);
            return static_cast<provider*>(ep->fid.context)
                ->post_atomic_message(*message,
                                      compare[0].addr,
                                      results[0],
                                      descriptors ? descriptors[0] : nullptr,
                                      flags,
                                      compare_descriptors ? compare_descriptors[0] : nullptr);
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
    fid_ep ep_handle{.fid = {.context = this, .ops = &ops},
                     .msg = &messages,
                     .rma = &rma,
                     .tagged = &tagged,
                     .atomic = &atomics};
    loom::completion_queue cq{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &cq_handle)};
    loom::endpoint ep;

    explicit provider(std::uint64_t receive_defaults = FI_MORE,
                      std::uint64_t transmit_defaults = FI_FENCE,
                      bool bind_completions = true)
        : ep{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
              loom::get_default_resource(),
              &ep_handle,
              loom::endpoint_types::rdm,
              loom::address_format::unspecified,
              0UZ,
              0UZ,
              loom::caps{},
              transmit_defaults,
              receive_defaults,
              &domain_handle)} {
        if (bind_completions)
            REQUIRE(ep.bind_cq(cq, loom::cq_bind::transmit | loom::cq_bind::recv));
    }

    auto post(void* ctx, std::uint64_t flags = 0) -> ssize_t {
        if (submission_status)
            return submission_status;
        // Exercise the provider's right to use all FI_CONTEXT2 storage.
        std::ranges::fill(static_cast<fi_context2*>(ctx)->internal, nullptr);
        if (!selective_completion || (flags & FI_COMPLETION))
            deferred.push_back(ctx);
        if (!(flags & FI_MORE))
            flush();
        return 0;
    }
    void flush() {
        pending.insert(pending.end(), deferred.begin(), deferred.end());
        deferred.clear();
    }
    auto post_message(const auto& message, std::uint64_t flags) -> ssize_t {
        submitted_flags = flags;
        submitted_context = message.context;
        REQUIRE(message.iov_count == 1);
        REQUIRE(message.addr == expected_message_peer);
        REQUIRE(message.msg_iov[0].iov_len == 16);
        if (expected_message_descriptor) {
            REQUIRE(message.desc);
            REQUIRE(message.desc[0] == expected_message_descriptor);
        } else {
            REQUIRE(message.desc == nullptr);
        }
        if (allocation_failure)
            throw std::bad_alloc{};
        return post(message.context, flags);
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
    auto post_atomic(const void* input,
                     const void* compare,
                     void* result,
                     void* descriptor,
                     void* ctx,
                     std::uint64_t flags = 0) -> ssize_t {
        if (expected_result &&
            (result != expected_result || descriptor != expected_result_descriptor))
            return -FI_EINVAL;
        const auto status = post(ctx, flags);
        if (!status)
            atomic_pending.push_back({static_cast<const std::uint64_t*>(input),
                                      static_cast<const std::uint64_t*>(compare),
                                      static_cast<std::uint64_t*>(result),
                                      ctx});
        return status;
    }
    auto post_atomic_message(const fi_msg_atomic& message,
                             const void* compare,
                             fi_ioc result,
                             void* descriptor,
                             std::uint64_t flags,
                             void* compare_descriptor = nullptr) -> ssize_t {
        submitted_flags = flags;
        submitted_context = message.context;
        REQUIRE(message.iov_count == 1);
        REQUIRE(message.msg_iov[0].count == 1);
        REQUIRE(message.rma_iov_count == 1);
        REQUIRE(message.rma_iov[0].count == 1);
        REQUIRE(message.rma_iov[0].addr == 0x1000);
        REQUIRE(message.rma_iov[0].key == 1);
        REQUIRE(message.addr == expected_atomic_peer);
        REQUIRE(message.datatype == FI_UINT64);
        REQUIRE(message.op == (compare ? FI_CSWAP : FI_SUM));
        REQUIRE(result.count == 1);
        if (expected_atomic_input) {
            REQUIRE(message.msg_iov[0].addr == expected_atomic_input);
            REQUIRE((message.desc ? message.desc[0] : nullptr) == expected_atomic_input_descriptor);
            REQUIRE(compare == expected_atomic_comparison);
            REQUIRE(compare_descriptor == expected_atomic_comparison_descriptor);
        }
        return post_atomic(
            message.msg_iov[0].addr, compare, result.addr, descriptor, message.context, flags);
    }
    auto take() -> void* {
        auto* ctx = pending.front();
        const auto atomic = std::ranges::find(atomic_pending, ctx, &pending_atomic::context);
        if (atomic != atomic_pending.end()) {
            // The provider may first read operands after submission has returned.
            *atomic->output = remote_value;
            if (!atomic->comparison)
                remote_value += *atomic->input;
            else if (remote_value == *atomic->comparison)
                remote_value = *atomic->input;
            atomic_pending.erase(atomic);
        }
        pending.erase(pending.begin());
        return ctx;
    }
};
}  // namespace

TEST_CASE("senders validate their directional completion queue", "[execution][progress][binding]") {
    provider native{0, 0, false}, other;
    using namespace loom;
    enum class route { correct, wrong, unbound };
    const auto binding = GENERATE(route::correct, route::wrong, route::unbound);
    CAPTURE(binding);
    std::array<std::byte, 16> storage{};
    std::uint64_t input{3}, comparison{42}, result{};
    const remote_memory remote{0x1000, 1, sizeof(result)};
    completion_state state;
    auto run = [&](auto make_sender, bool receive = false) {
        // Keep the opposite direction bound even when the required one is absent.
        if (binding != route::unbound || receive)
            REQUIRE(native.ep.bind_tx_cq(native.cq));
        if (binding != route::unbound || !receive)
            REQUIRE(native.ep.bind_rx_cq(other.cq));
        // Validate the current wrappers, not their addresses at bind time.
        auto ep = std::move(native.ep);
        auto tx = std::move(native.cq);
        auto rx = std::move(other.cq);
        auto& actual = receive ? rx : tx;
        auto& opposite = receive ? tx : rx;
        auto& supplied = binding == route::wrong ? opposite : actual;
        auto operation = execution::connect(make_sender(ep, &supplied), receiver{&state});
        execution::start(operation);
        const bool rejected = binding != route::correct;
        CHECK(state.calls == (rejected ? 1 : 0));
        CHECK(state.error ==
              (rejected ? make_error_code(errc::invalid_argument) : std::error_code{}));
        CHECK(native.pending.empty() == rejected);
        CHECK(native.submitted_flags == (rejected ? 0 : FI_COMPLETION));
        if (!native.pending.empty()) {
            completion_event event{.context = context_ptr<void>{native.take()},
                                   .bytes_transferred = native.completion_length};
            if (!rejected) {
                CHECK_FALSE(opposite.dispatch(event));
                CHECK(actual.dispatch(event));
                CHECK_FALSE(actual.dispatch(event));
            } else {
                // Retire a pre-fix submission before its borrowed state is destroyed.
                (void)supplied.dispatch(event);
            }
        }
        REQUIRE(state.calls == 1);
        REQUIRE(native.pending.empty());
        REQUIRE(native.atomic_pending.empty());
    };
    SECTION("send") {
        run([&](endpoint& ep, completion_queue* cq) { return async(ep).send(storage, cq); });
    }
    SECTION("receive") {
        run([&](endpoint& ep, completion_queue* cq) { return async(ep).recv(storage, cq); }, true);
    }
    SECTION("tagged send") {
        run([&](endpoint& ep, completion_queue* cq) {
            return async(ep).tagged_send(storage, 42, cq);
        });
    }
    SECTION("tagged receive") {
        run([&](endpoint& ep,
                completion_queue* cq) { return async(ep).tagged_recv(storage, 42, 0x80, cq); },
            true);
    }
    SECTION("read") {
        run([&](endpoint& ep, completion_queue* cq) {
            return rma_async(ep).read(storage, rma_addr{0x1000}, mr_key{1}, cq);
        });
    }
    SECTION("write") {
        run([&](endpoint& ep, completion_queue* cq) {
            return rma_async(ep).write(storage, rma_addr{0x1000}, mr_key{1}, cq);
        });
    }
    SECTION("fetch add") {
        run([&](endpoint& ep, completion_queue* cq) {
            return rma_async(ep).fetch_add<std::uint64_t>(
                rma_addr{0x1000}, 3, mr_key{1}, nullptr, cq);
        });
    }
    SECTION("compare swap") {
        run([&](endpoint& ep, completion_queue* cq) {
            return rma_async(ep).compare_swap<std::uint64_t>(
                rma_addr{0x1000}, 42, 3, mr_key{1}, nullptr, cq);
        });
    }
    SECTION("borrowed fetch add") {
        run([&](endpoint& ep, completion_queue* cq) {
            return rma_async(ep).fetch_add_to(
                input, nullptr, result, nullptr, fabric_addrs::unspecified, remote, cq);
        });
    }
    SECTION("borrowed compare swap") {
        run([&](endpoint& ep, completion_queue* cq) {
            return rma_async(ep).compare_swap_to(comparison,
                                                 nullptr,
                                                 input,
                                                 nullptr,
                                                 result,
                                                 nullptr,
                                                 fabric_addrs::unspecified,
                                                 remote,
                                                 cq);
        });
    }
}

TEST_CASE("message senders request selective completions", "[execution][progress][message]") {
    const auto tagged = GENERATE(false, true);
    const auto receive = GENERATE(false, true);
    const auto selective = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    provider native;
    native.selective_completion = selective;
    native.submission_status = status;
    std::array<std::byte, 16> storage{};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        CHECK(native.submitted_flags == (FI_COMPLETION | (receive ? 0 : FI_FENCE)));
        if (status) {
            REQUIRE(state.calls == 1);
            REQUIRE(state.error == loom::make_error_code_from_fi_errno(-status));
        } else {
            REQUIRE(state.calls == 0);
            REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 1);
            REQUIRE(state.calls == 1);
            REQUIRE_FALSE(state.error);
            REQUIRE(state.bytes == (receive ? native.completion_length : 0));
        }
        REQUIRE(native.pending.empty());
    };
    auto operations = loom::async(native.ep);
    if (tagged) {
        if (receive)
            run(operations.tagged_recv(storage, 42, 0x80, &native.cq));
        else
            run(operations.tagged_send(storage, 42, &native.cq));
    } else if (receive) {
        run(operations.recv(storage, &native.cq));
    } else {
        run(operations.send(storage, &native.cq));
    }
}

TEST_CASE("message senders preserve peers and registrations", "[execution][progress][message]") {
    const auto tagged = GENERATE(false, true);
    const auto receive = GENERATE(false, true);
    const auto registered = GENERATE(false, true);
    const auto explicit_peer = GENERATE(false, true);
    const auto failure = GENERATE(0, FI_EAGAIN, FI_EIO, FI_ENOMEM);
    provider native;
    native.selective_completion = true;
    native.allocation_failure = failure == FI_ENOMEM;
    native.submission_status = native.allocation_failure ? 0 : -failure;
    native.expected_message_peer = explicit_peer ? 19 : FI_ADDR_UNSPEC;
    std::array<std::byte, 16> storage{};
    fid_mr native_mr{.fid = {.ops = &native.ops}, .mem_desc = &native_mr};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        &native_mr,
        storage.data(),
        storage.size(),
        loom::detail::mr_key_lease{},
        0,
        0,
        &native.domain_handle)};
    const auto descriptor = registered ? mr.descriptor() : loom::mr_descriptor{};
    native.expected_message_descriptor = descriptor.raw();
    const loom::fabric_addr peer{native.expected_message_peer};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        REQUIRE(native.submitted_flags == (FI_COMPLETION | (receive ? 0 : FI_FENCE)));
        if (failure) {
            REQUIRE(state.calls == 1);
            REQUIRE(state.error == loom::make_error_code_from_fi_errno(failure));
            REQUIRE_FALSE(native.cq.dispatch(loom::completion_event{
                .context = loom::context_ptr<void>{native.submitted_context}}));
        } else {
            REQUIRE(state.calls == 0);
            REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 1);
            REQUIRE(state.calls == 1);
            REQUIRE_FALSE(state.error);
            REQUIRE(state.bytes == (receive ? native.completion_length : 0));
        }
        REQUIRE(native.pending.empty());
    };
    const auto operations = loom::async(native.ep);
    if (tagged) {
        if (receive)
            run(operations.tagged_recv_from(storage, descriptor, peer, 42, 0x80, &native.cq));
        else
            run(operations.tagged_send_to(storage, descriptor, peer, 42, &native.cq));
    } else if (receive) {
        run(operations.recv_from(storage, descriptor, peer, &native.cq));
    } else {
        run(operations.send_to(storage, descriptor, peer, &native.cq));
    }
}

TEST_CASE("receive senders require completion lengths", "[execution][progress][message]") {
    const auto tagged = GENERATE(false, true);
    const auto format =
        GENERATE(FI_CQ_FORMAT_CONTEXT, FI_CQ_FORMAT_MSG, FI_CQ_FORMAT_DATA, FI_CQ_FORMAT_TAGGED);
    provider native;
    native.cq =
        loom::completion_queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
            loom::get_default_resource(),
            &native.cq_handle,
            loom::runtime_progress_policy{},
            false,
            format)};
    REQUIRE(native.ep.bind_cq(native.cq, loom::cq_bind::transmit | loom::cq_bind::recv));
    std::array<std::byte, 16> storage{};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        const auto rejected = format == FI_CQ_FORMAT_CONTEXT;
        CHECK(native.pending.empty() == rejected);
        if (!native.pending.empty()) {
            loom::completion_event event{.context = loom::context_ptr<void>{native.take()},
                                         .bytes_transferred = 7};
            REQUIRE(native.cq.dispatch(event));
        }
        REQUIRE(state.calls == 1);
        CHECK(state.error ==
              (rejected ? loom::make_error_code(loom::errc::not_supported) : std::error_code{}));
        CHECK(state.bytes == (rejected ? 0 : 7));
    };
    auto operations = loom::async(native.ep);
    if (tagged)
        run(operations.tagged_recv(storage, 42, 0x80, &native.cq));
    else
        run(operations.recv(storage, &native.cq));

    state = {};
    auto send = loom::execution::connect(operations.send(storage, &native.cq), receiver{&state});
    loom::execution::start(send);
    REQUIRE(state.calls == 0);
    loom::completion_event sent{.context = loom::context_ptr<void>{native.take()}};
    REQUIRE(native.cq.dispatch(sent));
    REQUIRE(state.calls == 1);
    REQUIRE_FALSE(state.error);
}

TEST_CASE("message senders transfer to a registered RDM peer",
          "[execution][progress][message][loopback]") {
    using namespace loom;
    const auto tagged = GENERATE(false, true);
    const auto source_filter = GENERATE(false, true);
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::tagged | capability::directed_recv;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto fab = fabric::create(*info);
    REQUIRE(fab);
    auto dom = domain::create(*fab, *info);
    REQUIRE(dom);
    auto cq = completion_queue::create(*dom);
    REQUIRE(cq);
    auto av = address_vector::create(*dom);
    REQUIRE(av);
    std::array<endpoint, 2> endpoints;
    std::array<fabric_addr, 2> peers;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto ep = endpoint::create(*dom, *info);
        REQUIRE(ep);
        REQUIRE(
            ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
        REQUIRE(ep->bind_av(*av));
        REQUIRE(ep->enable());
        auto address = ep->get_local_address();
        REQUIRE(address);
        auto peer = av->insert(*address);
        REQUIRE(peer);
        peers[i] = fabric_addr{peer->value};
        endpoints[i] = std::move(*ep);
    }
    std::array<std::byte, 16> outgoing;
    std::array<std::byte, 32> incoming{};
    outgoing.fill(std::byte{0x5a});
    auto send_mr = memory_region::register_memory(*dom, outgoing, mr_access_flags::send);
    REQUIRE(send_mr);
    auto recv_mr = memory_region::register_memory(*dom, incoming, mr_access_flags::recv);
    REQUIRE(recv_mr);
    completion_state received;
    const auto source = source_filter ? peers[0] : fabric_addrs::unspecified;
    auto run = [&](auto receive_sender) {
        auto receive = execution::connect(std::move(receive_sender), receiver{&received});
        execution::start(receive);
        REQUIRE(received.calls == 0);
        auto send_and_wait = [&](auto send_sender) {
            completion_state sent;
            auto send = execution::connect(std::move(send_sender), receiver{&sent});
            execution::start(send);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while ((sent.calls == 0 || received.calls == 0) &&
                   std::chrono::steady_clock::now() < deadline) {
                const auto progress = execution::poll_completions(*cq);
                if (!progress || progress->unhandled)
                    break;
                std::this_thread::yield();
            }
            if (sent.calls == 0 || received.calls == 0) {
                // Retire native requests before destroying their borrowed storage.
                for (auto& ep : endpoints)
                    REQUIRE(ep.close());
                FAIL("RDM message did not complete");
            }
            REQUIRE(sent.calls == 1);
            REQUIRE_FALSE(sent.error);
            REQUIRE(received.calls == 1);
            REQUIRE_FALSE(received.error);
        };
        const auto operations = async(endpoints[0]);
        if (tagged)
            send_and_wait(
                operations.tagged_send_to(outgoing, send_mr->descriptor(), peers[1], 42, &*cq));
        else
            send_and_wait(operations.send_to(outgoing, send_mr->descriptor(), peers[1], &*cq));
    };
    const auto operations = async(endpoints[1]);
    if (tagged)
        run(operations.tagged_recv_from(incoming, recv_mr->descriptor(), source, 42, 0, &*cq));
    else
        run(operations.recv_from(incoming, recv_mr->descriptor(), source, &*cq));
    REQUIRE(received.bytes == outgoing.size());
    REQUIRE(std::ranges::equal(outgoing, std::span{incoming}.first(outgoing.size())));
    REQUIRE(std::ranges::all_of(std::span{incoming}.subspan(outgoing.size()),
                                [](std::byte value) { return value == std::byte{}; }));
}

TEST_CASE("senders complete without a later batch submission", "[execution][progress][batch]") {
    provider native{FI_MORE, FI_MORE | FI_FENCE | FI_DELIVERY_COMPLETE};
    native.selective_completion = true;
    std::array<std::byte, 16> storage{};
    completion_state state;
    auto run = [&](auto sender, std::uint64_t expected = FI_FENCE | FI_DELIVERY_COMPLETE) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        REQUIRE(state.calls == 0);
        CHECK(native.submitted_flags == (FI_COMPLETION | expected));
        CHECK(loom::execution::poll_completions(native.cq).value().completed == 1);
        CHECK(state.calls == 1);
        // Retire a deferred pre-fix operation before its storage goes away.
        if (!native.deferred.empty()) {
            native.flush();
            REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 1);
        }
        REQUIRE(state.calls == 1);
        REQUIRE_FALSE(state.error);
        REQUIRE(native.pending.empty());
        REQUIRE(native.deferred.empty());
    };
    const auto messages = loom::async(native.ep);
    const auto rma = loom::rma_async(native.ep);
    SECTION("send") {
        run(messages.send(storage, &native.cq));
    }
    SECTION("receive") {
        run(messages.recv(storage, &native.cq), 0);
    }
    SECTION("tagged send") {
        run(messages.tagged_send(storage, 42, &native.cq));
    }
    SECTION("tagged receive") {
        run(messages.tagged_recv(storage, 42, 0x80, &native.cq), 0);
    }
    SECTION("read") {
        run(rma.read(storage, loom::rma_addr{0x1000}, loom::mr_key{1}, &native.cq), FI_FENCE);
    }
    SECTION("write") {
        run(rma.write(storage, loom::rma_addr{0x1000}, loom::mr_key{1}, &native.cq));
    }
    SECTION("fetch add") {
        run(rma.fetch_add<std::uint64_t>(
            loom::rma_addr{0x1000}, 1, loom::mr_key{1}, nullptr, &native.cq));
    }
    SECTION("compare swap") {
        run(rma.compare_swap<std::uint64_t>(
            loom::rma_addr{0x1000}, 42, 43, loom::mr_key{1}, nullptr, &native.cq));
    }
}

TEST_CASE("core messages retain explicit batching", "[execution][progress][batch]") {
    provider native{0, FI_MORE};
    std::array<std::byte, 16> storage{};
    std::array<fi_context2, 2> contexts{};
    const iovec buffer{storage.data(), storage.size()};
    loom::msg::send_message<> message{.iov = {&buffer, 1}, .context = &contexts[0]};
    REQUIRE(loom::msg::sendmsg<loom::operation_defaults::inherit>(
        native.ep, message, loom::msg::send_flag::completion));
    REQUIRE_FALSE(native.cq.poll());
    REQUIRE(native.deferred.size() == 1);
    message.context = &contexts[1];
    REQUIRE(loom::msg::sendmsg(native.ep, message, loom::msg::send_flag::completion));
    REQUIRE(native.deferred.empty());
    REQUIRE(native.cq.poll().value().context.get() == &contexts[0]);
    REQUIRE(native.cq.poll().value().context.get() == &contexts[1]);
    REQUIRE(native.pending.empty());
}

TEST_CASE("receive senders reject multi-receive defaults", "[execution][progress][message]") {
    const auto tagged = GENERATE(false, true);
    provider native{GENERATE(FI_MULTI_RECV, FI_MULTI_RECV | FI_MORE)};
    std::array<std::byte, 16> storage{};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        CHECK(native.pending.empty());
        CHECK(native.submitted_flags == 0);
        // Retire any erroneously accepted work so the pre-fix regression does
        // not leave a live native context behind when the assertion fails.
        if (!native.pending.empty())
            (void)loom::execution::poll_completions(native.cq).value();
        REQUIRE(state.calls == 1);
        CHECK(state.error == loom::errc::not_supported);
        CHECK(state.bytes == 0);
        REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 0);
    };
    auto operations = loom::async(native.ep);
    if (tagged)
        run(operations.tagged_recv(storage, 42, 0x80, &native.cq));
    else
        run(operations.recv(storage, &native.cq));

    // Receive defaults must not disable transmit operations on this endpoint.
    state = {};
    auto send = loom::execution::connect(operations.send(storage, &native.cq), receiver{&state});
    loom::execution::start(send);
    REQUIRE(native.submitted_flags == (FI_COMPLETION | FI_FENCE));
    REQUIRE(state.calls == 0);
    REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 1);
    REQUIRE(state.calls == 1);
    REQUIRE_FALSE(state.error);
}

TEST_CASE("RMA senders preserve registered peer submissions", "[execution][progress]") {
    const auto write = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    enum class remote_range { valid, short_extent, overflow };
    const auto range =
        GENERATE(remote_range::valid, remote_range::short_extent, remote_range::overflow);
    enum class local_range {
        valid,
        empty,
        short_extent,
        before_start,
        unrelated,
        unmapped,
        foreign_domain,
        unknown_domain,
        disabled
    };
    const auto local = GENERATE(local_range::valid,
                                local_range::empty,
                                local_range::short_extent,
                                local_range::before_start,
                                local_range::unrelated,
                                local_range::unmapped,
                                local_range::foreign_domain,
                                local_range::unknown_domain,
                                local_range::disabled);
    const bool invalid_range = range != remote_range::valid || local != local_range::valid;
    provider fabric{FI_MORE, FI_FENCE | FI_MORE | FI_DELIVERY_COMPLETE};
    fabric.selective_completion = GENERATE(false, true);
    CAPTURE(fabric.selective_completion);
    fabric.submission_status = status;
    std::array<std::byte, 16> storage{}, unrelated{};
    fid_domain foreign_domain{};
    fid_mr native_mr{.fid = {.ops = &fabric.ops}, .mem_desc = &native_mr};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        local == local_range::empty ? nullptr : &native_mr,
        local == local_range::unmapped ? nullptr
        : local == local_range::unrelated
            ? unrelated.data()
            : storage.data() + (local == local_range::before_start ? 1 : 0),
        storage.size() - (local == local_range::short_extent ? 1 : 0),
        loom::detail::mr_key_lease{},
        0,
        local == local_range::disabled ? FI_MR_ENDPOINT : 0,
        local == local_range::foreign_domain   ? &foreign_domain
        : local == local_range::unknown_domain ? nullptr
                                               : &fabric.domain_handle)};
    const loom::fabric_addr peer{19};
    const loom::remote_memory remote{
        range == remote_range::overflow ? std::numeric_limits<std::uint64_t>::max() - 15 : 24,
        0x123456789abcdef0ULL,
        range == remote_range::short_extent ? 15u : 16u};
    completion_state state;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        if (invalid_range) {
            REQUIRE(state.calls == 1);
            REQUIRE(state.error == loom::errc::invalid_argument);
            REQUIRE(fabric.submitted_bytes == 0);
        } else {
            REQUIRE(fabric.submitted_flags ==
                    (FI_COMPLETION | FI_FENCE | (write ? FI_DELIVERY_COMPLETE : 0)));
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

TEST_CASE("registered RMA senders validate the registration used at start",
          "[execution][progress]") {
    const auto write = GENERATE(false, true);
    const auto fits = GENERATE(false, true);
    provider native;
    std::array<std::byte, 16> buffer{}, unrelated{};
    fid_mr first{.fid = {.ops = &native.ops}, .mem_desc = &first};
    fid_mr second{.fid = {.ops = &native.ops}, .mem_desc = &second};
    const auto region = [&](fid_mr& handle, auto& storage) {
        return loom::memory_region{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
            loom::get_default_resource(),
            &handle,
            storage.data(),
            storage.size(),
            loom::detail::mr_key_lease{},
            0,
            0,
            &native.domain_handle)};
    };
    auto mr = region(first, buffer);
    const loom::remote_memory remote{24, 7, buffer.size()};
    completion_state state;
    const auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        // A sender has no native work yet. Validate the borrowed MR at start
        // and submit its matching descriptor after replacement.
        mr = region(second, fits ? buffer : unrelated);
        loom::execution::start(operation);
        if (fits) {
            CHECK(native.local_descriptor == &second);
            REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 1);
            CHECK_FALSE(state.error);
        } else {
            CHECK(native.submitted_bytes == 0);
            CHECK(state.error == loom::errc::invalid_argument);
        }
        CHECK(state.calls == 1);
        CHECK(native.pending.empty());
    };
    const auto operations = loom::rma_async(native.ep);
    if (write)
        run(operations.write_to(buffer, mr, loom::fabric_addr{19}, remote, &native.cq));
    else
        run(operations.read_from(buffer, mr, loom::fabric_addr{19}, remote, &native.cq));
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
    native_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
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
    const auto remote = remote_memory::from_mr(*target_mr).value();
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
    fabric.selective_completion = GENERATE(false, true);
    fabric.submission_status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    completion_state state;
    std::uint64_t result{};
    int descriptor{};
    fid_mr native_mr{.fid = {.ops = &fabric.ops}, .mem_desc = &descriptor};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        &native_mr,
        &result,
        sizeof(result),
        loom::detail::mr_key_lease{},
        0,
        0,
        &fabric.domain_handle)};
    fabric.expected_result = &result;
    fabric.expected_result_descriptor = &descriptor;
    auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        CHECK(fabric.submitted_flags == (FI_COMPLETION | FI_FENCE));
        REQUIRE(result == 0);
        if (fabric.submission_status) {
            REQUIRE(state.calls == 1);
            REQUIRE(state.error == loom::make_error_code_from_fi_errno(-fabric.submission_status));
            REQUIRE(fabric.pending.empty());
            REQUIRE(fabric.atomic_pending.empty());
            return;
        }
        REQUIRE(state.calls == 0);
        REQUIRE(fabric.atomic_pending.size() == 1);
        REQUIRE(loom::execution::poll_completions(fabric.cq).value().completed == 1);
        REQUIRE(state.calls == 1);
        REQUIRE_FALSE(state.error);
        REQUIRE(state.bytes == 42);
        REQUIRE(result == 42);
        REQUIRE(fabric.remote_value == 43);
        REQUIRE(fabric.atomic_pending.empty());
    };
    if (compare_swap) {
        run(loom::rma_async(fabric.ep).compare_swap<std::uint64_t>(
            loom::rma_addr{0x1000}, 42, 43, loom::mr_key{1}, &mr, &fabric.cq));
    } else {
        run(loom::rma_async(fabric.ep).fetch_add<std::uint64_t>(
            loom::rma_addr{0x1000}, 1, loom::mr_key{1}, &mr, &fabric.cq));
    }
}

TEST_CASE("atomic senders borrow registered operands for explicit peers",
          "[execution][progress][atomic]") {
    const auto compare_swap = GENERATE(false, true);
    const auto registered = GENERATE(false, true);
    provider native{0, FI_MORE | FI_FENCE | FI_DELIVERY_COMPLETE};
    native.selective_completion = true;
    native.submission_status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    native.expected_atomic_peer = GENERATE(fi_addr_t{37}, FI_ADDR_UNSPEC);
    std::array<std::uint64_t, 3> inputs{0, 3, 0}, comparisons{0, 42, 0}, results{};
    const auto& input = inputs[1];
    const auto& comparison = comparisons[1];
    auto& result = results[1];
    fid_mr input_handle{.fid = {.ops = &native.ops}, .mem_desc = &input_handle};
    fid_mr compare_handle{.fid = {.ops = &native.ops}, .mem_desc = &compare_handle};
    fid_mr result_handle{.fid = {.ops = &native.ops}, .mem_desc = &result_handle};
    const auto region = [&](auto& values, fid_mr& handle) {
        return loom::memory_region{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
            loom::get_default_resource(),
            &handle,
            values.data(),
            sizeof(values),
            loom::detail::mr_key_lease{},
            0,
            0,
            &native.domain_handle)};
    };
    const auto input_mr = region(inputs, input_handle);
    const auto compare_mr = region(comparisons, compare_handle);
    const auto result_mr = region(results, result_handle);
    native.expected_atomic_input = &input;
    native.expected_atomic_input_descriptor = registered ? &input_handle : nullptr;
    native.expected_atomic_comparison = compare_swap ? &comparison : nullptr;
    native.expected_atomic_comparison_descriptor =
        registered && compare_swap ? &compare_handle : nullptr;
    native.expected_result = &result;
    native.expected_result_descriptor = registered ? &result_handle : nullptr;
    const loom::fabric_addr peer{native.expected_atomic_peer};
    const loom::remote_memory remote{0x1000, 1, sizeof(result)};
    completion_state state;
    auto run = [&](auto make_sender) {
        auto operation = [&] {
            auto sender = make_sender();
            static_assert(loom::execution::sender<decltype(sender)>);
            return loom::execution::connect(std::move(sender), receiver{&state});
        }();
        loom::execution::start(operation);
        REQUIRE(native.submitted_flags == (FI_COMPLETION | FI_FENCE | FI_DELIVERY_COMPLETE));
        REQUIRE(result == 0);
        if (native.submission_status) {
            REQUIRE(state.calls == 1);
            REQUIRE(state.error == loom::make_error_code_from_fi_errno(-native.submission_status));
            REQUIRE_FALSE(native.cq.dispatch(loom::completion_event{
                .context = loom::context_ptr<void>{native.submitted_context}}));
        } else {
            REQUIRE(state.calls == 0);
            REQUIRE(native.atomic_pending.size() == 1);
            REQUIRE(loom::execution::poll_completions(native.cq).value().completed == 1);
            REQUIRE(state.calls == 1);
            REQUIRE_FALSE(state.error);
            REQUIRE(state.bytes == 42);
            REQUIRE(result == 42);
            REQUIRE(native.remote_value == (compare_swap ? 3 : 45));
        }
        REQUIRE(native.pending.empty());
        REQUIRE(native.atomic_pending.empty());
    };
    const auto operations = loom::rma_async(native.ep);
    if (compare_swap) {
        run([&] {
            return operations.compare_swap_to(comparison,
                                              registered ? &compare_mr : nullptr,
                                              input,
                                              registered ? &input_mr : nullptr,
                                              result,
                                              registered ? &result_mr : nullptr,
                                              peer,
                                              remote,
                                              &native.cq);
        });
    } else {
        run([&] {
            return operations.fetch_add_to(input,
                                           registered ? &input_mr : nullptr,
                                           result,
                                           registered ? &result_mr : nullptr,
                                           peer,
                                           remote,
                                           &native.cq);
        });
    }
}

TEST_CASE("borrowed atomic senders reject invalid registered ranges",
          "[execution][progress][atomic]") {
    const auto which = GENERATE(0, 1, 2);
    const auto invalid = GENERATE(0, 1, 2, 3, 4);
    const auto compare_swap = GENERATE(false, true);
    if (!compare_swap && which == 1)
        return;
    provider native;
    std::array<std::uint64_t, 3> values{3, 42, 0};
    std::array<fid_mr, 3> handles;
    std::array<loom::memory_region, 3> regions;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        handles[i] = {.fid = {.ops = &native.ops}, .mem_desc = &handles[i]};
        void* address = &values[i];
        std::size_t length = sizeof(std::uint64_t);
        auto* handle = &handles[i];
        if (i == static_cast<std::size_t>(which)) {
            switch (invalid) {
                case 0:
                    handle = nullptr;
                    break;
                case 1:
                    address = nullptr;
                    break;
                case 2:
                    --length;
                    break;
                case 3:
                    address = values.data() + i + 1;
                    break;
                case 4:
                    length = 0;
                    break;
            }
        }
        regions[i] =
            loom::memory_region{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
                loom::get_default_resource(),
                handle,
                address,
                length,
                loom::detail::mr_key_lease{},
                0,
                0,
                &native.domain_handle)};
    }
    completion_state state;
    const auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        REQUIRE(state.calls == 1);
        REQUIRE(state.error == loom::errc::invalid_argument);
        REQUIRE(native.submitted_flags == 0);
        REQUIRE(native.pending.empty());
        REQUIRE(native.atomic_pending.empty());
    };
    const auto operations = loom::rma_async(native.ep);
    const loom::remote_memory remote{0x1000, 1, sizeof(std::uint64_t)};
    if (compare_swap)
        run(operations.compare_swap_to(values[1],
                                       &regions[1],
                                       values[0],
                                       &regions[0],
                                       values[2],
                                       &regions[2],
                                       loom::fabric_addr{19},
                                       remote,
                                       &native.cq));
    else
        run(operations.fetch_add_to(values[0],
                                    &regions[0],
                                    values[2],
                                    &regions[2],
                                    loom::fabric_addr{19},
                                    remote,
                                    &native.cq));
}

TEST_CASE("borrowed atomic senders require valid endpoints and remote ranges",
          "[execution][progress][atomic]") {
    const auto compare_swap = GENERATE(false, true);
    const auto invalid = GENERATE(0, 1, 2, 3);
    provider native;
    if (invalid == 0)
        native.ep = {};
    if (invalid == 1)
        native.cq = {};
    std::uint64_t input{3}, comparison{42}, result{};
    completion_state state;
    const auto run = [&](auto sender) {
        auto operation = loom::execution::connect(std::move(sender), receiver{&state});
        loom::execution::start(operation);
        REQUIRE(state.calls == 1);
        REQUIRE(state.error == (invalid >= 2
                                    ? loom::make_error_code(loom::errc::invalid_argument)
                                    : std::make_error_code(std::errc::bad_file_descriptor)));
        REQUIRE(native.submitted_flags == 0);
        REQUIRE(native.pending.empty());
    };
    const auto operations = loom::rma_async(native.ep);
    const loom::remote_memory remote{
        invalid == 3 ? std::numeric_limits<std::uint64_t>::max() - sizeof(result) + 1 : 0x1000,
        1,
        invalid == 2 ? sizeof(result) - 1 : sizeof(result)};
    if (compare_swap)
        run(operations.compare_swap_to(comparison,
                                       nullptr,
                                       input,
                                       nullptr,
                                       result,
                                       nullptr,
                                       loom::fabric_addr{19},
                                       remote,
                                       &native.cq));
    else
        run(operations.fetch_add_to(
            input, nullptr, result, nullptr, loom::fabric_addr{19}, remote, &native.cq));
}

TEST_CASE("borrowed atomic senders update a registered RDM peer",
          "[execution][progress][atomic][loopback]") {
    using namespace loom;
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::atomic;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    static_cast<fi_info*>(info->internal_ptr())->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
    auto fab = fabric::create(*info).value();
    auto dom = domain::create(fab, *info).value();
    auto cq = completion_queue::create(dom).value();
    auto av = address_vector::create(dom).value();
    auto source = endpoint::create(dom, *info).value();
    auto target = endpoint::create(dom, *info).value();
    for (auto* ep : {&source, &target}) {
        REQUIRE(ep->bind_cq(cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
        REQUIRE(ep->bind_av(av));
        REQUIRE(ep->enable());
    }
    const fabric_addr peer{av.insert(target.get_local_address().value()).value().value};
    std::array<std::uint64_t, 3> local{3, 46, 0};
    std::uint64_t remote_value{42};
    const auto local_mr =
        memory_region::register_memory(dom,
                                       std::as_writable_bytes(std::span{local}),
                                       mr_access_flags::read | mr_access_flags::write)
            .value();
    auto remote_mr =
        memory_region::register_memory(dom,
                                       std::as_writable_bytes(std::span{&remote_value, 1}),
                                       mr_access_flags::remote_read | mr_access_flags::remote_write)
            .value();
    const auto remote = remote_memory::from_mr(remote_mr).value();
    std::size_t limit{};
    REQUIRE(
        atomic::is_fetch_valid(source, atomic::operation::sum, atomic::datatype::uint64, &limit));
    REQUIRE(limit >= 1);
    REQUIRE(atomic::is_compare_valid(
        source, atomic::operation::compare_swap, atomic::datatype::uint64, &limit));
    REQUIRE(limit >= 1);
    completion_state state;
    const auto run = [&](auto sender) {
        state = {};
        auto operation = execution::connect(std::move(sender), receiver{&state});
        execution::start(operation);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (state.calls == 0 && std::chrono::steady_clock::now() < deadline) {
            const auto progress = execution::poll_completions(cq);
            if (!progress || progress->unhandled)
                break;
            std::this_thread::yield();
        }
        if (state.calls == 0) {
            REQUIRE(source.close());
            REQUIRE(target.close());
            FAIL("RDM atomic sender did not complete");
        }
        REQUIRE(state.calls == 1);
        REQUIRE_FALSE(state.error);
        REQUIRE(state.bytes == local[2]);
    };
    const auto operations = rma_async(source);
    run(operations.fetch_add_to(local[0], &local_mr, local[2], &local_mr, peer, remote, &cq));
    REQUIRE(local[2] == 42);
    REQUIRE(remote_value == 45);
    run(operations.compare_swap_to(
        local[1], &local_mr, local[0], &local_mr, local[2], &local_mr, peer, remote, &cq));
    REQUIRE(local[2] == 45);
    REQUIRE(remote_value == 45);
    local[1] = 45;
    run(operations.compare_swap_to(
        local[1], &local_mr, local[0], &local_mr, local[2], &local_mr, peer, remote, &cq));
    REQUIRE(local[2] == 45);
    REQUIRE(remote_value == 3);
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
        invalid == 3 ? 1 : storage.size(),
        loom::detail::mr_key_lease{},
        0,
        0,
        &fabric.domain_handle)};
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
