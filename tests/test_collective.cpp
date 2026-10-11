// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace loom;

TEST_CASE("collective operation enums", "[collective]") {
    auto barrier_op = collective::operation::barrier;
    auto broadcast_op = collective::operation::broadcast;
    auto alltoall_op = collective::operation::all_to_all;
    auto allreduce_op = collective::operation::all_reduce;
    auto allgather_op = collective::operation::all_gather;
    auto reduce_scatter_op = collective::operation::reduce_scatter;
    auto reduce_op = collective::operation::reduce;
    auto scatter_op = collective::operation::scatter;
    auto gather_op = collective::operation::gather;

    REQUIRE(barrier_op != broadcast_op);
    REQUIRE(alltoall_op != allreduce_op);
    REQUIRE(allgather_op != reduce_scatter_op);
    REQUIRE(reduce_op != scatter_op);
    REQUIRE(scatter_op != gather_op);
    REQUIRE(gather_op != barrier_op);
}

TEST_CASE("is_supported query", "[collective][.provider]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::collective;

    auto info_result = query_fabric(hints);
    if (!info_result) {
        SKIP("No collective providers found");
    }

    auto& info = *info_result;
    auto fab_result = fabric::create(info);
    REQUIRE(static_cast<bool>(fab_result));

    auto dom_result = domain::create(*fab_result, info);
    REQUIRE(static_cast<bool>(dom_result));

    auto& dom = *dom_result;

    [[maybe_unused]] bool barrier_supported =
        collective::is_supported<atomic::datatype::uint8, atomic::operation::sum>(
            dom, collective::operation::barrier);
    [[maybe_unused]] bool broadcast_supported =
        collective::is_supported<std::int32_t, atomic::operation::sum>(
            dom, collective::operation::broadcast);
    [[maybe_unused]] bool allreduce_supported =
        collective::is_supported<std::int32_t, atomic::operation::sum>(
            dom, collective::operation::all_reduce);
}
