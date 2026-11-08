// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/address_vector.hpp>
#include <loom/core/messaging.hpp>
#include <loom/core/rma.hpp>
#include <loom/core/scalable_endpoint.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>
#include <loom/detail/impl/scalable_endpoint_impl.hpp>

#include <rdma/fi_cm.h>

#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <tuple>
#include <variant>

#include <netinet/in.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
auto attribute_fields(const fi_tx_attr& attr) {
    return std::tie(attr.caps,
                    attr.mode,
                    attr.op_flags,
                    attr.msg_order,
                    attr.comp_order,
                    attr.inject_size,
                    attr.size,
                    attr.iov_limit,
                    attr.rma_iov_limit,
                    attr.tclass);
}
auto attribute_fields(const fi_rx_attr& attr) {
    return std::tie(attr.caps,
                    attr.mode,
                    attr.op_flags,
                    attr.msg_order,
                    attr.comp_order,
                    attr.total_buffered_recv,
                    attr.size,
                    attr.iov_limit);
}

struct scalable_provider {
    int context_calls{};
    int context_closes{};
    int context_result{};
    int expected_tx_index{1};
    int expected_rx_index{2};
    int submissions{};
    std::uint64_t submitted_flags{};
    std::optional<fi_tx_attr> requested_tx;
    std::optional<fi_rx_attr> requested_rx;
    fi_tx_attr returned_tx{};
    fi_rx_attr returned_rx{};
    fi_tx_attr tx_defaults{.caps = FI_MSG | FI_TRIGGER,
                           .mode = FI_CONTEXT2,
                           .op_flags = FI_FENCE,
                           .inject_size = 32,
                           .size = 64,
                           .iov_limit = 3,
                           .rma_iov_limit = 7};
    fi_rx_attr rx_defaults{.caps = FI_MSG | FI_TRIGGER,
                           .mode = FI_CONTEXT2,
                           .op_flags = FI_COMPLETION,
                           .size = 128,
                           .iov_limit = 5};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid* handle) {
                   auto& self = *static_cast<scalable_provider*>(handle->context);
                   if (handle == &self.tx.fid || handle == &self.rx.fid)
                       ++self.context_closes;
                   return 0;
               }};
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
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvmsg = [](fid_ep* ep, const fi_msg*, std::uint64_t flags) -> ssize_t {
            auto& self = *static_cast<scalable_provider*>(ep->fid.context);
            ++self.submissions;
            self.submitted_flags = flags;
            return 0;
        },
        .sendmsg = [](fid_ep* ep, const fi_msg*, std::uint64_t flags) -> ssize_t {
            auto& self = *static_cast<scalable_provider*>(ep->fid.context);
            ++self.submissions;
            self.submitted_flags = flags;
            return 0;
        },
    };
    fid_ep tx{.fid = {.context = this, .ops = &ops}, .cm = &cm, .msg = &messages};
    fid_ep rx{.fid = {.context = this, .ops = &ops}, .cm = &cm, .msg = &messages};
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .tx_ctx =
            [](fid_ep* sep, int index, fi_tx_attr* attr, fid_ep** output, void*) {
                auto& self = *static_cast<scalable_provider*>(sep->fid.context);
                REQUIRE(index == self.expected_tx_index);
                ++self.context_calls;
                if (attr) {
                    self.requested_tx = *attr;
                    *attr = self.returned_tx;
                }
                if (self.context_result)
                    return self.context_result;
                *output = &self.tx;
                return 0;
            },
        .rx_ctx =
            [](fid_ep* sep, int index, fi_rx_attr* attr, fid_ep** output, void*) {
                auto& self = *static_cast<scalable_provider*>(sep->fid.context);
                REQUIRE(index == self.expected_rx_index);
                ++self.context_calls;
                if (attr) {
                    self.requested_rx = *attr;
                    *attr = self.returned_rx;
                }
                if (self.context_result)
                    return self.context_result;
                *output = &self.rx;
                return 0;
            },
    };
    fid_ep sep{.fid = {.context = this, .ops = &ops}, .ops = &endpoint_ops, .cm = &cm};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .scalable_ep =
            [](fid_domain* domain, fi_info*, fid_ep** output, void*) {
                *output = &static_cast<scalable_provider*>(domain->fid.context)->sep;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &ops}, .ops = &domain_ops};

    auto adopt(std::size_t tx_count = 2, std::size_t rx_count = 3, std::uint64_t mode = 0)
        -> loom::scalable_endpoint {
        return loom::scalable_endpoint{
            loom::detail::make_pmr_unique<loom::detail::scalable_endpoint_impl>(
                loom::get_default_resource(),
                &sep,
                tx_count,
                rx_count,
                loom::endpoint_types::rdm,
                loom::address_format::inet6,
                16384UZ,
                tx_defaults.inject_size,
                loom::capability::msg | loom::capability::trigger,
                tx_defaults.op_flags,
                rx_defaults.op_flags,
                &domain_handle,
                loom::endpoint_iov_limits{
                    tx_defaults.iov_limit, rx_defaults.iov_limit, tx_defaults.rma_iov_limit},
                &tx_defaults,
                &rx_defaults,
                mode)};
    }
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
    const auto expected_tx = *raw_info->tx_attr;
    const auto expected_rx = *raw_info->rx_attr;
    auto sep = loom::scalable_endpoint::create(
        domain, info, explicit_resource ? loom::get_default_resource() : nullptr);
    REQUIRE(sep);
    const auto local_address = sep->get_local_address();
    REQUIRE(local_address);
    REQUIRE(std::holds_alternative<loom::ipv6_address>(*local_address));
    CHECK(std::get<loom::ipv6_address>(*local_address).is_loopback());
    CHECK(std::get<loom::ipv6_address>(*local_address).port() == 1234);
    raw_info->tx_attr->iov_limit = 9;
    raw_info->rx_attr->iov_limit = 9;
    raw_info->tx_attr->rma_iov_limit = 9;
    REQUIRE(sep->tx_context_attributes());
    REQUIRE(sep->rx_context_attributes());
    CHECK(attribute_fields(*sep->tx_context_attributes()) == attribute_fields(expected_tx));
    CHECK(attribute_fields(*sep->rx_context_attributes()) == attribute_fields(expected_rx));
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
    const auto configured = GENERATE(false, true);
    CAPTURE(bits, configured);
    fabric_hints hints;
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::named_rx_ctx;
    hints.addr_format = address_format::inet;
    hints.tx_context_count = queue_size{1UZ};
    hints.rx_context_count = queue_size{2UZ};
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider with named receive contexts unavailable");
    REQUIRE(info->get_endpoint_attr()->tx_ctx_cnt == hints.tx_context_count);
    REQUIRE(info->get_endpoint_attr()->rx_ctx_cnt == hints.rx_context_count);
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
    auto tx_defaults = sep.tx_context_attributes();
    auto rx_defaults = sep.rx_context_attributes();
    REQUIRE(tx_defaults);
    REQUIRE(rx_defaults);
    auto tx_attributes = *tx_defaults;
    auto rx_attributes = *rx_defaults;
    CAPTURE(tx_attributes.op_flags,
            tx_attributes.size,
            tx_attributes.iov_limit,
            tx_attributes.rma_iov_limit,
            rx_attributes.size,
            rx_attributes.iov_limit);
    tx_attributes.size = 16;
    tx_attributes.iov_limit = 1;
    tx_attributes.rma_iov_limit = 1;
    rx_attributes.size = 16;
    rx_attributes.iov_limit = 1;
    auto sender_result = configured ? sep.tx_context(0, tx_attributes) : sep.tx_context(0);
    INFO("transmit context: " << (sender_result ? "ok" : sender_result.error().message()));
    REQUIRE(sender_result);
    auto sender = std::move(*sender_result);
    auto first_receiver = configured ? sep.rx_context(0, rx_attributes) : sep.rx_context(0);
    INFO("receive context 0: " << (first_receiver ? "ok" : first_receiver.error().message()));
    REQUIRE(first_receiver);
    auto second_receiver = configured ? sep.rx_context(1, rx_attributes) : sep.rx_context(1);
    INFO("receive context 1: " << (second_receiver ? "ok" : second_receiver.error().message()));
    REQUIRE(second_receiver);
    std::array receivers{std::move(*first_receiver), std::move(*second_receiver)};
    if (configured) {
        CHECK(sender.iov_limits().transmit == tx_attributes.iov_limit);
        CHECK(sender.impl_operation_flags(completion_direction::transmit) ==
              tx_attributes.op_flags);
        for (const auto& receiver : receivers)
            CHECK(receiver.iov_limits().receive == rx_attributes.iov_limit);
    }
    REQUIRE(sender.bind_cq(send_queue, cq_bind::transmit));
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        REQUIRE(receivers[i].bind_cq(receive_queues[i], cq_bind::recv));
        REQUIRE(receivers[i].enable());
    }
    REQUIRE(sender.enable());
    REQUIRE(sep.enable());
    const auto local_address = sep.get_local_address();
    REQUIRE(local_address);
    REQUIRE(get_address_format(*local_address) == address_format::inet);
    const auto peer = av.insert(*local_address);
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

