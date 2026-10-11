// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/messaging.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct message_provider {
    int calls{};
    int status{};
    std::uint64_t flags{}, tx_defaults{}, rx_defaults{}, data{}, tag{}, ignore{};
    fi_addr_t peer{};
    void* operation_context{};
    std::vector<void*> descriptors;

    static auto record(fid_ep* ep, void** desc, std::size_t count) -> ssize_t {
        auto& self = *static_cast<message_provider*>(ep->fid.context);
        ++self.calls;
        if (desc) {
            self.descriptors.assign(desc, desc + count);
        }
        return self.status;
    }

    static auto record_message(fid_ep* ep, const auto* msg, std::uint64_t flags) -> ssize_t {
        auto& self = *static_cast<message_provider*>(ep->fid.context);
        self.flags = flags;
        self.peer = msg->addr;
        self.operation_context = msg->context;
        self.data = msg->data;
        if constexpr (requires { msg->tag; }) {
            self.tag = msg->tag;
            self.ignore = msg->ignore;
        }
        return record(ep, msg->desc, msg->iov_count);
    }

    fi_ops ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
        .control = [](fid*, int command, void* argument) -> int {
            if (command == FI_CANCEL_WORK) {
                return 0;
            }
            REQUIRE(command == FI_QUEUE_WORK);
            const auto& work = *static_cast<const fi_deferred_work*>(argument);
            switch (work.op_type) {
                case FI_OP_SEND:
                case FI_OP_RECV:
                    return static_cast<int>(
                        record(work.op.msg->ep, work.op.msg->msg.desc, work.op.msg->msg.iov_count));
                case FI_OP_TSEND:
                case FI_OP_TRECV:
                    return static_cast<int>(record(work.op.tagged->ep,
                                                   work.op.tagged->msg.desc,
                                                   work.op.tagged->msg.iov_count));
                case FI_OP_READ:
                case FI_OP_WRITE:
                    return static_cast<int>(
                        record(work.op.rma->ep, work.op.rma->msg.desc, work.op.rma->msg.iov_count));
                default:
                    return -FI_EINVAL;
            }
        },
    };
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvv =
            [](fid_ep* ep, const iovec*, void** desc, std::size_t count, fi_addr_t, void*) {
                return record(ep, desc, count);
            },
        .recvmsg = [](fid_ep* ep,
                      const fi_msg* msg,
                      std::uint64_t flags) { return record_message(ep, msg, flags); },
        .sendv =
            [](fid_ep* ep, const iovec*, void** desc, std::size_t count, fi_addr_t, void*) {
                return record(ep, desc, count);
            },
        .sendmsg = [](fid_ep* ep,
                      const fi_msg* msg,
                      std::uint64_t flags) { return record_message(ep, msg, flags); },
    };
    fi_ops_tagged tags{
        .size = sizeof(fi_ops_tagged),
        .recvv = [](fid_ep* ep,
                    const iovec*,
                    void** desc,
                    std::size_t count,
                    fi_addr_t,
                    std::uint64_t,
                    std::uint64_t,
                    void*) { return record(ep, desc, count); },
        .recvmsg = [](fid_ep* ep,
                      const fi_msg_tagged* msg,
                      std::uint64_t flags) { return record_message(ep, msg, flags); },
        .sendv = [](fid_ep* ep,
                    const iovec*,
                    void** desc,
                    std::size_t count,
                    fi_addr_t,
                    std::uint64_t,
                    void*) { return record(ep, desc, count); },
        .sendmsg = [](fid_ep* ep,
                      const fi_msg_tagged* msg,
                      std::uint64_t flags) { return record_message(ep, msg, flags); },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .msg = &messages, .tagged = &tags};
    fid_cntr counter_handle{.fid = {.ops = &ops}};
    fid_domain domain_handle{.fid = {.ops = &ops}};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::rdm,
                                                                       loom::address_format::inet,
                                                                       0UZ,
                                                                       0UZ,
                                                                       loom::caps{},
                                                                       tx_defaults,
                                                                       rx_defaults)};
    }
    auto counter() -> loom::counter {
        return loom::counter{loom::detail::make_pmr_unique<loom::detail::counter_impl>(
            loom::get_default_resource(), &counter_handle)};
    }
    auto domain() -> loom::domain {
        return loom::domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
            loom::get_default_resource(), &domain_handle)};
    }
};

struct context : loom::triggered_context<context> {};

enum class unavailable_operation { endpoint, table, entry, layout };
}  // namespace

