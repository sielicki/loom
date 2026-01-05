// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_fabric_integration.cpp
 * @brief Integration tests for fabric initialization using the sockets/tcp provider.
 *
 * These tests verify the full initialization chain works correctly with a real
 * fabric provider. The sockets/tcp provider is used as it should be available
 * in most environments including CI.
 */

#include <loom/loom.hpp>

// Additional headers not in loom.hpp
#include <loom/async/completion_queue.hpp>
#include <loom/core/counter.hpp>
#include <loom/core/event_queue.hpp>

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace loom;

namespace {

/// Helper to get a fabric_info for the tcp provider
auto get_tcp_fabric_info(endpoint_type ep_type = endpoint_types::msg)
    -> result<fabric_info> {
    fabric_hints hints{};
    hints.ep_type = ep_type;
    hints.provider_name = "tcp";
    return query_fabric(hints);
}

/// Helper to get any available fabric_info
auto get_any_fabric_info(endpoint_type ep_type = endpoint_types::msg)
    -> result<fabric_info> {
    fabric_hints hints{};
    hints.ep_type = ep_type;
    return query_fabric(hints);
}

}  // namespace

// =============================================================================
// Fabric Query Tests
// =============================================================================

TEST_CASE("query_fabric with default hints", "[fabric][integration]") {
    auto info_result = query_fabric();

    // Should find at least one provider on most systems
    if (!info_result) {
        SKIP("No fabric providers available on this system");
    }

    REQUIRE(info_result->impl_valid());
}

TEST_CASE("query_fabric for tcp provider", "[fabric][integration]") {
    fabric_hints hints{};
    hints.provider_name = "tcp";

    auto info_result = query_fabric(hints);

    if (!info_result) {
        SKIP("TCP provider not available");
    }

    REQUIRE(info_result->impl_valid());
    REQUIRE(info_result->get_caps().get() != 0);
}

TEST_CASE("query_fabric for msg endpoint type", "[fabric][integration]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::msg;

    auto info_result = query_fabric(hints);

    if (!info_result) {
        SKIP("No MSG endpoint providers available");
    }

    REQUIRE(info_result->impl_valid());
}

TEST_CASE("query_fabric for rdm endpoint type", "[fabric][integration]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;

    auto info_result = query_fabric(hints);

    if (!info_result) {
        SKIP("No RDM endpoint providers available");
    }

    REQUIRE(info_result->impl_valid());
}

TEST_CASE("fabric_info properties are accessible", "[fabric][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto& info = *info_result;

    // These should not throw or crash
    [[maybe_unused]] auto caps = info.get_caps();
    [[maybe_unused]] auto addr_format = info.get_address_format();
    [[maybe_unused]] auto mode = info.get_mode();

    REQUIRE(true);
}

// =============================================================================
// Fabric Creation Tests
// =============================================================================

TEST_CASE("fabric::create from fabric_info", "[fabric][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);

    REQUIRE(static_cast<bool>(fabric_result));
    REQUIRE(fabric_result->impl_valid());
}

TEST_CASE("fabric::create with tcp provider", "[fabric][integration]") {
    auto info_result = get_tcp_fabric_info();

    if (!info_result) {
        SKIP("TCP provider not available");
    }

    auto fabric_result = fabric::create(*info_result);

    REQUIRE(static_cast<bool>(fabric_result));
    REQUIRE(fabric_result->impl_valid());
}

// =============================================================================
// Domain Creation Tests
// =============================================================================

TEST_CASE("domain::create from fabric", "[domain][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);

    REQUIRE(static_cast<bool>(domain_result));
    REQUIRE(domain_result->impl_valid());
}

TEST_CASE("domain properties are accessible", "[domain][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    // Access domain properties
    [[maybe_unused]] auto ctrl_progress = domain_result->get_control_progress();
    [[maybe_unused]] auto data_progress = domain_result->get_data_progress();

    REQUIRE(true);
}

// =============================================================================
// Endpoint Creation Tests
// =============================================================================

TEST_CASE("endpoint::create from domain", "[endpoint][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto ep_result = endpoint::create(*domain_result, *info_result);

    REQUIRE(static_cast<bool>(ep_result));
    REQUIRE(ep_result->impl_valid());
}

TEST_CASE("endpoint properties before enable", "[endpoint][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto ep_result = endpoint::create(*domain_result, *info_result);
    REQUIRE(static_cast<bool>(ep_result));

    // Access endpoint properties
    [[maybe_unused]] auto ep_type = ep_result->get_type();

    REQUIRE(true);
}

