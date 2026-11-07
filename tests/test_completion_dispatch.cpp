// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <random>
#include <thread>

#include "joining_thread.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
struct queue_fixture {
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fid_cq handle{.fid = {.ops = &ops}};
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &handle)};
};
struct endpoint_fixture {
    int bind_status{};
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
        .bind =
            [](fid* handle, fid*, std::uint64_t) {
                return static_cast<endpoint_fixture*>(handle->context)->bind_status;
            },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}};
    loom::endpoint endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
        loom::get_default_resource(),
        &handle,
        loom::endpoint_types::rdm,
        loom::address_format::unspecified)};
};
struct observer {
    int calls{};
    loom::completion_event event;
    static void complete(void* pointer, const loom::completion_event& event) noexcept {
        auto& self = *static_cast<observer*>(pointer);
        ++self.calls;
        self.event = event;
    }
};
auto completion(void* pointer) -> loom::completion_event {
    loom::completion_event event{};
    event.context = loom::context_ptr<void>{pointer};
    event.bytes_transferred = 7;
    event.tag = 13;
    event.data = 17;
    event.source = loom::fabric_addr{19};
    return event;
}
}  // namespace

TEST_CASE("completion dispatch never dereferences foreign contexts", "[cq][dispatch]") {
    queue_fixture fixture;
    observer receiver;
    fi_context2 own{}, foreign{};
    loom::completion_registration registration;
    REQUIRE(registration.bind(
        fixture.queue, loom::context_ptr<void>{&own}, &receiver, observer::complete));
    for (auto* pointer : std::array<void*, 3>{nullptr, &foreign, reinterpret_cast<void*>(1)}) {
        auto event = completion(pointer);
        event.error = loom::errc::io_error;
        REQUIRE_FALSE(fixture.queue.dispatch(event));
        REQUIRE(event.context.get() == pointer);
        REQUIRE(event.error == loom::errc::io_error);
        REQUIRE(event.source == loom::fabric_addr{19});
        REQUIRE(receiver.calls == 0);
    }
    queue_fixture other;
    REQUIRE_FALSE(other.queue.dispatch(completion(&own)));
    REQUIRE(receiver.calls == 0);
    auto event = completion(&own);
    event.error = loom::errc::canceled;
    REQUIRE(fixture.queue.dispatch(event));
    REQUIRE(receiver.calls == 1);
    REQUIRE(receiver.event.error == loom::errc::canceled);
    REQUIRE(receiver.event.tag == 13);
    REQUIRE(receiver.event.source == loom::fabric_addr{19});
    REQUIRE_FALSE(fixture.queue.dispatch(event));
    REQUIRE(receiver.calls == 1);
}

TEST_CASE("completion registrations reject duplicate ownership", "[cq][dispatch]") {
    queue_fixture fixture;
    observer receiver;
    fi_context2 context{};
    loom::completion_registration first, second;
    const auto key = loom::context_ptr<void>{&context};
    REQUIRE(first.bind(fixture.queue, key, &receiver, observer::complete));
    REQUIRE(first.bind(fixture.queue, key, &receiver, observer::complete).error() ==
            loom::errc::busy);
    REQUIRE(second.bind(fixture.queue, key, &receiver, observer::complete).error() ==
            loom::errc::address_in_use);
    first.reset();
    first.reset();
    REQUIRE_FALSE(fixture.queue.dispatch(completion(&context)));
    REQUIRE(second.bind(fixture.queue, key, &receiver, observer::complete));
    REQUIRE(fixture.queue.dispatch(completion(&context)));
    REQUIRE(receiver.calls == 1);
    loom::completion_queue empty;
    REQUIRE_FALSE(first.bind(empty, key, &receiver, observer::complete));
    REQUIRE_FALSE(first.bind(fixture.queue, {}, &receiver, observer::complete));
    REQUIRE_FALSE(first.bind(fixture.queue, key, &receiver, nullptr));
}

TEST_CASE("completion registration removal preserves other entries", "[cq][dispatch]") {
    queue_fixture fixture;
    std::array<fi_context2, 4> contexts{};
    std::array<observer, 4> receivers{};
    std::array<loom::completion_registration, 4> registrations;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        REQUIRE(registrations[i].bind(fixture.queue,
                                      loom::context_ptr<void>{&contexts[i]},
                                      &receivers[i],
                                      observer::complete));
    }
    registrations[1].reset();                                   // Middle
    REQUIRE(fixture.queue.dispatch(completion(&contexts[0])));  // Tail
    REQUIRE(fixture.queue.dispatch(completion(&contexts[3])));  // Head
    REQUIRE(fixture.queue.dispatch(completion(&contexts[2])));
    REQUIRE_FALSE(fixture.queue.dispatch(completion(&contexts[1])));
    REQUIRE(receivers[0].calls == 1);
    REQUIRE(receivers[1].calls == 0);
    REQUIRE(receivers[2].calls == 1);
    REQUIRE(receivers[3].calls == 1);
}