TEST_CASE("context-count hints reject shared-context ambiguity", "[scalable][fabric_query]") {
    using namespace loom;
    const auto explicit_resource = GENERATE(false, true);
    fabric_hints hints;
    hints.provider_name = "loom_nonexistent_provider";
    SECTION("transmit sentinel") {
        hints.tx_context_count = queue_size{FI_SHARED_CONTEXT};
    }
    SECTION("receive sentinel") {
        hints.rx_context_count = queue_size{FI_SHARED_CONTEXT};
    }
    SECTION("receive sentinel with shared mode") {
        hints.shared_receive_context = true;
        hints.rx_context_count = queue_size{FI_SHARED_CONTEXT};
    }
    SECTION("receive count with shared mode") {
        hints.shared_receive_context = true;
        hints.rx_context_count = queue_size{1UZ};
    }
    const auto info = query_fabric(hints, explicit_resource ? get_default_resource() : nullptr);
    REQUIRE_FALSE(info);
    CHECK(info.error() == errc::invalid_argument);
}

TEST_CASE("sockets negotiates scalable endpoint context counts",
          "[scalable][fabric_query][.provider]") {
    using namespace loom;
    const auto transmit = GENERATE(0UZ, 1UZ, 2UZ);
    const auto receive = GENERATE(0UZ, 1UZ, 3UZ);
    const auto explicit_resource = GENERATE(false, true);
    CAPTURE(transmit, receive, explicit_resource);
    fabric_hints hints;
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg;
    const auto available = query_fabric(hints);
    if (!available)
        SKIP("Sockets provider unavailable");
    hints.tx_context_count = queue_size{transmit};
    hints.rx_context_count = queue_size{receive};
    auto info = query_fabric(hints, explicit_resource ? get_default_resource() : nullptr);
    REQUIRE(info);
    const auto attributes = info->get_endpoint_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->tx_ctx_cnt.get() > 0);
    REQUIRE(attributes->rx_ctx_cnt.get() > 0);
    if (transmit)
        CHECK(attributes->tx_ctx_cnt == hints.tx_context_count);
    if (receive)
        CHECK(attributes->rx_ctx_cnt == hints.rx_context_count);
    CHECK_FALSE(attributes->shared_receive_context);
    auto fabric = fabric::create(*info).value();
    auto domain = domain::create(fabric, *info).value();
    auto sep = scalable_endpoint::create(domain, *info).value();
    CHECK(sep.tx_context_count() == attributes->tx_ctx_cnt.get());
    CHECK(sep.rx_context_count() == attributes->rx_ctx_cnt.get());
    auto sender = sep.tx_context(sep.tx_context_count() - 1);
    auto receiver = sep.rx_context(sep.rx_context_count() - 1);
    REQUIRE(sender);
    REQUIRE(receiver);
    CHECK(sep.tx_context(sep.tx_context_count()).error() == errc::invalid_argument);
    CHECK(sep.rx_context(sep.rx_context_count()).error() == errc::invalid_argument);
    REQUIRE(sender->close());
    REQUIRE(receiver->close());
}

