// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/fabric_query.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("fabric_capability concept", "[fabric_query]") {
    static_assert(loom::fabric_capability<loom::rdm_tagged_messaging>);
    static_assert(loom::fabric_capability<loom::rdma_write_ops>);
    static_assert(loom::fabric_capability<loom::rdma_read_ops>);
    static_assert(loom::fabric_capability<loom::gpu_direct_rdma>);
    static_assert(loom::fabric_capability<loom::atomic_ops>);
    static_assert(loom::fabric_capability<loom::connection_oriented_messaging>);
    static_assert(loom::fabric_capability<loom::datagram_messaging>);
}

TEST_CASE("rdm_tagged_messaging properties", "[fabric_query]") {
    using cap = loom::rdm_tagged_messaging;

    static_assert((cap::required_caps & loom::capability::msg) != loom::caps{0ULL});
    static_assert((cap::required_caps & loom::capability::tagged) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::rdm);
    static_assert(cap::description == "RDM with tagged messaging");
}

TEST_CASE("rdma_write_ops properties", "[fabric_query]") {
    using cap = loom::rdma_write_ops;

    static_assert((cap::required_caps & loom::capability::rma) != loom::caps{0ULL});
    static_assert((cap::required_caps & loom::capability::remote_write) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("rdma_read_ops properties", "[fabric_query]") {
    using cap = loom::rdma_read_ops;

    static_assert((cap::required_caps & loom::capability::rma) != loom::caps{0ULL});
    static_assert((cap::required_caps & loom::capability::remote_read) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("gpu_direct_rdma properties", "[fabric_query]") {
    using cap = loom::gpu_direct_rdma;

    static_assert((cap::required_caps & loom::capability::hmem) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("atomic_ops properties", "[fabric_query]") {
    using cap = loom::atomic_ops;

    static_assert((cap::required_caps & loom::capability::atomic) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("connection_oriented_messaging properties", "[fabric_query]") {
    using cap = loom::connection_oriented_messaging;

    static_assert((cap::required_caps & loom::capability::msg) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::msg);
}

TEST_CASE("datagram_messaging properties", "[fabric_query]") {
    using cap = loom::datagram_messaging;

    static_assert((cap::required_caps & loom::capability::msg) != loom::caps{0ULL});
    static_assert(cap::ep_type == loom::endpoint_types::dgram);
}

TEST_CASE("combined_capabilities single", "[fabric_query]") {
    using combined = loom::combined_capabilities<loom::rdm_tagged_messaging>;

    static_assert(combined::required_caps == loom::rdm_tagged_messaging::required_caps);
    static_assert(combined::optional_caps == loom::rdm_tagged_messaging::optional_caps);
    static_assert(combined::ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("combined_capabilities multiple", "[fabric_query]") {
    using combined = loom::combined_capabilities<loom::rdma_write_ops, loom::rdma_read_ops>;

    constexpr auto expected_caps =
        loom::rdma_write_ops::required_caps | loom::rdma_read_ops::required_caps;
    static_assert(combined::required_caps == expected_caps);
    static_assert(combined::ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("combined_capabilities empty", "[fabric_query]") {
    using combined = loom::combined_capabilities<>;

    static_assert(combined::ep_type == loom::endpoint_types::msg);
}

TEST_CASE("fabric_query build_hints single capability", "[fabric_query]") {
    using query = loom::fabric_query<loom::rdm_tagged_messaging>;
    constexpr auto hints = query::build_hints();

    static_assert(hints.capabilities == loom::rdm_tagged_messaging::required_caps);
    static_assert(hints.ep_type == loom::endpoint_types::rdm);
}

TEST_CASE("fabric_query build_hints multiple capabilities", "[fabric_query]") {
    using query = loom::fabric_query<loom::rdma_write_ops, loom::atomic_ops>;
    constexpr auto hints = query::build_hints();

    constexpr auto expected = loom::rdma_write_ops::required_caps | loom::atomic_ops::required_caps;
    static_assert(hints.capabilities == expected);
}

TEST_CASE("fabric_query build_hints no capabilities", "[fabric_query]") {
    using query = loom::fabric_query<>;
    constexpr auto hints = query::build_hints();

    static_assert(hints.capabilities == loom::caps{0ULL});
}

TEST_CASE("fabric_query get_required_caps", "[fabric_query]") {
    using query = loom::fabric_query<loom::rdm_tagged_messaging>;

    constexpr auto caps = query::get_required_caps();
    static_assert(caps == loom::rdm_tagged_messaging::required_caps);
}

TEST_CASE("fabric_query get_optional_caps", "[fabric_query]") {
    using query = loom::fabric_query<loom::rdm_tagged_messaging>;

    constexpr auto caps = query::get_optional_caps();
    static_assert(caps == loom::rdm_tagged_messaging::optional_caps);
}

TEST_CASE("fabric_query get_endpoint_type", "[fabric_query]") {
    using rdm_query = loom::fabric_query<loom::rdm_tagged_messaging>;
    using msg_query = loom::fabric_query<loom::connection_oriented_messaging>;
    using dgram_query = loom::fabric_query<loom::datagram_messaging>;

    static_assert(rdm_query::get_endpoint_type() == loom::endpoint_types::rdm);
    static_assert(msg_query::get_endpoint_type() == loom::endpoint_types::msg);
    static_assert(dgram_query::get_endpoint_type() == loom::endpoint_types::dgram);
}

TEST_CASE("fabric_query empty get_endpoint_type", "[fabric_query]") {
    using query = loom::fabric_query<>;

    static_assert(query::get_endpoint_type() == loom::endpoint_types::msg);
}

TEST_CASE("capability descriptions", "[fabric_query]") {
    static_assert(loom::rdm_tagged_messaging::description == "RDM with tagged messaging");
    static_assert(loom::rdma_write_ops::description == "RDMA write operations");
    static_assert(loom::rdma_read_ops::description == "RDMA read operations");
    static_assert(loom::gpu_direct_rdma::description == "GPUDirect RDMA support");
    static_assert(loom::atomic_ops::description == "Atomic operations");
    static_assert(loom::connection_oriented_messaging::description ==
                  "Connection-oriented messaging");
    static_assert(loom::datagram_messaging::description == "Datagram messaging");
}

TEST_CASE("query_providers returns provider_range", "[fabric_query][.provider]") {
    auto result = loom::query_providers<loom::rdm_tagged_messaging>();

    if (result) {
        auto& range = *result;
        REQUIRE((range.empty() || !range.empty()));
    }
}
