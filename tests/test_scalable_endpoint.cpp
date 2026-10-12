// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/address_vector.hpp>
#include <loom/core/messaging.hpp>
#include <loom/core/rma.hpp>
#include <loom/core/scalable_endpoint.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <rdma/fi_cm.h>

#include <array>
#include <chrono>
#include <cstring>
#include <thread>
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
    const bool explicit_resource = GENERATE(false, true);
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
    raw_info->tx_attr->iov_limit = 3;
    raw_info->rx_attr->iov_limit = 5;
    raw_info->tx_attr->rma_iov_limit = 7;
    raw_info->ep_attr->tx_ctx_cnt = 2;
    raw_info->ep_attr->rx_ctx_cnt = 3;
    raw_info->addr_format = FI_SOCKADDR_IN6;
    loom::fabric_info info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), raw_info)};
    auto sep = loom::scalable_endpoint::create(
        domain, info, explicit_resource ? loom::get_default_resource() : nullptr);
    REQUIRE(sep);
    raw_info->tx_attr->iov_limit = 9;
    raw_info->rx_attr->iov_limit = 9;
    raw_info->tx_attr->rma_iov_limit = 9;
    REQUIRE(sep->tx_context_count() == 2);
    REQUIRE(sep->rx_context_count() == 3);
    REQUIRE_FALSE(sep->tx_context(2));
    REQUIRE_FALSE(sep->rx_context(3));
    auto context = receive ? sep->rx_context(2) : sep->tx_context(1);
    REQUIRE(context);
    CHECK(context->iov_limits() == loom::endpoint_iov_limits{3, 5, 7});
    CHECK(context->belongs_to(domain));
    loom::domain empty_domain;
    CHECK_FALSE(context->belongs_to(empty_domain));
    auto moved_domain = std::move(domain);
    CHECK_FALSE(context->belongs_to(domain));
    CHECK(context->belongs_to(moved_domain));
    domain = std::move(moved_domain);
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

TEST_CASE("sockets delivers to the encoded receive context", "[scalable][rx_context][.provider]") {
    using namespace loom;
    const auto bits = GENERATE(1UZ, 2UZ);
    CAPTURE(bits);
    fabric_hints hints;
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::named_rx_ctx;
    hints.addr_format = address_format::inet;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider with named receive contexts unavailable");
    auto* native_info = static_cast<fi_info*>(info->internal_ptr());
    REQUIRE(native_info->domain_attr->max_ep_rx_ctx >= 2);
    // Context-count configuration and SEP name lookup still require native access.
    native_info->ep_attr->tx_ctx_cnt = 1;
    native_info->ep_attr->rx_ctx_cnt = 2;
    auto fabric = fabric::create(*info).value();
    auto domain = domain::create(fabric, *info).value();
    std::array<std::array<std::byte, 24>, 2> source{}, target{};
    source[0].fill(std::byte{0x31});
    source[1].fill(std::byte{0x72});
    std::array<fi_context2, 2> sends{}, receives{};
    auto av = address_vector::create(domain, {.rx_ctx_bits = bits}).value();
    CHECK(av.receive_context_bits() == bits);
    auto send_queue = completion_queue::create(domain).value();
    std::array receive_queues{completion_queue::create(domain).value(),
                              completion_queue::create(domain).value()};
    auto sep = scalable_endpoint::create(domain, *info).value();
    REQUIRE(sep.bind_av(av));
    auto sender = sep.tx_context(0).value();
    std::array receivers{sep.rx_context(0).value(), sep.rx_context(1).value()};
    REQUIRE(sender.bind_cq(send_queue, cq_bind::transmit));
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        REQUIRE(receivers[i].bind_cq(receive_queues[i], cq_bind::recv));
        REQUIRE(receivers[i].enable());
    }
    REQUIRE(sender.enable());
    REQUIRE(sep.enable());
    sockaddr_in name{};
    auto length = sizeof(name);
    auto* native_sep = static_cast<fid_ep*>(sep.internal_ptr());
    REQUIRE(fi_getname(&native_sep->fid, &name, &length) == 0);
    REQUIRE(length == sizeof(name));
    REQUIRE(name.sin_family == AF_INET);
    const auto peer = av.insert(ipv4_address{ntohl(name.sin_addr.s_addr), ntohs(name.sin_port)});
    REQUIRE(peer);
    std::array destinations{av.receive_context_address(*peer, 0).value(),
                            av.receive_context_address(*peer, 1).value()};
    CHECK(destinations[0] != destinations[1]);
    for (std::size_t i = 0; i < receivers.size(); ++i)
        REQUIRE(recv(receivers[i], target[i], fabric_addrs::unspecified, {}, &receives[i]));

    std::array<bool, 2> sent{}, received{};
    auto poll = [&] {
        if (auto event = send_queue.poll()) {
            REQUIRE_FALSE(event->error);
            bool matched{};
            for (std::size_t i = 0; i < sends.size(); ++i) {
                if (event->context.get() == &sends[i]) {
                    CHECK_FALSE(sent[i]);
                    sent[i] = true;
                    matched = true;
                }
            }
            REQUIRE(matched);
        }
        for (std::size_t i = 0; i < receive_queues.size(); ++i) {
            if (auto event = receive_queues[i].poll()) {
                REQUIRE_FALSE(event->error);
                CHECK(event->context.get() == &receives[i]);
                CHECK(event->len == source[i].size());
                CHECK(target[i] == source[i]);
                CHECK_FALSE(received[i]);
                received[i] = true;
            }
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    // Send to context one first so a missing index cannot pass through FIFO ordering.
    for (auto i : {1UZ, 0UZ}) {
        void_result submitted;
        do {
            submitted = send(sender, source[i], destinations[i], {}, &sends[i]);
            if (submitted || submitted.error() != errc::again)
                break;
            poll();
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(submitted);
    }
    while ((!sent[0] || !sent[1] || !received[0] || !received[1]) &&
           std::chrono::steady_clock::now() < deadline) {
        poll();
        std::this_thread::yield();
    }
    CHECK(sent == std::array{true, true});
    CHECK(received == std::array{true, true});
    REQUIRE(sender.close());
    for (auto& receiver : receivers)
        REQUIRE(receiver.close());
}
