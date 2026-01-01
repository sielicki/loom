// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <vector>

#include "ut.hpp"

using namespace boost::ut;
using namespace loom;

suite memory_suite = [] {
    "basic memory registration"_test = [] {
        skip / "requires fabric provider"_test = [] {
            fabric_hints hints{};
            hints.ep_type = endpoint_types::rdm;
            hints.capabilities = capability::msg;

            auto info_result = query_fabric(hints);
            expect(static_cast<bool>(info_result)) << "No providers found";

            auto& info = *info_result;
            auto fab_result = fabric::create(info);
            expect(static_cast<bool>(fab_result)) << "Fabric creation failed";

            auto dom_result = domain::create(*fab_result, info);
            expect(static_cast<bool>(dom_result)) << "Domain creation failed";

            std::vector<std::byte> buffer(4096);
            auto mr_result = memory_region::register_memory(
                *dom_result, std::span{buffer}, mr_access_flags::read | mr_access_flags::write);

            expect(static_cast<bool>(mr_result)) << mr_result.error().message();

            auto& mr = *mr_result;
            expect(static_cast<bool>(mr.descriptor())) << "descriptor should not be empty";
            expect(mr.key() != 0U) << "key should not be zero";
            expect(mr.address() == buffer.data()) << "address mismatch";
            expect(mr.length() == buffer.size()) << "length mismatch";
        };
    };

    "hmem device creation"_test = [] {
        auto cuda_dev = hmem_device::cuda(0);
        expect(cuda_dev.iface == hmem_iface::cuda);
        expect(cuda_dev.device_id == 0);

        auto rocr_dev = hmem_device::rocr(1);
        expect(rocr_dev.iface == hmem_iface::rocr);
        expect(rocr_dev.device_id == 1);

        auto ze_dev = hmem_device::level_zero(2);
        expect(ze_dev.iface == hmem_iface::level_zero);
        expect(ze_dev.device_id == 2);

        auto neuron_dev = hmem_device::neuron(3);
        expect(neuron_dev.iface == hmem_iface::neuron);
        expect(neuron_dev.device_id == 3);

        auto synapse_dev = hmem_device::synapseai(4);
        expect(synapse_dev.iface == hmem_iface::synapseai);
        expect(synapse_dev.device_id == 4);
    };

    "remote memory descriptor"_test = [] {
        std::uint64_t addr = 0x12345678;
        std::uint64_t key = 0xABCDEF;
        std::size_t length = 4096;

        remote_memory remote{addr, key, length};

        expect(remote.addr == addr);
        expect(remote.key == key);
        expect(remote.length == length);
    };
};

auto main() -> int {}
