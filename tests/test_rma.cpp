// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>

#include <array>
#include <vector>

#include "ut.hpp"

using namespace boost::ut;
using namespace loom;

suite rma_suite = [] {
    "rma type definitions"_test = [] {
        rma::rma_iov iov{};
        iov.addr = 0x1000;
        iov.len = 4096;
        iov.key = 0xABCD;

        expect(iov.addr == 0x1000UL);
        expect(iov.len == 4096UL);
        expect(iov.key == 0xABCDUL);

        remote_memory remote{0x2000, 0x5678, 8192};
        expect(remote.addr == 0x2000UL);
        expect(remote.key == 0x5678UL);
        expect(remote.length == 8192UL);
    };

    skip / "rma size queries"_test = [] {
        fabric_hints hints{};
        hints.ep_type = endpoint_types::rdm;
        hints.capabilities = capability::rma;

        auto info_result = query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No RMA providers found";

        auto& info = *info_result;
        auto fab_result = fabric::create(info);
        expect(static_cast<bool>(fab_result)) << "Fabric creation failed";

        auto dom_result = domain::create(*fab_result, info);
        expect(static_cast<bool>(dom_result)) << "Domain creation failed";

        auto ep_result = endpoint::create(*dom_result, info);
        expect(static_cast<bool>(ep_result)) << "Endpoint creation failed";

        auto& ep = *ep_result;
        auto enable_result = ep.enable();
        expect(static_cast<bool>(enable_result)) << "Endpoint enable failed";

        auto max_size = rma::get_max_rma_size(ep);
        expect(max_size > 0_ul);

        auto inject_size = rma::get_inject_size(ep);
        expect(inject_size > 0_ul);
    };

    skip / "inject write small data"_test = [] {
        fabric_hints hints{};
        hints.ep_type = endpoint_types::rdm;
        hints.capabilities = capability::rma;

        auto info_result = query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No RMA providers found";

        auto& info = *info_result;
        auto fab_result = fabric::create(info);
        expect(static_cast<bool>(fab_result)) << "Fabric creation failed";

        auto dom_result = domain::create(*fab_result, info);
        expect(static_cast<bool>(dom_result)) << "Domain creation failed";

        auto ep_result = endpoint::create(*dom_result, info);
        expect(static_cast<bool>(ep_result)) << "Endpoint creation failed";

        auto& ep = *ep_result;
        auto enable_result = ep.enable();
        expect(static_cast<bool>(enable_result)) << "Endpoint enable failed";

        std::array<std::byte, 64> data{};
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<std::byte>(i);
        }

        remote_memory remote{0x1000, 0xABCD, data.size()};

        [[maybe_unused]] auto result = rma::inject_write(ep, std::span{data}, remote);
    };
};

auto main() -> int {}