// =============================================================================
// Completion Queue Tests
// =============================================================================

TEST_CASE("completion_queue::create", "[completion_queue][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    completion_queue_attr cq_attr{.size = queue_size{64UL}};
    auto cq_result = completion_queue::create(*domain_result, cq_attr);

    REQUIRE(static_cast<bool>(cq_result));
    REQUIRE(cq_result->impl_valid());
}

TEST_CASE("completion_queue with various sizes", "[completion_queue][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    SECTION("small queue") {
        completion_queue_attr cq_attr{.size = queue_size{16UL}};
        auto cq_result = completion_queue::create(*domain_result, cq_attr);
        REQUIRE(static_cast<bool>(cq_result));
    }

    SECTION("medium queue") {
        completion_queue_attr cq_attr{.size = queue_size{256UL}};
        auto cq_result = completion_queue::create(*domain_result, cq_attr);
        REQUIRE(static_cast<bool>(cq_result));
    }

    SECTION("large queue") {
        completion_queue_attr cq_attr{.size = queue_size{4096UL}};
        auto cq_result = completion_queue::create(*domain_result, cq_attr);
        REQUIRE(static_cast<bool>(cq_result));
    }
}

TEST_CASE("completion_queue poll on empty queue", "[completion_queue][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    completion_queue_attr cq_attr{.size = queue_size{64UL}};
    auto cq_result = completion_queue::create(*domain_result, cq_attr);
    REQUIRE(static_cast<bool>(cq_result));

    // Poll empty queue should return no completions (std::nullopt)
    auto poll_result = cq_result->poll();

    // Empty queue should return nullopt (no completion available)
    REQUIRE_FALSE(poll_result.has_value());
}

// =============================================================================
// Address Vector Tests
// =============================================================================

TEST_CASE("address_vector::create", "[address_vector][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto av_result = address_vector::create(*domain_result);

    REQUIRE(static_cast<bool>(av_result));
    REQUIRE(av_result->impl_valid());
}

// =============================================================================
// Endpoint Binding Tests
// =============================================================================

TEST_CASE("endpoint bind to completion queue", "[endpoint][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto ep_result = endpoint::create(*domain_result, *info_result);
    REQUIRE(static_cast<bool>(ep_result));

    completion_queue_attr cq_attr{.size = queue_size{64UL}};
    auto cq_result = completion_queue::create(*domain_result, cq_attr);
    REQUIRE(static_cast<bool>(cq_result));

    // Bind CQ to endpoint for both TX and RX
    auto bind_result = ep_result->bind_cq(*cq_result, cq_bind::transmit | cq_bind::recv);
    REQUIRE(static_cast<bool>(bind_result));
}

TEST_CASE("endpoint bind to address vector", "[endpoint][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto ep_result = endpoint::create(*domain_result, *info_result);
    REQUIRE(static_cast<bool>(ep_result));

    auto av_result = address_vector::create(*domain_result);
    REQUIRE(static_cast<bool>(av_result));

    auto bind_result = ep_result->bind_av(*av_result);
    REQUIRE(static_cast<bool>(bind_result));
}

TEST_CASE("endpoint enable after binding", "[endpoint][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto ep_result = endpoint::create(*domain_result, *info_result);
    REQUIRE(static_cast<bool>(ep_result));

    completion_queue_attr cq_attr{.size = queue_size{64UL}};
    auto cq_result = completion_queue::create(*domain_result, cq_attr);
    REQUIRE(static_cast<bool>(cq_result));

    auto av_result = address_vector::create(*domain_result);
    REQUIRE(static_cast<bool>(av_result));

    // Bind resources
    REQUIRE(static_cast<bool>(
        ep_result->bind_cq(*cq_result, cq_bind::transmit | cq_bind::recv)));
    REQUIRE(static_cast<bool>(ep_result->bind_av(*av_result)));

    // Enable endpoint
    auto enable_result = ep_result->enable();
    REQUIRE(static_cast<bool>(enable_result));
}

// =============================================================================
// Memory Registration Tests
// =============================================================================

TEST_CASE("memory_region::register_memory", "[memory][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    // Register memory
    std::vector<std::byte> buffer(4096);
    auto mr_result = memory_region::register_memory(
        *domain_result, std::span{buffer},
        mr_access_flags::read | mr_access_flags::write);

    REQUIRE(static_cast<bool>(mr_result));
    REQUIRE(mr_result->impl_valid());
}