TEST_CASE("dispatch permits callback destruction and reentrant registration", "[cq][dispatch]") {
    queue_fixture fixture;
    struct state {
        loom::completion_queue* queue;
        std::unique_ptr<loom::completion_registration> first;
        loom::completion_registration second;
        fi_context2 first_context{}, second_context{};
        observer receiver;
        bool rebound{};
        bool dispatched{};
    } state{&fixture.queue, std::make_unique<loom::completion_registration>()};
    REQUIRE(state.first->bind(fixture.queue,
                              loom::context_ptr<void>{&state.first_context},
                              &state,
                              [](void* pointer, const loom::completion_event&) noexcept {
                                  auto& self = *static_cast<struct state*>(pointer);
                                  self.first.reset();
                                  self.rebound = static_cast<bool>(self.second.bind(
                                      *self.queue,
                                      loom::context_ptr<void>{&self.second_context},
                                      &self.receiver,
                                      observer::complete));
                                  self.dispatched =
                                      self.queue->dispatch(completion(&self.second_context));
                              }));
    REQUIRE(fixture.queue.dispatch(completion(&state.first_context)));
    REQUIRE_FALSE(state.first);
    REQUIRE(state.rebound);
    REQUIRE(state.dispatched);
    REQUIRE(state.receiver.calls == 1);
}

TEST_CASE("completion registrations follow native queue ownership", "[cq][dispatch]") {
    loom::completion_registration registration;
    observer receiver;
    fi_context2 context{};
    {
        queue_fixture fixture;
        REQUIRE(registration.bind(
            fixture.queue, loom::context_ptr<void>{&context}, &receiver, observer::complete));
        loom::completion_queue moved{std::move(fixture.queue)};
        REQUIRE_FALSE(fixture.queue.dispatch(completion(&context)));
        REQUIRE(moved.dispatch(completion(&context)));
        REQUIRE(receiver.calls == 1);
        REQUIRE(registration.bind(
            moved, loom::context_ptr<void>{&context}, &receiver, observer::complete));
    }
    // Queue destruction detaches the registration without calling user code.
    registration.reset();
    REQUIRE(receiver.calls == 1);
    queue_fixture replacement;
    REQUIRE(registration.bind(
        replacement.queue, loom::context_ptr<void>{&context}, &receiver, observer::complete));
    REQUIRE(replacement.queue.dispatch(completion(&context)));
    REQUIRE(receiver.calls == 2);
}

TEST_CASE("separate completion registrations support concurrent progress", "[cq][dispatch]") {
    queue_fixture fixture;
    std::atomic<int> calls{};
    std::atomic<bool> valid{true};
    std::array<loom::test::joining_thread, 4> workers;
    for (auto& worker : workers) {
        worker = loom::test::joining_thread{[&] {
            for (int i = 0; i < 256; ++i) {
                fi_context2 context{};
                loom::completion_registration registration;
                auto bound =
                    registration.bind(fixture.queue,
                                      loom::context_ptr<void>{&context},
                                      &calls,
                                      [](void* pointer, const loom::completion_event&) noexcept {
                                          static_cast<std::atomic<int>*>(pointer)->fetch_add(1);
                                      });
                if (!bound || !fixture.queue.dispatch(completion(&context))) {
                    valid.store(false);
                }
            }
        }};
    }
    for (auto& worker : workers)
        worker.join();
    REQUIRE(valid.load());
    REQUIRE(calls.load() == 1024);
}