TEST_CASE("discovery rejects context counts exceeding provider limits",
          "[scalable][fabric_query][.provider]") {
    using namespace loom;
    const auto receive = GENERATE(false, true);
    fabric_hints hints;
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg;
    const auto available = query_fabric(hints);
    if (!available)
        SKIP("Sockets provider unavailable");
    const auto attributes = available->get_domain_attr();
    REQUIRE(attributes);
    const auto maximum = receive ? attributes->max_ep_rx_ctx : attributes->max_ep_tx_ctx;
    REQUIRE(maximum < std::numeric_limits<std::size_t>::max() - 1);
    (receive ? hints.rx_context_count : hints.tx_context_count) = queue_size{maximum + 1};
    const auto info = query_fabric(hints);
    REQUIRE_FALSE(info);
    CHECK(info.error() == errc::no_data);
}

TEST_CASE("custom contexts use returned attributes for submissions", "[scalable][attributes]") {
    using namespace loom;
    const auto receive = GENERATE(false, true);
    const auto triggered = GENERATE(false, true);
    const auto limit = GENERATE(0UZ, 1UZ);
    CAPTURE(receive, triggered, limit);
    scalable_provider native;
    auto sep = native.adopt();
    auto tx = sep.tx_context_attributes().value();
    auto rx = sep.rx_context_attributes().value();
    tx.tclass = 0x12345678U;
    tx.msg_order = FI_ORDER_SAS;
    tx.comp_order = FI_ORDER_STRICT;
    rx.total_buffered_recv = 123;
    rx.msg_order = FI_ORDER_RAR;
    rx.comp_order = FI_ORDER_DATA;
    const auto requested_tx = tx;
    const auto requested_rx = rx;
    native.returned_tx = {.caps = FI_MSG | (triggered ? FI_TRIGGER : 0),
                          .mode = FI_CONTEXT,
                          .op_flags = FI_DELIVERY_COMPLETE | FI_MORE,
                          .msg_order = FI_ORDER_WAW,
                          .comp_order = FI_ORDER_DATA,
                          .inject_size = 7,
                          .size = 16,
                          .iov_limit = limit,
                          .rma_iov_limit = limit,
                          .tclass = 0x87654321U};
    native.returned_rx = {.caps = FI_MSG | (triggered ? FI_TRIGGER : 0),
                          .mode = FI_CONTEXT,
                          .op_flags = FI_MORE,
                          .msg_order = FI_ORDER_SAS,
                          .comp_order = FI_ORDER_NONE,
                          .total_buffered_recv = 37,
                          .size = 32,
                          .iov_limit = limit};
    auto context = receive ? sep.rx_context(2, rx) : sep.tx_context(1, tx);
    REQUIRE(context);
    CHECK(native.context_calls == 1);
    if (receive) {
        REQUIRE(native.requested_rx);
        CHECK(attribute_fields(*native.requested_rx) == attribute_fields(requested_rx));
        CHECK(attribute_fields(rx) == attribute_fields(native.returned_rx));
        CHECK(context->iov_limits() == endpoint_iov_limits{3, limit, 7});
        CHECK(context->impl_operation_flags(completion_direction::receive) == rx.op_flags);
        CHECK(context->impl_operation_flags(completion_direction::transmit) == FI_FENCE);
        CHECK(rma::get_inject_size(*context) == 0);
    } else {
        REQUIRE(native.requested_tx);
        CHECK(attribute_fields(*native.requested_tx) == attribute_fields(requested_tx));
        CHECK(attribute_fields(tx) == attribute_fields(native.returned_tx));
        CHECK(context->iov_limits() == endpoint_iov_limits{limit, 5, limit});
        CHECK(context->impl_operation_flags(completion_direction::transmit) == tx.op_flags);
        CHECK(context->impl_operation_flags(completion_direction::receive) == FI_COMPLETION);
        CHECK(rma::get_inject_size(*context) == 7);
    }
    CHECK(trigger::supports_trigger(*context) == triggered);
    CHECK(rma::get_max_rma_size(*context) == 16384);
    CHECK(attribute_fields(*sep.tx_context_attributes()) == attribute_fields(native.tx_defaults));
    CHECK(attribute_fields(*sep.rx_context_attributes()) == attribute_fields(native.rx_defaults));

    std::array<std::byte, 2> bytes{};
    fi_context2 storage{};
    const std::array buffers{iovec{bytes.data(), 1}, iovec{bytes.data() + 1, 1}};
    const auto submit = [&](std::size_t count) {
        const auto iov = std::span{buffers}.first(count);
        if (receive)
            return msg::recvmsg<operation_defaults::inherit>(
                *context, msg::recv_message<>{.iov = iov, .context = &storage});
        return msg::sendmsg<operation_defaults::inherit>(
            *context,
            msg::send_message<>{.iov = iov, .dest_addr = fabric_addr{0ULL}, .context = &storage});
    };
    CHECK(submit(limit + 1).error() == errc::invalid_argument);
    CHECK(native.submissions == 0);
    REQUIRE(submit(limit));
    CHECK(native.submissions == 1);
    CHECK(native.submitted_flags == (receive ? rx.op_flags : tx.op_flags));
}

