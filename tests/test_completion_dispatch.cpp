// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace {
struct queue_fixture {
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fid_cq handle{.fid = {.ops = &ops}};
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &handle)};
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
    std::array<std::jthread, 4> workers;
    for (auto& worker : workers) {
        worker = std::jthread{[&] {
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
