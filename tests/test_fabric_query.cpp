// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/fabric_query.hpp>

#include "ut.hpp"

using namespace boost::ut;

suite fabric_query_suite = [] {
    "fabric_capability concept"_test = [] {
        static_assert(loom::fabric_capability<loom::rdm_tagged_messaging>);
        static_assert(loom::fabric_capability<loom::rdma_write_ops>);
        static_assert(loom::fabric_capability<loom::rdma_read_ops>);
        static_assert(loom::fabric_capability<loom::gpu_direct_rdma>);
        static_assert(loom::fabric_capability<loom::atomic_ops>);
        static_assert(loom::fabric_capability<loom::connection_oriented_messaging>);
        static_assert(loom::fabric_capability<loom::datagram_messaging>);
    };

    "rdm_tagged_messaging properties"_test = [] {
        using cap = loom::rdm_tagged_messaging;

        static_assert((cap::required_caps & loom::capability::msg) != loom::caps{0ULL});
        static_assert((cap::required_caps & loom::capability::tagged) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::rdm);
        static_assert(cap::description == "RDM with tagged messaging");
    };

    "rdma_write_ops properties"_test = [] {
        using cap = loom::rdma_write_ops;

        static_assert((cap::required_caps & loom::capability::rma) != loom::caps{0ULL});
        static_assert((cap::required_caps & loom::capability::remote_write) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::rdm);
    };

    "rdma_read_ops properties"_test = [] {
        using cap = loom::rdma_read_ops;

        static_assert((cap::required_caps & loom::capability::rma) != loom::caps{0ULL});
        static_assert((cap::required_caps & loom::capability::remote_read) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::rdm);
    };

    "gpu_direct_rdma properties"_test = [] {
        using cap = loom::gpu_direct_rdma;

        static_assert((cap::required_caps & loom::capability::hmem) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::rdm);
    };

    "atomic_ops properties"_test = [] {
        using cap = loom::atomic_ops;

        static_assert((cap::required_caps & loom::capability::atomic) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::rdm);
    };

    "connection_oriented_messaging properties"_test = [] {
        using cap = loom::connection_oriented_messaging;

        static_assert((cap::required_caps & loom::capability::msg) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::msg);
    };

    "datagram_messaging properties"_test = [] {
        using cap = loom::datagram_messaging;

        static_assert((cap::required_caps & loom::capability::msg) != loom::caps{0ULL});
        static_assert(cap::endpoint_type == loom::endpoint_types::dgram);
    };

    "combined_capabilities single"_test = [] {
        using combined = loom::combined_capabilities<loom::rdm_tagged_messaging>;

        static_assert(combined::required_caps == loom::rdm_tagged_messaging::required_caps);
        static_assert(combined::optional_caps == loom::rdm_tagged_messaging::optional_caps);
        static_assert(combined::endpoint_type == loom::endpoint_types::rdm);
    };

    "combined_capabilities multiple"_test = [] {
        using combined = loom::combined_capabilities<loom::rdma_write_ops, loom::rdma_read_ops>;

        constexpr auto expected_caps =
            loom::rdma_write_ops::required_caps | loom::rdma_read_ops::required_caps;
        static_assert(combined::required_caps == expected_caps);
        static_assert(combined::endpoint_type == loom::endpoint_types::rdm);
    };

    "combined_capabilities empty"_test = [] {
        using combined = loom::combined_capabilities<>;

        static_assert(combined::endpoint_type == loom::endpoint_types::msg);
    };

    "fabric_query build_hints single capability"_test = [] {
        using query = loom::fabric_query<loom::rdm_tagged_messaging>;
        constexpr auto hints = query::build_hints();

        static_assert(hints.capabilities == loom::rdm_tagged_messaging::required_caps);
        static_assert(hints.ep_type == loom::endpoint_types::rdm);
    };

    "fabric_query build_hints multiple capabilities"_test = [] {
        using query = loom::fabric_query<loom::rdma_write_ops, loom::atomic_ops>;
        constexpr auto hints = query::build_hints();

        constexpr auto expected =
            loom::rdma_write_ops::required_caps | loom::atomic_ops::required_caps;
        static_assert(hints.capabilities == expected);
    };

    "fabric_query build_hints no capabilities"_test = [] {
        using query = loom::fabric_query<>;
        constexpr auto hints = query::build_hints();

        static_assert(hints.capabilities == loom::caps{0ULL});
    };

    "fabric_query get_required_caps"_test = [] {
        using query = loom::fabric_query<loom::rdm_tagged_messaging>;

        constexpr auto caps = query::get_required_caps();
        static_assert(caps == loom::rdm_tagged_messaging::required_caps);
    };

    "fabric_query get_optional_caps"_test = [] {
        using query = loom::fabric_query<loom::rdm_tagged_messaging>;

        constexpr auto caps = query::get_optional_caps();
        static_assert(caps == loom::rdm_tagged_messaging::optional_caps);
    };

    "fabric_query get_endpoint_type"_test = [] {
        using rdm_query = loom::fabric_query<loom::rdm_tagged_messaging>;
        using msg_query = loom::fabric_query<loom::connection_oriented_messaging>;
        using dgram_query = loom::fabric_query<loom::datagram_messaging>;

        static_assert(rdm_query::get_endpoint_type() == loom::endpoint_types::rdm);
        static_assert(msg_query::get_endpoint_type() == loom::endpoint_types::msg);
        static_assert(dgram_query::get_endpoint_type() == loom::endpoint_types::dgram);
    };

    "fabric_query empty get_endpoint_type"_test = [] {
        using query = loom::fabric_query<>;

        static_assert(query::get_endpoint_type() == loom::endpoint_types::msg);
    };

    "capability descriptions"_test = [] {
        static_assert(loom::rdm_tagged_messaging::description == "RDM with tagged messaging");
        static_assert(loom::rdma_write_ops::description == "RDMA write operations");
        static_assert(loom::rdma_read_ops::description == "RDMA read operations");
        static_assert(loom::gpu_direct_rdma::description == "GPUDirect RDMA support");
        static_assert(loom::atomic_ops::description == "Atomic operations");
        static_assert(loom::connection_oriented_messaging::description ==
                      "Connection-oriented messaging");
        static_assert(loom::datagram_messaging::description == "Datagram messaging");
    };

    skip / "query_providers returns provider_range"_test = [] {
        auto result = loom::query_providers<loom::rdm_tagged_messaging>();

        if (result) {
            auto& range = *result;
            expect(!range.empty() || range.empty()) << "should return valid range";
        }
    };
};

auto main() -> int {}
