// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>
#include <loom/core/passive_endpoint.hpp>
#include <loom/core/event_queue.hpp>
#include <loom/async/completion_queue.hpp>

#include <tuple>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("public headers support empty resource lifetimes", "[resources]") {
    std::tuple<loom::fabric_info, loom::fabric, loom::domain, loom::counter,
               loom::address_vector, loom::completion_queue, loom::event_queue,
               loom::shared_tx_context, loom::shared_rx_context, loom::collective::group,
               loom::passive_endpoint, loom::scalable_endpoint, loom::memory_region> resources;
    const auto check = [](const auto& resource) { CHECK_FALSE(static_cast<bool>(resource)); };
    std::apply([&check](const auto&... resource) { (check(resource), ...); },
               resources);
}