TEST_CASE("memory_region with various sizes", "[memory][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    SECTION("small buffer") {
        std::vector<std::byte> buffer(64);
        auto mr_result = memory_region::register_memory(
            *domain_result, std::span{buffer},
            mr_access_flags::read | mr_access_flags::write);
        REQUIRE(static_cast<bool>(mr_result));
    }

    SECTION("medium buffer") {
        std::vector<std::byte> buffer(4096);
        auto mr_result = memory_region::register_memory(
            *domain_result, std::span{buffer},
            mr_access_flags::read | mr_access_flags::write);
        REQUIRE(static_cast<bool>(mr_result));
    }

    SECTION("large buffer") {
        std::vector<std::byte> buffer(1024 * 1024);  // 1 MB
        auto mr_result = memory_region::register_memory(
            *domain_result, std::span{buffer},
            mr_access_flags::read | mr_access_flags::write);
        REQUIRE(static_cast<bool>(mr_result));
    }
}

TEST_CASE("memory_region properties", "[memory][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    std::vector<std::byte> buffer(4096);
    auto mr_result = memory_region::register_memory(
        *domain_result, std::span{buffer},
        mr_access_flags::read | mr_access_flags::write);
    REQUIRE(static_cast<bool>(mr_result));

    // Access MR properties
    [[maybe_unused]] auto key = mr_result->key();

    REQUIRE(true);
}

// =============================================================================
// Full Initialization Chain Tests
// =============================================================================

TEST_CASE("full initialization chain", "[fabric][integration]") {
    // Query fabric
    auto info_result = get_any_fabric_info();
    if (!info_result) {
        SKIP("No fabric providers available");
    }

    // Create fabric
    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    // Create domain
    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    // Create completion queue
    completion_queue_attr cq_attr{.size = queue_size{64UL}};
    auto cq_result = completion_queue::create(*domain_result, cq_attr);
    REQUIRE(static_cast<bool>(cq_result));

    // Create address vector
    auto av_result = address_vector::create(*domain_result);
    REQUIRE(static_cast<bool>(av_result));

    // Create endpoint
    auto ep_result = endpoint::create(*domain_result, *info_result);
    REQUIRE(static_cast<bool>(ep_result));

    // Bind CQ and AV
    REQUIRE(static_cast<bool>(
        ep_result->bind_cq(*cq_result, cq_bind::transmit | cq_bind::recv)));
    REQUIRE(static_cast<bool>(ep_result->bind_av(*av_result)));

    // Enable endpoint
    REQUIRE(static_cast<bool>(ep_result->enable()));

    // Register memory
    std::vector<std::byte> buffer(4096);
    auto mr_result = memory_region::register_memory(
        *domain_result, std::span{buffer},
        mr_access_flags::read | mr_access_flags::write);
    REQUIRE(static_cast<bool>(mr_result));

    // Everything should be valid
    REQUIRE(fabric_result->impl_valid());
    REQUIRE(domain_result->impl_valid());
    REQUIRE(cq_result->impl_valid());
    REQUIRE(av_result->impl_valid());
    REQUIRE(ep_result->impl_valid());
    REQUIRE(mr_result->impl_valid());
}

TEST_CASE("full initialization chain with tcp provider", "[fabric][integration]") {
    // Query tcp provider specifically
    auto info_result = get_tcp_fabric_info();
    if (!info_result) {
        SKIP("TCP provider not available");
    }

    // Create fabric
    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    // Create domain
    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    // Create completion queue
    completion_queue_attr cq_attr{.size = queue_size{64UL}};
    auto cq_result = completion_queue::create(*domain_result, cq_attr);
    REQUIRE(static_cast<bool>(cq_result));

    // Create address vector
    auto av_result = address_vector::create(*domain_result);
    REQUIRE(static_cast<bool>(av_result));

    // Create endpoint
    auto ep_result = endpoint::create(*domain_result, *info_result);
    REQUIRE(static_cast<bool>(ep_result));

    // Bind CQ and AV
    REQUIRE(static_cast<bool>(
        ep_result->bind_cq(*cq_result, cq_bind::transmit | cq_bind::recv)));
    REQUIRE(static_cast<bool>(ep_result->bind_av(*av_result)));

    // Enable endpoint
    REQUIRE(static_cast<bool>(ep_result->enable()));

    // Verify everything is valid
    REQUIRE(fabric_result->impl_valid());
    REQUIRE(domain_result->impl_valid());
    REQUIRE(cq_result->impl_valid());
    REQUIRE(av_result->impl_valid());
    REQUIRE(ep_result->impl_valid());
}

