// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <array>
#include <utility>
#include <vector>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace loom;

TEST_CASE("memory binding rejects empty endpoints before calling the provider", "[memory]") {
    test::mock_domain provider;
    provider.region_ops.bind = [](fid*, fid*, std::uint64_t) {
        FAIL("invalid endpoint reached the memory provider");
        return 0;
    };
    std::array<std::byte, 64> buffer{};
    auto region = memory_region::register_memory(provider.dom, buffer, mr_access_flags::send);
    REQUIRE(region);
    endpoint empty;
    const auto result = region->bind(empty);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == errc::invalid_argument);
}

TEST_CASE("memory registration rejects empty domains", "[memory]") {
    domain empty;
    std::array<std::byte, 64> buffer{};
    const auto access = mr_access_flags::read;
    auto check = [](result<memory_region> region) {
        REQUIRE_FALSE(region);
        REQUIRE(region.error() == errc::invalid_argument);
    };
    check(memory_region::register_memory(empty, buffer, access));
    check(memory_region::register_hmem(
        empty, buffer.data(), buffer.size(), access, hmem_device::cuda(0)));
    check(memory_region::register_dmabuf(empty, buffer.data(), buffer.size(), access, 7, 0));
}

TEST_CASE("registration translates access permissions", "[memory]") {
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    constexpr std::array permissions{
        std::pair{mr_access_flags::read, FI_READ},
        std::pair{mr_access_flags::write, FI_WRITE},
        std::pair{mr_access_flags::remote_read, FI_REMOTE_READ},
        std::pair{mr_access_flags::remote_write, FI_REMOTE_WRITE},
        std::pair{mr_access_flags::send, FI_SEND},
        std::pair{mr_access_flags::recv, FI_RECV},
        std::pair{mr_access_flags::send | mr_access_flags::recv, FI_SEND | FI_RECV},
    };

    for (const auto& [access, expected] : permissions) {
        {
            auto region = memory_region::register_memory(provider.dom, buffer, access);
            REQUIRE(region);
            REQUIRE(provider.access == expected);
        }
        {
            auto region = memory_region::register_dmabuf(
                provider.dom, buffer.data(), buffer.size(), access, 7, 0);
            REQUIRE(region);
            REQUIRE(provider.access == expected);
            REQUIRE(provider.flags == FI_MR_DMABUF);
        }
        {
            auto region = memory_region::register_hmem(
                provider.dom, buffer.data(), buffer.size(), access, hmem_device::cuda(0));
            REQUIRE(region);
            REQUIRE(provider.access == expected);
        }
    }
}

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
        REQUIRE(static_cast<bool>(mr));
        REQUIRE(mr.address() == buffer.data());
        REQUIRE(mr.length() == buffer.size());
    }
}

TEST_CASE("device registrations retain nonzero ordinals", "[memory]") {
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    const std::array devices{
        hmem_device::cuda(2),
        hmem_device::level_zero(3),
        hmem_device::neuron(4),
        hmem_device::synapseai(5),
#if FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION) >= FI_VERSION(2, 5)
        hmem_device::rocr(6),
#endif
    };
    for (const auto& device : devices) {
        auto region = memory_region::register_hmem(
            provider.dom, buffer.data(), buffer.size(), mr_access_flags::send, device);
        REQUIRE(region);
        REQUIRE(provider.iface == static_cast<fi_hmem_iface>(device.iface));
        REQUIRE(provider.device_id == device.device_id);
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
