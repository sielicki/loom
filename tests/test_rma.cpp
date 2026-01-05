// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace loom;

TEST_CASE("rma type definitions", "[rma]") {
    rma::rma_iov iov{};
    iov.addr = 0x1000;
    iov.len = 4096;
    iov.key = 0xABCD;

    REQUIRE(iov.addr == 0x1000UL);
    REQUIRE(iov.len == 4096UL);
    REQUIRE(iov.key == 0xABCDUL);

    remote_memory remote{0x2000, 0x5678, 8192};
    REQUIRE(remote.addr == 0x2000UL);
    REQUIRE(remote.key == 0x5678UL);
    REQUIRE(remote.length == 8192UL);
}

TEST_CASE("rma size queries", "[rma][.provider]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::rma;

    auto info_result = query_fabric(hints);
    if (!info_result) {
        SKIP("No RMA providers found");
    }

    auto& info = *info_result;
    auto fab_result = fabric::create(info);
    REQUIRE(static_cast<bool>(fab_result));

    auto dom_result = domain::create(*fab_result, info);
    REQUIRE(static_cast<bool>(dom_result));

    auto ep_result = endpoint::create(*dom_result, info);
    REQUIRE(static_cast<bool>(ep_result));

    auto& ep = *ep_result;
    auto enable_result = ep.enable();
    REQUIRE(static_cast<bool>(enable_result));

    auto max_size = rma::get_max_rma_size(ep);
    REQUIRE(max_size > 0);

    auto inject_size = rma::get_inject_size(ep);
    REQUIRE(inject_size > 0);
}

TEST_CASE("inject write small data", "[rma][.provider]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::rma;

    auto info_result = query_fabric(hints);
    if (!info_result) {
        SKIP("No RMA providers found");
    }

    auto& info = *info_result;
    auto fab_result = fabric::create(info);
    REQUIRE(static_cast<bool>(fab_result));

    auto dom_result = domain::create(*fab_result, info);
    REQUIRE(static_cast<bool>(dom_result));

    auto ep_result = endpoint::create(*dom_result, info);
    REQUIRE(static_cast<bool>(ep_result));

    auto& ep = *ep_result;
    auto enable_result = ep.enable();
    REQUIRE(static_cast<bool>(enable_result));

    std::array<std::byte, 64> data{};
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i);
    }

    remote_memory remote{0x1000, 0xABCD, data.size()};

    [[maybe_unused]] auto result = rma::inject_write(ep, std::span{data}, remote);
}