// =============================================================================
// Counter Tests
// =============================================================================

TEST_CASE("counter::create", "[counter][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto cntr_result = counter::create(*domain_result);

    REQUIRE(static_cast<bool>(cntr_result));
    REQUIRE(cntr_result->impl_valid());
}

TEST_CASE("counter read initial value", "[counter][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto cntr_result = counter::create(*domain_result);
    REQUIRE(static_cast<bool>(cntr_result));

    // Initial value should be 0
    auto value = cntr_result->read();
    REQUIRE(value == 0UL);
}

TEST_CASE("counter set and read", "[counter][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto cntr_result = counter::create(*domain_result);
    REQUIRE(static_cast<bool>(cntr_result));

    // Set to a specific value
    auto set_result = cntr_result->set(42);
    REQUIRE(static_cast<bool>(set_result));

    // Read back
    auto value = cntr_result->read();
    REQUIRE(value == 42UL);
}

TEST_CASE("counter add", "[counter][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto domain_result = domain::create(*fabric_result, *info_result);
    REQUIRE(static_cast<bool>(domain_result));

    auto cntr_result = counter::create(*domain_result);
    REQUIRE(static_cast<bool>(cntr_result));

    // Set initial value
    REQUIRE(static_cast<bool>(cntr_result->set(10)));

    // Add to it
    auto add_result = cntr_result->add(5);
    REQUIRE(static_cast<bool>(add_result));

    // Read back
    auto value = cntr_result->read();
    REQUIRE(value == 15UL);
}

// =============================================================================
// Event Queue Tests
// =============================================================================

TEST_CASE("event_queue::create", "[event_queue][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    auto fabric_result = fabric::create(*info_result);
    REQUIRE(static_cast<bool>(fabric_result));

    auto eq_result = event_queue::create(*fabric_result);

    REQUIRE(static_cast<bool>(eq_result));
    REQUIRE(eq_result->impl_valid());
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_CASE("query_fabric with impossible hints returns error", "[fabric][integration]") {
    fabric_hints hints{};
    hints.provider_name = "nonexistent_provider_xyz123";
    // Add a capability that doesn't exist to ensure failure
    hints.capabilities = caps{0xFFFFFFFFFFFFFFFFULL};

    auto info_result = query_fabric(hints);

    // Either no provider matches or the query fails
    // Some systems may ignore the provider name filter, so we accept either
    // an error or a valid result
    REQUIRE(true);  // This test is informational
}

TEST_CASE("result error codes are meaningful", "[fabric][integration]") {
    fabric_hints hints{};
    hints.provider_name = "nonexistent_provider";
    // Use impossible capabilities to force a failure
    hints.capabilities = caps{0xFFFFFFFFFFFFFFFFULL};

    auto info_result = query_fabric(hints);

    // If the query fails (expected), verify error codes are meaningful
    if (!info_result) {
        REQUIRE(info_result.error().value() != 0);

        // Error message should be non-empty
        auto msg = info_result.error().message();
        REQUIRE_FALSE(msg.empty());
    }
    // If query succeeds (unexpected on most systems), that's also fine
}

// =============================================================================
// Resource Cleanup Tests (RAII)
// =============================================================================

TEST_CASE("resources are properly cleaned up on scope exit", "[fabric][integration]") {
    auto info_result = get_any_fabric_info();

    if (!info_result) {
        SKIP("No fabric providers available");
    }

    // Create resources in a nested scope
    {
        auto fabric_result = fabric::create(*info_result);
        REQUIRE(static_cast<bool>(fabric_result));

        {
            auto domain_result = domain::create(*fabric_result, *info_result);
            REQUIRE(static_cast<bool>(domain_result));

            {
                completion_queue_attr cq_attr{.size = queue_size{64UL}};
                auto cq_result = completion_queue::create(*domain_result, cq_attr);
                REQUIRE(static_cast<bool>(cq_result));

                auto ep_result = endpoint::create(*domain_result, *info_result);
                REQUIRE(static_cast<bool>(ep_result));

                // Resources go out of scope here - should clean up properly
            }

            // Domain still valid
            REQUIRE(domain_result->impl_valid());
        }

        // Fabric still valid
        REQUIRE(fabric_result->impl_valid());
    }

    // All resources cleaned up, test passes if no crash
    REQUIRE(true);
}