TEST_CASE("endpoint completion bindings survive wrapper moves", "[cq][dispatch][binding]") {
    queue_fixture tx, rx;
    endpoint_fixture fixture;
    fi_context2 tx_context{}, rx_context{};
    observer tx_receiver, rx_receiver;
    loom::completion_registration tx_registration, rx_registration;
    REQUIRE_FALSE(tx_registration.bind(fixture.endpoint,
                                       loom::completion_direction::transmit,
                                       loom::context_ptr<void>{&tx_context},
                                       &tx_receiver,
                                       observer::complete));
    REQUIRE_FALSE(rx_registration.bind(fixture.endpoint,
                                       loom::completion_direction::receive,
                                       loom::context_ptr<void>{&rx_context},
                                       &rx_receiver,
                                       observer::complete));
    REQUIRE(fixture.endpoint.bind_cq(
        tx.queue, loom::cq_bind::transmit | loom::cq_bind::selective_completion));
    REQUIRE(fixture.endpoint.bind_rx_cq(rx.queue));
    REQUIRE(fixture.endpoint.bound_cq(loom::completion_direction::transmit) == &tx.queue);
    REQUIRE(fixture.endpoint.bound_cq(loom::completion_direction::receive) == &rx.queue);
    auto endpoint = std::move(fixture.endpoint);
    auto transmit = std::move(tx.queue);
    auto receive = std::move(rx.queue);
    REQUIRE(endpoint.bound_cq(loom::completion_direction::transmit) == &transmit);
    REQUIRE(endpoint.bound_cq(loom::completion_direction::receive) == &receive);
    tx.queue = std::move(transmit);
    rx.queue = std::move(receive);
    REQUIRE(endpoint.bound_cq(loom::completion_direction::transmit) == &tx.queue);
    REQUIRE(endpoint.bound_cq(loom::completion_direction::receive) == &rx.queue);
    transmit = std::move(tx.queue);
    receive = std::move(rx.queue);
    REQUIRE(endpoint.bound_cq(loom::completion_direction::transmit) == &transmit);
    REQUIRE(endpoint.bound_cq(loom::completion_direction::receive) == &receive);
    REQUIRE_FALSE(fixture.endpoint.bound_cq(loom::completion_direction::transmit));
    REQUIRE_FALSE(tx_registration.bind(fixture.endpoint,
                                       loom::completion_direction::transmit,
                                       loom::context_ptr<void>{&tx_context},
                                       &tx_receiver,
                                       observer::complete));
    REQUIRE(tx_registration.bind(endpoint,
                                 loom::completion_direction::transmit,
                                 loom::context_ptr<void>{&tx_context},
                                 &tx_receiver,
                                 observer::complete));
    REQUIRE(rx_registration.bind(endpoint,
                                 loom::completion_direction::receive,
                                 loom::context_ptr<void>{&rx_context},
                                 &rx_receiver,
                                 observer::complete));
    REQUIRE_FALSE(transmit.dispatch(completion(&rx_context)));
    REQUIRE_FALSE(receive.dispatch(completion(&tx_context)));
    REQUIRE(transmit.dispatch(completion(&tx_context)));
    REQUIRE(receive.dispatch(completion(&rx_context)));
    REQUIRE(tx_receiver.calls == 1);
    REQUIRE(rx_receiver.calls == 1);
    REQUIRE_FALSE(tx_registration.bind(endpoint,
                                       static_cast<loom::completion_direction>(255),
                                       loom::context_ptr<void>{&tx_context},
                                       &tx_receiver,
                                       observer::complete));
}

TEST_CASE("failed native binding preserves the previous completion route",
          "[cq][dispatch][binding]") {
    queue_fixture original, replacement;
    endpoint_fixture fixture;
    fi_context2 context{};
    observer receiver;
    loom::completion_registration registration;
    REQUIRE(
        fixture.endpoint.bind_cq(original.queue, loom::cq_bind::transmit | loom::cq_bind::recv));
    fixture.bind_status = -FI_EIO;
    const auto rebound =
        fixture.endpoint.bind_cq(replacement.queue, loom::cq_bind::transmit | loom::cq_bind::recv);
    REQUIRE_FALSE(rebound);
    REQUIRE(rebound.error() == loom::errc::io_error);
    for (auto direction :
         {loom::completion_direction::transmit, loom::completion_direction::receive}) {
        REQUIRE(registration.bind(fixture.endpoint,
                                  direction,
                                  loom::context_ptr<void>{&context},
                                  &receiver,
                                  observer::complete));
        REQUIRE_FALSE(replacement.queue.dispatch(completion(&context)));
        REQUIRE(original.queue.dispatch(completion(&context)));
    }
    fixture.bind_status = 0;
    REQUIRE(fixture.endpoint.bind_tx_cq(replacement.queue));
    REQUIRE(registration.bind(fixture.endpoint,
                              loom::completion_direction::transmit,
                              loom::context_ptr<void>{&context},
                              &receiver,
                              observer::complete));
    REQUIRE_FALSE(original.queue.dispatch(completion(&context)));
    REQUIRE(replacement.queue.dispatch(completion(&context)));
    REQUIRE(registration.bind(fixture.endpoint,
                              loom::completion_direction::receive,
                              loom::context_ptr<void>{&context},
                              &receiver,
                              observer::complete));
    REQUIRE_FALSE(replacement.queue.dispatch(completion(&context)));
    REQUIRE(original.queue.dispatch(completion(&context)));
    REQUIRE(receiver.calls == 4);
}