TEST_CASE("context creation preserves requested attributes on native failure",
          "[scalable][attributes]") {
    using namespace loom;
    const auto receive = GENERATE(false, true);
    const auto error = GENERATE(FI_EIO, FI_ENOSYS, FI_EINVAL);
    scalable_provider native;
    native.context_result = -error;
    auto sep = native.adopt();
    auto tx = sep.tx_context_attributes().value();
    auto rx = sep.rx_context_attributes().value();
    const auto result = receive ? sep.rx_context(2, rx) : sep.tx_context(1, tx);
    REQUIRE_FALSE(result);
    CHECK(result.error() == make_error_code_from_fi_errno(error));
    CHECK(native.context_calls == 1);
    CHECK(attribute_fields(tx) == attribute_fields(native.tx_defaults));
    CHECK(attribute_fields(rx) == attribute_fields(native.rx_defaults));
}

TEST_CASE("scalable context creation checks native support and index bounds",
          "[scalable][attributes]") {
    using namespace loom;
    enum class support { available, missing_table, missing_member, truncated, empty };
    const auto receive = GENERATE(false, true);
    const auto configured = GENERATE(false, true);
    const auto state = GENERATE(support::available,
                                support::missing_table,
                                support::missing_member,
                                support::truncated,
                                support::empty);
    const auto index = GENERATE(0UZ,
                                static_cast<std::size_t>(std::numeric_limits<int>::max()),
                                static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1,
                                std::numeric_limits<std::size_t>::max());
    CAPTURE(receive, configured, state, index);
    scalable_provider native;
    auto sep = native.adopt(std::numeric_limits<std::size_t>::max(),
                            std::numeric_limits<std::size_t>::max());
    if (std::in_range<int>(index))
        native.expected_tx_index = native.expected_rx_index = static_cast<int>(index);
    if (state == support::missing_table)
        native.sep.ops = nullptr;
    if (state == support::missing_member) {
        if (receive)
            native.endpoint_ops.rx_ctx = nullptr;
        else
            native.endpoint_ops.tx_ctx = nullptr;
    }
    if (state == support::truncated)
        native.endpoint_ops.size =
            receive ? offsetof(fi_ops_ep, rx_ctx) : offsetof(fi_ops_ep, tx_ctx);
    if (state == support::empty)
        sep = {};
    auto tx = native.tx_defaults;
    auto rx = native.rx_defaults;
    auto result = receive ? (configured ? sep.rx_context(index, rx) : sep.rx_context(index))
                          : (configured ? sep.tx_context(index, tx) : sep.tx_context(index));
    if (state == support::empty || !std::in_range<int>(index)) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == errc::invalid_argument);
        CHECK(native.context_calls == 0);
    } else if (state != support::available) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == errc::not_supported);
        CHECK(native.context_calls == 0);
    } else {
        REQUIRE(result);
        CHECK(native.context_calls == 1);
    }
    if (!result) {
        CHECK(attribute_fields(tx) == attribute_fields(native.tx_defaults));
        CHECK(attribute_fields(rx) == attribute_fields(native.rx_defaults));
    }
}