TEST_CASE("message APIs reject unavailable native operations", "[message][submission]") {
    const auto tagged = GENERATE(false, true);
    const auto receive = GENERATE(false, true);
    const auto unavailable = GENERATE(unavailable_operation::endpoint,
                                      unavailable_operation::table,
                                      unavailable_operation::entry,
                                      unavailable_operation::layout);
    message_provider native;
    auto ep = native.endpoint();
    switch (unavailable) {
        case unavailable_operation::endpoint:
            ep = {};
            break;
        case unavailable_operation::table:
            native.handle.msg = nullptr;
            native.handle.tagged = nullptr;
            break;
        case unavailable_operation::entry:
            native.messages = {.size = sizeof(fi_ops_msg)};
            native.tags = {.size = sizeof(fi_ops_tagged)};
            break;
        case unavailable_operation::layout:
            native.messages.size = sizeof(std::size_t);
            native.tags.size = sizeof(std::size_t);
            break;
    }
    using namespace loom;
    const auto result = tagged ? (receive ? msg::tagged_recvmsg(ep, msg::tagged_recv_message<>{})
                                          : msg::tagged_sendmsg(ep, msg::tagged_send_message<>{}))
                               : (receive ? msg::recvmsg(ep, msg::recv_message<>{})
                                          : msg::sendmsg(ep, msg::send_message<>{}));
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == (unavailable == unavailable_operation::endpoint
                                   ? errc::invalid_argument
                                   : errc::not_supported));
    REQUIRE(native.calls == 0);
}

TEST_CASE("deferred operations reject mismatched descriptor counts", "[message][trigger]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    const auto count = GENERATE(0, 1, 20, 21);
    CAPTURE(operation, count);
    message_provider native;
    auto ep = native.endpoint();
    auto counter = native.counter();
    auto domain = native.domain();
    const loom::trigger::threshold_condition trigger{&counter, 1};
    std::array<std::byte, 20> storage{};
    std::array<iovec, 20> iov{};
    for (std::size_t i = 0; i < iov.size(); ++i) {
        iov[i] = {storage.data() + i, 1};
    }
    const std::vector descriptors(static_cast<std::size_t>(count), loom::mr_descriptor{&storage});
    const std::array remote{loom::rma::rma_iov{0x1000, storage.size(), 1}};
    auto work = [&]() -> loom::result<loom::trigger::deferred_work> {
        using namespace loom;
        using trigger::deferred_work;
        switch (operation) {
            case 0:
                return deferred_work::create_send(
                    domain,
                    ep,
                    trigger,
                    nullptr,
                    msg::send_message<void*>{.iov = iov, .desc = descriptors});
            case 1:
                return deferred_work::create_recv(
                    domain,
                    ep,
                    trigger,
                    nullptr,
                    msg::recv_message<void*>{.iov = iov, .desc = descriptors});
            case 2:
                return deferred_work::create_tagged_send(
                    domain,
                    ep,
                    trigger,
                    nullptr,
                    msg::tagged_send_message<void*>{.iov = iov, .desc = descriptors});
            case 3:
                return deferred_work::create_tagged_recv(
                    domain,
                    ep,
                    trigger,
                    nullptr,
                    msg::tagged_recv_message<void*>{.iov = iov, .desc = descriptors});
            case 4:
                return deferred_work::create_read(
                    domain, ep, trigger, nullptr, iov, descriptors, remote);
            default:
                return deferred_work::create_write(
                    domain, ep, trigger, nullptr, iov, descriptors, remote);
        }
    }();
    if (count == 0 || count == 20) {
        REQUIRE(work);
        REQUIRE(native.calls == 1);
        REQUIRE(native.descriptors ==
                std::vector<void*>(static_cast<std::size_t>(count), &storage));
    } else {
        REQUIRE(work.error() == loom::errc::invalid_argument);
        REQUIRE(native.calls == 0);
    }
}

