// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/memory.hpp>

#include <array>
#include <memory_resource>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("memory registration closes native handles when allocation fails", "[allocation]") {
    using namespace loom;
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    auto* resource = std::pmr::null_memory_resource();
    SECTION("host memory") {
        REQUIRE_THROWS_AS(
            memory_region::register_memory(provider.dom, buffer, mr_access_flags::send, resource),
            std::bad_alloc);
    }
    SECTION("device memory") {
        REQUIRE_THROWS_AS(memory_region::register_hmem(provider.dom,
                                                       buffer.data(),
                                                       buffer.size(),
                                                       mr_access_flags::send,
                                                       hmem_device::cuda(0),
                                                       resource),
                          std::bad_alloc);
    }
    SECTION("DMA buffer") {
        REQUIRE_THROWS_AS(
            memory_region::register_dmabuf(
                provider.dom, buffer.data(), buffer.size(), mr_access_flags::send, 7, 0, resource),
            std::bad_alloc);
    }
    REQUIRE(provider.registrations == 1);
    REQUIRE(provider.live_regions == 0);
}