TEST_CASE("context defaults survive moves and report unavailable metadata",
          "[scalable][attributes]") {
    using namespace loom;
    scalable_provider native;
    auto sep = native.adopt();
    auto moved = std::move(sep);
    CHECK(sep.tx_context_attributes().error() == errc::invalid_argument);
    CHECK(sep.rx_context_attributes().error() == errc::invalid_argument);
    CHECK(attribute_fields(*moved.tx_context_attributes()) == attribute_fields(native.tx_defaults));
    CHECK(attribute_fields(*moved.rx_context_attributes()) == attribute_fields(native.rx_defaults));
    auto impl = detail::make_pmr_unique<detail::scalable_endpoint_impl>(get_default_resource());
    impl->sep = &native.sep;
    scalable_endpoint missing{std::move(impl)};
    CHECK(missing.tx_context_attributes().error() == errc::not_supported);
    CHECK(missing.rx_context_attributes().error() == errc::not_supported);
}

TEST_CASE("scalable contexts reject asynchronous descriptor retention", "[scalable][attributes]") {
    using namespace loom;
    const auto receive = GENERATE(false, true);
    enum class mode_source { endpoint, inherited, requested, returned };
    const auto source = GENERATE(mode_source::endpoint,
                                 mode_source::inherited,
                                 mode_source::requested,
                                 mode_source::returned);
    scalable_provider native;
    if (source == mode_source::inherited) {
        native.tx_defaults.mode |= FI_ASYNC_IOV;
        native.rx_defaults.mode |= FI_ASYNC_IOV;
    }
    auto sep = native.adopt(2, 3, source == mode_source::endpoint ? FI_ASYNC_IOV : 0);
    auto tx = native.tx_defaults;
    auto rx = native.rx_defaults;
    native.returned_tx = tx;
    native.returned_rx = rx;
    if (source == mode_source::requested) {
        tx.mode |= FI_ASYNC_IOV;
        rx.mode |= FI_ASYNC_IOV;
    } else if (source == mode_source::returned) {
        native.returned_tx.mode |= FI_ASYNC_IOV;
        native.returned_rx.mode |= FI_ASYNC_IOV;
    }
    const auto original_tx = tx;
    const auto original_rx = rx;
    const auto result = source == mode_source::inherited || source == mode_source::endpoint
                            ? (receive ? sep.rx_context(2) : sep.tx_context(1))
                            : (receive ? sep.rx_context(2, rx) : sep.tx_context(1, tx));
    REQUIRE_FALSE(result);
    CHECK(result.error() == errc::not_supported);
    CHECK(native.context_calls == (source == mode_source::returned ? 1 : 0));
    CHECK(native.context_closes == (source == mode_source::returned ? 1 : 0));
    CHECK(attribute_fields(tx) == attribute_fields(original_tx));
    CHECK(attribute_fields(rx) == attribute_fields(original_rx));
}
