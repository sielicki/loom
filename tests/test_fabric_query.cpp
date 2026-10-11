// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/fabric_query.hpp>

#include <rdma/fabric.h>

#include <string>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
struct unsupported_capabilities {
    static constexpr loom::caps required_caps{~std::uint64_t{0}};
    static constexpr loom::caps optional_caps{};
    static constexpr auto ep_type = loom::endpoint_types::msg;
};
}  // namespace

TEST_CASE("filtered provider queries return owning ranges", "[fabric_query][integration]") {
    auto available = loom::fabric_query<>::query();
    if (!available)
        SKIP("No providers available");
    const auto count = available->size();
    REQUIRE(count > 0);
    auto filtered = [] {
        const auto accept = [](const loom::fabric_info&) {
            return true;
        };
        return loom::query_providers<>(accept);
    }();
    REQUIRE(filtered);
    REQUIRE(std::ranges::distance(*filtered) == count);
    auto moved = std::move(filtered);
    REQUIRE(std::ranges::distance(*moved) == count);
    for (const auto& info : *moved) {
        REQUIRE(info);
        REQUIRE(info.get_fabric_attr());
    }
    auto none =
        loom::fabric_query<>::query_filtered([](const loom::fabric_info&) { return false; });
    REQUIRE(none);
    REQUIRE(std::ranges::empty(*none));
}

TEST_CASE("filtered provider queries propagate discovery errors", "[fabric_query]") {
    using query = loom::fabric_query<unsupported_capabilities>;
    const auto available = query::query();
    REQUIRE_FALSE(available);
    bool called{};
    auto filtered = query::query_filtered([&](const loom::fabric_info&) {
        called = true;
        return true;
    });
    REQUIRE_FALSE(filtered);
    REQUIRE(filtered.error() == available.error());
    REQUIRE_FALSE(called);
}

TEST_CASE("first provider queries return a live matching provider", "[fabric_query][integration]") {
    using query = loom::fabric_query<loom::connection_oriented_messaging>;
    const auto available = query::query();
    if (!available)
        SKIP("No message providers available");
    REQUIRE_FALSE(available->empty());
    auto first = query::query_first();
    REQUIRE(first);
    REQUIRE(first->has_value());
    REQUIRE(**first);
    REQUIRE((*first)->get_caps().has(loom::capability::msg));
    REQUIRE((*first)->get_fabric_attr()->provider_name ==
            available->begin()->get_fabric_attr()->provider_name);
    auto moved = std::move(**first);
    REQUIRE(moved);
    REQUIRE(moved.get_endpoint_attr()->type == loom::endpoint_types::msg);
}

TEST_CASE("first provider queries propagate discovery errors", "[fabric_query]") {
    using query = loom::fabric_query<unsupported_capabilities>;
    const auto available = query::query();
    REQUIRE_FALSE(available);
    const auto first = query::query_first();
    REQUIRE_FALSE(first);
    REQUIRE(first.error() == available.error());
}

TEST_CASE("fabric queries honor name filters", "[fabric_query][integration]") {
    loom::fabric_hints hints;
    SECTION("provider") {
        hints.provider_name = "loom_nonexistent_provider";
    }
    SECTION("fabric") {
        hints.fabric_name = "loom_nonexistent_fabric";
    }
    SECTION("domain") {
        hints.domain_name = "loom_nonexistent_domain";
    }
    auto result = loom::query_fabric(hints);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::no_data);
}

TEST_CASE("fabric queries retain bounded names and progress requirements",
          "[fabric_query][integration]") {
    loom::fabric_hints hints;
    auto available = loom::query_fabric(hints);
    if (!available) {
        SKIP("No providers available");
    }
    const auto fabric_attr = available->get_fabric_attr();
    const auto domain_attr = available->get_domain_attr();
    REQUIRE(fabric_attr);
    REQUIRE(domain_attr);
    const std::string provider = std::string{fabric_attr->provider_name} + "unused_suffix";
    hints.provider_name = std::string_view{provider}.substr(0, fabric_attr->provider_name.size());
    hints.control_progress = domain_attr->control_progress;
    hints.data_progress = domain_attr->data_progress;
    auto result = loom::query_fabric(hints);
    REQUIRE(result);
    REQUIRE(result->get_fabric_attr()->provider_name == fabric_attr->provider_name);
    REQUIRE(result->get_domain_attr()->control_progress == *hints.control_progress);
    REQUIRE(result->get_domain_attr()->data_progress == *hints.data_progress);
}

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

TEMPLATE_TEST_CASE("RMA queries require initiator and target capabilities",
                   "[fabric_query][integration][rma]",
                   loom::rdma_read_ops,
                   loom::rdma_write_ops,
                   (loom::combined_capabilities<loom::rdma_read_ops, loom::rdma_write_ops>)) {
    constexpr auto read = !std::same_as<TestType, loom::rdma_write_ops>;
    constexpr auto write = !std::same_as<TestType, loom::rdma_read_ops>;
    constexpr std::uint64_t initiator = (read ? FI_READ : 0) | (write ? FI_WRITE : 0);
    constexpr std::uint64_t target = (read ? FI_REMOTE_READ : 0) | (write ? FI_REMOTE_WRITE : 0);
    auto providers = loom::fabric_query<TestType>::query();
    REQUIRE(providers);
    REQUIRE_FALSE(providers->empty());
    for (const auto& info : *providers) {
        const auto& native = *static_cast<const fi_info*>(info.internal_ptr());
        REQUIRE(native.tx_attr);
        REQUIRE(native.rx_attr);
        CHECK((native.caps & (FI_RMA | initiator | target)) == (FI_RMA | initiator | target));
        CHECK((native.tx_attr->caps & initiator) == initiator);
        CHECK((native.rx_attr->caps & target) == target);
    }
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

    static_assert(loom::fabric_capability<combined>);
    static_assert(combined::required_caps == loom::caps{0ULL});
    static_assert(combined::optional_caps == loom::caps{0ULL});
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