TEST_CASE("destroyed resources invalidate endpoint completion bindings",
          "[cq][dispatch][binding]") {
    endpoint_fixture first, second;
    fi_context2 context{};
    observer receiver;
    loom::completion_registration registration;
    {
        queue_fixture queue;
        REQUIRE(first.endpoint.bind_cq(queue.queue, loom::cq_bind::transmit | loom::cq_bind::recv));
        REQUIRE(
            second.endpoint.bind_cq(queue.queue, loom::cq_bind::transmit | loom::cq_bind::recv));
        // Native mocks permit teardown without real outstanding I/O. Erasing
        // the queue must invalidate both endpoints' observations, not dangle.
    }
    for (auto* endpoint : {&first.endpoint, &second.endpoint}) {
        for (auto direction :
             {loom::completion_direction::transmit, loom::completion_direction::receive}) {
            REQUIRE_FALSE(endpoint->bound_cq(direction));
            REQUIRE_FALSE(registration.bind(*endpoint,
                                            direction,
                                            loom::context_ptr<void>{&context},
                                            &receiver,
                                            observer::complete));
        }
    }
    queue_fixture replacement;
    REQUIRE(first.endpoint.bind_tx_cq(replacement.queue));
    REQUIRE(registration.bind(first.endpoint,
                              loom::completion_direction::transmit,
                              loom::context_ptr<void>{&context},
                              &receiver,
                              observer::complete));
    REQUIRE(replacement.queue.dispatch(completion(&context)));
    REQUIRE(first.endpoint.close());
    REQUIRE_FALSE(registration.bind(first.endpoint,
                                    loom::completion_direction::transmit,
                                    loom::context_ptr<void>{&context},
                                    &receiver,
                                    observer::complete));
    REQUIRE(receiver.calls == 1);
}

TEST_CASE("endpoint close detaches bindings without disturbing other endpoints",
          "[cq][dispatch][binding]") {
    queue_fixture queue;
    endpoint_fixture first, second;
    REQUIRE(first.endpoint.bind_cq(queue.queue, loom::cq_bind::transmit | loom::cq_bind::recv));
    REQUIRE(second.endpoint.bind_cq(queue.queue, loom::cq_bind::transmit | loom::cq_bind::recv));
    REQUIRE(first.endpoint.close());
    fi_context2 context{};
    observer receiver;
    loom::completion_registration registration;
    REQUIRE(registration.bind(second.endpoint,
                              loom::completion_direction::receive,
                              loom::context_ptr<void>{&context},
                              &receiver,
                              observer::complete));
    REQUIRE(queue.queue.dispatch(completion(&context)));
    REQUIRE(receiver.calls == 1);
}

TEST_CASE("completion callback exceptions propagate after detachment", "[cq][dispatch]") {
    struct callback_error {};
    queue_fixture fixture;
    endpoint_fixture endpoint;
    REQUIRE(endpoint.endpoint.bind_tx_cq(fixture.queue));
    fi_context2 context{}, other_context{};
    auto throwing = std::make_unique<loom::completion_registration>();
    loom::completion_registration other;
    observer receiver;
    REQUIRE(other.bind(
        fixture.queue, loom::context_ptr<void>{&other_context}, &receiver, observer::complete));
    REQUIRE(throwing->bind(
        endpoint.endpoint,
        loom::completion_direction::transmit,
        loom::context_ptr<void>{&context},
        &throwing,
        [](void* pointer, const loom::completion_event&) {
            // The callback may destroy itself before throwing to its progress loop.
            static_cast<std::unique_ptr<loom::completion_registration>*>(pointer)->reset();
            throw callback_error{};
        }));
    REQUIRE_THROWS_AS(fixture.queue.dispatch(completion(&context)), callback_error);
    REQUIRE_FALSE(throwing);
    REQUIRE_FALSE(fixture.queue.dispatch(completion(&context)));
    // No lock or detached entry is left behind, and other work still completes.
    REQUIRE(fixture.queue.dispatch(completion(&other_context)));
    REQUIRE(receiver.calls == 1);
}