TEST_CASE("message vectors preserve registrations and reject mismatched counts", "[message]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    const auto count = GENERATE(0, 1, 20, 21);
    CAPTURE(operation, count);
    message_provider native;
    auto ep = native.endpoint();
    auto counter = native.counter();
    context trigger_context;
    const loom::trigger::threshold_condition trigger{&counter, 1};
    std::array<std::byte, 21> storage{};
    std::array<iovec, 20> iov{};
    std::vector<loom::mr_descriptor> descriptors;
    std::vector<void*> expected;
    for (std::size_t i = 0; i < iov.size(); ++i) {
        iov[i] = {storage.data() + i, 1};
    }
    for (int i = 0; i < count; ++i) {
        descriptors.emplace_back(storage.data() + i);
        expected.push_back(storage.data() + i);
    }
    const auto result = [&]() -> loom::void_result {
        using namespace loom;
        switch (operation) {
            case 0:
                return sendv(ep, iov, descriptors, fabric_addrs::unspecified);
            case 1:
                return recvv(ep, iov, descriptors, fabric_addrs::unspecified);
            case 2:
                return tagged_sendv(ep, iov, descriptors, fabric_addrs::unspecified, 42);
            case 3:
                return tagged_recvv(ep, iov, descriptors, fabric_addrs::unspecified, 42);
            case 4:
                return msg::sendmsg(ep, msg::send_message<void*>{.iov = iov, .desc = descriptors});
            case 5:
                return msg::recvmsg(ep, msg::recv_message<void*>{.iov = iov, .desc = descriptors});
            case 6:
                return msg::tagged_sendmsg(
                    ep, msg::tagged_send_message<void*>{.iov = iov, .desc = descriptors});
            case 7:
                return msg::tagged_recvmsg(
                    ep, msg::tagged_recv_message<void*>{.iov = iov, .desc = descriptors});
            case 8:
                return trigger::sendmsg(ep,
                                        msg::send_message<context*>{.iov = iov,
                                                                    .desc = descriptors,
                                                                    .context = &trigger_context},
                                        trigger);
            case 9:
                return trigger::recvmsg(ep,
                                        msg::recv_message<context*>{.iov = iov,
                                                                    .desc = descriptors,
                                                                    .context = &trigger_context},
                                        trigger);
            case 10:
                return trigger::tagged_sendmsg(
                    ep,
                    msg::tagged_send_message<context*>{
                        .iov = iov, .desc = descriptors, .context = &trigger_context},
                    trigger);
            default:
                return trigger::tagged_recvmsg(
                    ep,
                    msg::tagged_recv_message<context*>{
                        .iov = iov, .desc = descriptors, .context = &trigger_context},
                    trigger);
        }
    }();
    if (count == 0 || count == 20) {
        REQUIRE(result);
        REQUIRE(native.calls == 1);
        REQUIRE(native.descriptors == expected);
    } else {
        REQUIRE(result.error() == loom::errc::invalid_argument);
        REQUIRE(native.calls == 0);
    }
}

TEST_CASE("message flags distinguish inherited direction from explicit overrides",
          "[message][flags]") {
    using namespace loom;
    const auto tagged = GENERATE(false, true);
    const auto receive = GENERATE(false, true);
    const auto inherit = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    message_provider native;
    native.status = status;
    native.tx_defaults = FI_DELIVERY_COMPLETE | FI_FENCE;
    native.rx_defaults = FI_MORE;
    auto ep = native.endpoint();
    std::array<std::byte, 8> bytes{};
    const std::array iov{iovec{bytes.data(), 3}, iovec{bytes.data() + 3, 5}};
    int first{}, second{}, context{};
    const std::array desc{mr_descriptor{&first}, mr_descriptor{&second}};
    const fabric_addr peer{19};
    auto submit = [&]<operation_defaults Defaults>() -> void_result {
        if (tagged) {
            if (receive)
                return msg::tagged_recvmsg<Defaults>(
                    ep,
                    msg::tagged_recv_message<>{.iov = iov,
                                               .desc = desc,
                                               .src_addr = peer,
                                               .tag = 42,
                                               .ignore = 0x80,
                                               .context = &context},
                    msg::recv_flag::completion);
            return msg::tagged_sendmsg<Defaults>(ep,
                                                 msg::tagged_send_message<>{.iov = iov,
                                                                            .desc = desc,
                                                                            .dest_addr = peer,
                                                                            .tag = 42,
                                                                            .context = &context,
                                                                            .data = 23},
                                                 msg::send_flag::completion |
                                                     msg::send_flag::remote_cq_data);
        }
        if (receive)
            return msg::recvmsg<Defaults>(
                ep,
                msg::recv_message<>{
                    .iov = iov, .desc = desc, .src_addr = peer, .context = &context},
                msg::recv_flag::completion);
        return msg::sendmsg<Defaults>(
            ep,
            msg::send_message<>{
                .iov = iov, .desc = desc, .dest_addr = peer, .context = &context, .data = 23},
            msg::send_flag::completion | msg::send_flag::remote_cq_data);
    };
    const auto result = inherit ? submit.template operator()<operation_defaults::inherit>()
                                : submit.template operator()<operation_defaults::replace>();
    REQUIRE(native.calls == 1);
    REQUIRE(native.flags == (FI_COMPLETION | (receive ? 0 : FI_REMOTE_CQ_DATA) |
                             (inherit ? (receive ? native.rx_defaults : native.tx_defaults) : 0)));
    REQUIRE(native.peer == peer.get());
    REQUIRE(native.operation_context == &context);
    REQUIRE(native.data == (receive ? 0 : 23));
    REQUIRE(native.tag == (tagged ? 42 : 0));
    REQUIRE(native.ignore == (tagged && receive ? 0x80 : 0));
    REQUIRE(native.descriptors == std::vector<void*>{&first, &second});
    if (status)
        REQUIRE(result.error() == make_error_code_from_fi_errno(-status));
    else
        REQUIRE(result);
}

