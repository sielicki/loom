// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/messaging.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct message_provider {
    int calls{};
    std::vector<void*> descriptors;

    static auto record(fid_ep* ep, void** desc, std::size_t count) -> ssize_t {
        auto& self = *static_cast<message_provider*>(ep->fid.context);
        ++self.calls;
        if (desc) {
            self.descriptors.assign(desc, desc + count);
        }
        return 0;
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
                      std::uint64_t) { return record(ep, msg->desc, msg->iov_count); },
        .sendv =
            [](fid_ep* ep, const iovec*, void** desc, std::size_t count, fi_addr_t, void*) {
                return record(ep, desc, count);
            },
        .sendmsg = [](fid_ep* ep,
                      const fi_msg* msg,
                      std::uint64_t) { return record(ep, msg->desc, msg->iov_count); },
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
                      std::uint64_t) { return record(ep, msg->desc, msg->iov_count); },
        .sendv = [](fid_ep* ep,
                    const iovec*,
                    void** desc,
                    std::size_t count,
                    fi_addr_t,
                    std::uint64_t,
                    void*) { return record(ep, desc, count); },
        .sendmsg = [](fid_ep* ep,
                      const fi_msg_tagged* msg,
                      std::uint64_t) { return record(ep, msg->desc, msg->iov_count); },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .msg = &messages, .tagged = &tags};
    fid_cntr counter_handle{.fid = {.ops = &ops}};
    fid_domain domain_handle{.fid = {.ops = &ops}};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::rdm,
                                                                       loom::address_format::inet)};
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
}  // namespace

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