TEST_CASE("completion registry preserves ownership across mixed operations", "[cq][dispatch]") {
    queue_fixture fixture;
    constexpr std::size_t count = 193;
    std::array<fi_context2, count> contexts{};
    std::array<observer, count> receivers{};
    std::array<loom::completion_registration, count> registrations;
    std::array<bool, count> pending{};
    std::array<int, count> completed{};
    std::mt19937 random{1729};
    for (int step = 0; step < 10000; ++step) {
        const auto index = random() % count;
        const auto action = random() % 5;
        CAPTURE(step, index, action);
        const auto context = loom::context_ptr<void>{&contexts[index]};
        switch (action) {
            case 0: {
                const auto bound = registrations[index].bind(
                    fixture.queue, context, &receivers[index], observer::complete);
                CHECK(static_cast<bool>(bound) == !pending[index]);
                if (pending[index])
                    CHECK(bound.error() == loom::errc::busy);
                pending[index] = true;
                break;
            }
            case 1:
                CHECK(fixture.queue.dispatch(completion(context.get())) == pending[index]);
                completed[index] += pending[index];
                pending[index] = false;
                break;
            case 2:
                registrations[index].reset();
                pending[index] = false;
                break;
            case 3: {
                loom::completion_registration duplicate;
                const auto bound =
                    duplicate.bind(fixture.queue, context, &receivers[index], observer::complete);
                CHECK(static_cast<bool>(bound) == !pending[index]);
                if (pending[index])
                    CHECK(bound.error() == loom::errc::address_in_use);
                break;
            }
            default:
                CHECK_FALSE(fixture.queue.dispatch(
                    completion(reinterpret_cast<std::byte*>(&contexts[index]) + 1)));
                break;
        }
        CHECK(receivers[index].calls == completed[index]);
    }
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(fixture.queue.dispatch(completion(&contexts[i])) == pending[i]);
        completed[i] += pending[i];
        CHECK(receivers[i].calls == completed[i]);
    }
}

TEST_CASE("large completion registries detach without invoking callbacks", "[cq][dispatch]") {
    constexpr std::size_t count = 4096;
    std::array<fi_context2, count> contexts{};
    std::array<loom::completion_registration, count> registrations;
    observer receiver;
    {
        queue_fixture fixture;
        for (std::size_t i = 0; i < count; ++i)
            REQUIRE(registrations[i].bind(fixture.queue,
                                          loom::context_ptr<void>{&contexts[i]},
                                          &receiver,
                                          observer::complete));
        // Misses and interior removals must not lose still-owned entries.
        CHECK_FALSE(fixture.queue.dispatch(completion(reinterpret_cast<void*>(1))));
        for (std::size_t i = 1; i < count; i += 2)
            registrations[i].reset();
        CHECK_FALSE(fixture.queue.dispatch(completion(reinterpret_cast<void*>(-1))));
    }
    CHECK(receiver.calls == 0);
    queue_fixture replacement;
    for (std::size_t i = 0; i < count; ++i)
        REQUIRE(registrations[i].bind(replacement.queue,
                                      loom::context_ptr<void>{&contexts[i]},
                                      &receiver,
                                      observer::complete));
    for (std::size_t i = count; i > 0; --i)
        REQUIRE(replacement.queue.dispatch(completion(&contexts[i - 1])));
    CHECK(receiver.calls == count);
}

TEST_CASE("completion callback can immediately reuse its registration and context",
          "[cq][dispatch]") {
    queue_fixture fixture;
    struct state {
        loom::completion_queue* queue;
        loom::completion_registration registration;
        fi_context2 context{};
        int calls{};
        bool rebound{};
        static void complete(void* object, const loom::completion_event&) {
            auto& self = *static_cast<state*>(object);
            if (++self.calls == 1)
                self.rebound = static_cast<bool>(self.registration.bind(
                    *self.queue, loom::context_ptr<void>{&self.context}, &self, complete));
        }
    } receiver{&fixture.queue};
    REQUIRE(receiver.registration.bind(
        fixture.queue, loom::context_ptr<void>{&receiver.context}, &receiver, state::complete));
    REQUIRE(fixture.queue.dispatch(completion(&receiver.context)));
    CHECK(receiver.calls == 1);
    REQUIRE(receiver.rebound);
    REQUIRE(fixture.queue.dispatch(completion(&receiver.context)));
    CHECK(receiver.calls == 2);
    CHECK_FALSE(fixture.queue.dispatch(completion(&receiver.context)));
}