TEST_CASE("selective message completions accompany sockets transfers", "[message][loopback]") {
    using namespace loom;
    const auto tagged = GENERATE(false, true);
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::tagged;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto* native_info = static_cast<fi_info*>(info->internal_ptr());
    native_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
    native_info->rx_attr->op_flags = 0;
    auto fab = fabric::create(*info);
    REQUIRE(fab);
    auto dom = domain::create(*fab, *info);
    REQUIRE(dom);
    auto cq = completion_queue::create(*dom);
    REQUIRE(cq);
    auto av = address_vector::create(*dom);
    REQUIRE(av);
    auto ep = endpoint::create(*dom, *info);
    REQUIRE(ep);
    REQUIRE(ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
    REQUIRE(ep->bind_av(*av));
    REQUIRE(ep->enable());
    auto address = ep->get_local_address();
    REQUIRE(address);
    auto peer = av->insert(*address);
    REQUIRE(peer);
    std::array<std::byte, 8> outgoing{}, incoming{};
    outgoing.fill(std::byte{42});
    auto send_mr = memory_region::register_memory(*dom, outgoing, mr_access_flags::send);
    REQUIRE(send_mr);
    auto recv_mr = memory_region::register_memory(*dom, incoming, mr_access_flags::recv);
    REQUIRE(recv_mr);
    const std::array send_iov{iovec{outgoing.data(), outgoing.size()}};
    const std::array recv_iov{iovec{incoming.data(), incoming.size()}};
    const std::array send_desc{send_mr->descriptor()}, recv_desc{recv_mr->descriptor()};
    fi_context2 send_context{}, recv_context{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    auto post = [&](auto submit) {
        auto result = submit();
        while (!result && result.error() == errc::again &&
               std::chrono::steady_clock::now() < deadline) {
            (void)cq->progress();
            std::this_thread::yield();
            result = submit();
        }
        return result;
    };
    REQUIRE(post([&] {
        if (tagged)
            return msg::tagged_recvmsg<operation_defaults::inherit>(
                *ep,
                msg::tagged_recv_message<>{
                    .iov = recv_iov, .desc = recv_desc, .tag = 42, .context = &recv_context},
                msg::recv_flag::completion);
        return msg::recvmsg<operation_defaults::inherit>(
            *ep,
            msg::recv_message<>{.iov = recv_iov, .desc = recv_desc, .context = &recv_context},
            msg::recv_flag::completion);
    }));
    const auto submitted = post([&] {
        if (tagged)
            return msg::tagged_sendmsg<operation_defaults::inherit>(
                *ep,
                msg::tagged_send_message<>{.iov = send_iov,
                                           .desc = send_desc,
                                           .dest_addr = fabric_addr{peer->value},
                                           .tag = 42,
                                           .context = &send_context},
                msg::send_flag::completion);
        return msg::sendmsg<operation_defaults::inherit>(
            *ep,
            msg::send_message<>{.iov = send_iov,
                                .desc = send_desc,
                                .dest_addr = fabric_addr{peer->value},
                                .context = &send_context},
            msg::send_flag::completion);
    });
    std::array<std::optional<completion_event>, 2> completions;
    std::size_t completed{};
    while (submitted && completed < completions.size() &&
           std::chrono::steady_clock::now() < deadline) {
        if (auto event = cq->poll())
            completions[completed++] = std::move(event);
        std::this_thread::yield();
    }
    // Retire native work before assertions can release borrowed test buffers.
    REQUIRE(ep->close());
    REQUIRE(submitted);
    REQUIRE(completed == 2);
    int sent{}, received{};
    for (const auto& event : completions) {
        REQUIRE_FALSE(event->error);
        if (event->context.get() == &recv_context) {
            ++received;
            REQUIRE(event->bytes_transferred == incoming.size());
            if (tagged)
                REQUIRE(event->tag == 42);
        } else {
            REQUIRE(event->context.get() == &send_context);
            ++sent;
        }
    }
    REQUIRE(sent == 1);
    REQUIRE(received == 1);
    REQUIRE(incoming == outgoing);
}
