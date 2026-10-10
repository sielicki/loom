// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/messaging.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/counter_impl.hpp>
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

    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
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
};

struct context : loom::triggered_context<context> {};
}  // namespace

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
