// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace loom;

TEST_CASE("basic memory registration", "[memory]") {
    SECTION("requires fabric provider") {
        fabric_hints hints{};
        hints.ep_type = endpoint_types::rdm;
        hints.capabilities = capability::msg;

        auto info_result = query_fabric(hints);
        if (!info_result) {
            SKIP("No providers found");
        }

        auto& info = *info_result;
        auto fab_result = fabric::create(info);
        REQUIRE(static_cast<bool>(fab_result));

        auto dom_result = domain::create(*fab_result, info);
        REQUIRE(static_cast<bool>(dom_result));

        std::vector<std::byte> buffer(4096);
        auto mr_result = memory_region::register_memory(
            *dom_result, std::span{buffer}, mr_access_flags::read | mr_access_flags::write);

        REQUIRE(static_cast<bool>(mr_result));

        auto& mr = *mr_result;
        REQUIRE(static_cast<bool>(mr.descriptor()));
        REQUIRE(mr.key() != 0U);
        REQUIRE(mr.address() == buffer.data());
        REQUIRE(mr.length() == buffer.size());
    }
}

TEST_CASE("hmem device creation", "[memory]") {
    auto cuda_dev = hmem_device::cuda(0);
    REQUIRE(cuda_dev.iface == hmem_iface::cuda);
    REQUIRE(cuda_dev.device_id == 0);

    auto rocr_dev = hmem_device::rocr(1);
    REQUIRE(rocr_dev.iface == hmem_iface::rocr);
    REQUIRE(rocr_dev.device_id == 1);

    auto ze_dev = hmem_device::level_zero(2);
    REQUIRE(ze_dev.iface == hmem_iface::level_zero);
    REQUIRE(ze_dev.device_id == 2);

    auto neuron_dev = hmem_device::neuron(3);
    REQUIRE(neuron_dev.iface == hmem_iface::neuron);
    REQUIRE(neuron_dev.device_id == 3);

    auto synapse_dev = hmem_device::synapseai(4);
    REQUIRE(synapse_dev.iface == hmem_iface::synapseai);
    REQUIRE(synapse_dev.device_id == 4);
}

TEST_CASE("remote memory descriptor", "[memory]") {
    std::uint64_t addr = 0x12345678;
    std::uint64_t key = 0xABCDEF;
    std::size_t length = 4096;

    remote_memory remote{addr, key, length};

    REQUIRE(remote.addr == addr);
    REQUIRE(remote.key == key);
    REQUIRE(remote.length == length);
}
