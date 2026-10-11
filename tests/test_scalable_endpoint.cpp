// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/rma.hpp>
#include <loom/core/scalable_endpoint.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <rdma/fi_cm.h>

#include <cstring>
#include <variant>

#include <netinet/in.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct scalable_provider {
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cm cm{
        .size = sizeof(fi_ops_cm),
        .getname =
            [](fid*, void* buffer, std::size_t* length) {
                const sockaddr_in6 address{
                    .sin6_family = AF_INET6,
                    .sin6_port = htons(1234),
                    .sin6_addr = IN6ADDR_LOOPBACK_INIT,
                };
                REQUIRE(*length >= sizeof(address));
                std::memcpy(buffer, &address, sizeof(address));
                *length = sizeof(address);
                return 0;
            },
    };
    fid_ep tx{.fid = {.ops = &ops}, .cm = &cm};
    fid_ep rx{.fid = {.ops = &ops}, .cm = &cm};
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .tx_ctx =
            [](fid_ep* sep, int index, fi_tx_attr*, fid_ep** output, void*) {
                REQUIRE(index == 1);
                *output = &static_cast<scalable_provider*>(sep->fid.context)->tx;
                return 0;
            },
        .rx_ctx =
            [](fid_ep* sep, int index, fi_rx_attr*, fid_ep** output, void*) {
                REQUIRE(index == 2);
                *output = &static_cast<scalable_provider*>(sep->fid.context)->rx;
                return 0;
            },
    };
    fid_ep sep{.fid = {.context = this, .ops = &ops}, .ops = &endpoint_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .scalable_ep =
            [](fid_domain* domain, fi_info*, fid_ep** output, void*) {
                *output = &static_cast<scalable_provider*>(domain->fid.context)->sep;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &ops}, .ops = &domain_ops};
};
}  // namespace

TEST_CASE("scalable endpoint contexts preserve parent metadata", "[endpoint][scalable]") {
    const bool receive = GENERATE(false, true);
    const bool triggered = GENERATE(false, true);
    scalable_provider native;
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native.domain_handle)};
    auto* raw_info = fi_allocinfo();
    REQUIRE(raw_info);
    raw_info->caps = FI_MSG | (triggered ? FI_TRIGGER : 0);
    raw_info->ep_attr->type = FI_EP_RDM;
    raw_info->ep_attr->max_msg_size = 16384;
    raw_info->tx_attr->inject_size = 32;
    raw_info->tx_attr->op_flags = FI_FENCE | FI_DELIVERY_COMPLETE;
    raw_info->rx_attr->op_flags = FI_COMPLETION;
    raw_info->ep_attr->tx_ctx_cnt = 2;
    raw_info->ep_attr->rx_ctx_cnt = 3;
    raw_info->addr_format = FI_SOCKADDR_IN6;
    loom::fabric_info info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), raw_info)};
    auto sep = loom::scalable_endpoint::create(domain, info);
    REQUIRE(sep);
    REQUIRE(sep->tx_context_count() == 2);
    REQUIRE(sep->rx_context_count() == 3);
    REQUIRE_FALSE(sep->tx_context(2));
    REQUIRE_FALSE(sep->rx_context(3));
    auto context = receive ? sep->rx_context(2) : sep->tx_context(1);
    REQUIRE(context);
    REQUIRE(context->impl_operation_flags(loom::completion_direction::transmit) ==
            (FI_FENCE | FI_DELIVERY_COMPLETE));
    REQUIRE(context->impl_operation_flags(loom::completion_direction::receive) == FI_COMPLETION);
    REQUIRE(loom::trigger::supports_trigger(*context) == triggered);
    REQUIRE(loom::rma::get_max_rma_size(*context) == 16384);
    REQUIRE(loom::rma::get_inject_size(*context) == (receive ? 0 : 32));
    REQUIRE(context->get_type().value() == loom::endpoint_type{loom::endpoint_types::rdm});
    const auto address = context->get_local_address();
    REQUIRE(address);
    REQUIRE(std::holds_alternative<loom::ipv6_address>(*address));
    REQUIRE(std::get<loom::ipv6_address>(*address).port() == 1234);
    REQUIRE(std::get<loom::ipv6_address>(*address).is_loopback());
}
