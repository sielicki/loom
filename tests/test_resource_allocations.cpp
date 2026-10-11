// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/address_vector.hpp>
#include <loom/core/memory.hpp>
#include <loom/core/passive_endpoint.hpp>
#include <loom/core/scalable_endpoint.hpp>
#include <loom/core/shared_context.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <array>
#include <memory_resource>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct opening_provider {
    int opened{};
    int closed{};
    int open_error{};

    template <typename Native>
    inline static fi_ops lifetime_ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                ++static_cast<opening_provider*>(handle->context)->closed;
                delete reinterpret_cast<Native*>(handle);
                return 0;
            },
    };
    template <typename Native>
    auto open(Native** output) -> int {
        if (open_error)
            return open_error;
        auto* handle = new Native{};
        handle->fid.context = this;
        handle->fid.ops = &lifetime_ops<Native>;
        if constexpr (std::same_as<Native, fid_ep>)
            handle->ops = &endpoint_ops;
        *output = handle;
        ++opened;
        return 0;
    }
    template <typename Parent, typename Native>
    static auto open(Parent* parent, Native** output) -> int {
        return static_cast<opening_provider*>(parent->fid.context)->open(output);
    }

    fi_ops parent_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .tx_ctx =
            [](fid_ep* parent, int, fi_tx_attr*, fid_ep** output, void*) {
                return open(parent, output);
            },
        .rx_ctx =
            [](fid_ep* parent, int, fi_rx_attr*, fid_ep** output, void*) {
                return open(parent, output);
            },
    };
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .av_open =
            [](fid_domain* parent, fi_av_attr*, fid_av** output, void*) {
                return open(parent, output);
            },
        .cq_open =
            [](fid_domain* parent, fi_cq_attr*, fid_cq** output, void*) {
                return open(parent, output);
            },
        .endpoint =
            [](fid_domain* parent, fi_info*, fid_ep** output, void*) {
                return open(parent, output);
            },
        .scalable_ep =
            [](fid_domain* parent, fi_info*, fid_ep** output, void*) {
                return open(parent, output);
            },
        .stx_ctx =
            [](fid_domain* parent, fi_tx_attr*, fid_stx** output, void*) {
                return open(parent, output);
            },
        .srx_ctx =
            [](fid_domain* parent, fi_rx_attr*, fid_ep** output, void*) {
                return open(parent, output);
            },
    };
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .domain =
            [](fid_fabric* parent, fi_info*, fid_domain** output, void*) {
                return open(parent, output);
            },
        .passive_ep =
            [](fid_fabric* parent, fi_info*, fid_pep** output, void*) {
                return open(parent, output);
            },
        .eq_open =
            [](fid_fabric* parent, fi_eq_attr*, fid_eq** output, void*) {
                return open(parent, output);
            },
    };
    fid_domain native_domain{.fid = {.context = this, .ops = &parent_ops}, .ops = &domain_ops};
    fid_fabric native_fabric{.fid = {.context = this, .ops = &parent_ops}, .ops = &fabric_ops};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native_domain)};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &native_fabric, nullptr)};
    loom::fabric_info info = [] {
        auto* native = fi_allocinfo();
        REQUIRE(native);
        native->ep_attr->tx_ctx_cnt = 1;
        native->ep_attr->rx_ctx_cnt = 1;
        return loom::fabric_info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
            loom::get_default_resource(), native)};
    }();

    auto create(int kind, loom::memory_resource* resource) -> bool {
        switch (kind) {
            case 0:
                return loom::domain::create(fabric, info, resource).has_value();
            case 1:
                return loom::endpoint::create(domain, info, resource).has_value();
            case 2:
                return loom::scalable_endpoint::create(domain, info, resource).has_value();
            case 3:
                return loom::address_vector::create(domain, {}, resource).has_value();
            case 4:
                return loom::completion_queue::create(domain, {}, resource).has_value();
            case 5:
                return loom::event_queue::create(fabric, {}, resource).has_value();
            case 6:
                return loom::passive_endpoint::create(fabric, info, resource).has_value();
            case 7:
                return loom::shared_tx_context::create(domain, info, resource).has_value();
            default:
                return loom::shared_rx_context::create(domain, info, resource).has_value();
        }
    }
};

struct default_resource_scope {
    std::pmr::memory_resource* previous =
        std::pmr::set_default_resource(std::pmr::null_memory_resource());
    ~default_resource_scope() { std::pmr::set_default_resource(previous); }
};
}  // namespace

TEST_CASE("resource factories close native handles exactly once", "[allocation]") {
    const auto kind = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    INFO("resource factory " << kind);
    opening_provider provider;
    SECTION("allocation failure") {
        REQUIRE_THROWS_AS(provider.create(kind, std::pmr::null_memory_resource()), std::bad_alloc);
        REQUIRE(provider.opened == 1);
        REQUIRE(provider.closed == 1);
    }
    SECTION("successful ownership transfer") {
        REQUIRE(provider.create(kind, nullptr));
        REQUIRE(provider.opened == 1);
        REQUIRE(provider.closed == 1);
    }
    SECTION("native open failure") {
        provider.open_error = -FI_EIO;
        REQUIRE_FALSE(provider.create(kind, std::pmr::null_memory_resource()));
        REQUIRE(provider.opened == 0);
        REQUIRE(provider.closed == 0);
    }
}

TEST_CASE("scalable contexts close native handles when allocation fails", "[allocation]") {
    opening_provider provider;
    auto endpoint = loom::scalable_endpoint::create(provider.domain, provider.info);
    REQUIRE(endpoint);
    {
        default_resource_scope resource;
        SECTION("transmit context") {
            REQUIRE_THROWS_AS(endpoint->tx_context(0), std::bad_alloc);
        }
        SECTION("receive context") {
            REQUIRE_THROWS_AS(endpoint->rx_context(0), std::bad_alloc);
        }
    }
    REQUIRE(provider.opened == 2);
    REQUIRE(provider.closed == 1);
    *endpoint = {};
    REQUIRE(provider.closed == 2);
}

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

TEST_CASE("discovery and fabric creation release native allocations on failure",
          "[allocation][integration]") {
    auto info = loom::query_fabric({});
    if (!info)
        SKIP("No providers available");
    auto fabric = loom::fabric::create(*info);
    if (!fabric)
        SKIP("Provider fabric cannot be opened");
    // LeakSanitizer also checks allocations made inside libfabric here.
    SECTION("discovery result") {
        REQUIRE_THROWS_AS(loom::query_fabric({}, std::pmr::null_memory_resource()), std::bad_alloc);
    }
    SECTION("fabric handle") {
        REQUIRE_THROWS_AS(loom::fabric::create(*info, std::pmr::null_memory_resource()),
                          std::bad_alloc);
    }
}
