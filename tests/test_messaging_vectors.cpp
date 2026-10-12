// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/messaging.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/shared_context_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

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
                        record_message(work.op.msg->ep, &work.op.msg->msg, work.op.msg->flags));
                case FI_OP_TSEND:
                case FI_OP_TRECV:
                    return static_cast<int>(record_message(
                        work.op.tagged->ep, &work.op.tagged->msg, work.op.tagged->flags));
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
        return loom::endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(),
            &handle,
            loom::endpoint_types::rdm,
            loom::address_format::inet,
            0UZ,
            0UZ,
            loom::caps{},
            tx_defaults,
            rx_defaults,
            nullptr,
            loom::endpoint_iov_limits{64, 64, 64})};
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

struct receive_flag_case {
    loom::msg::recv_flags flags;
    std::uint64_t native;
    bool untagged_valid;
    bool tagged_valid;
    bool needs_context;
};

constexpr auto receive_flag_cases = [] {
    using namespace loom::msg;
    return std::array{
        receive_flag_case{recv_flag::none, 0, true, true, false},
        receive_flag_case{recv_flag::peek, FI_PEEK, false, true, false},
        receive_flag_case{recv_flag::claim, FI_CLAIM, false, true, true},
        receive_flag_case{recv_flag::discard, FI_DISCARD, false, false, false},
        receive_flag_case{
            recv_flag::peek | recv_flag::claim, FI_PEEK | FI_CLAIM, false, true, true},
        receive_flag_case{
            recv_flag::peek | recv_flag::discard, FI_PEEK | FI_DISCARD, false, true, false},
        receive_flag_case{
            recv_flag::claim | recv_flag::discard, FI_CLAIM | FI_DISCARD, false, true, true},
        receive_flag_case{recv_flag::peek | recv_flag::claim | recv_flag::discard,
                          FI_PEEK | FI_CLAIM | FI_DISCARD,
                          false,
                          true,
                          true}};
}();

enum class unavailable_operation { endpoint, table, entry, layout };
}  // namespace

TEST_CASE("message APIs reject unavailable native operations", "[message][submission]") {
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
    std::array<std::byte, 8> bytes{};
    const fabric_addr peer{19};
    const std::array iov{iovec{bytes.data(), bytes.size()}};
    const std::array<mr_descriptor, 0> descriptors{};
    const std::array results{
        msg::sendmsg(ep, msg::send_message<>{}),
        msg::recvmsg(ep, msg::recv_message<>{}),
        msg::tagged_sendmsg(ep, msg::tagged_send_message<>{}),
        msg::tagged_recvmsg(ep, msg::tagged_recv_message<>{}),
        msg::send_to(ep, bytes, peer),
        msg::recv_from(ep, bytes, peer),
        msg::send_data_to(ep, bytes, 7, peer),
        msg::post_multi_recv(ep, bytes),
        loom::send(ep, bytes, peer),
        loom::send(ep, bytes, peer, mr_descriptor{}, op_flag::completion),
        loom::recv(ep, bytes, peer),
        loom::recv(ep, bytes, peer, mr_descriptor{}, op_flag::completion),
        tagged_send(ep, bytes, peer, 42),
        tagged_send(ep, bytes, peer, 42, mr_descriptor{}, op_flag::completion),
        tagged_recv(ep, bytes, peer, 42),
        tagged_recv(ep, bytes, peer, 42, 0, mr_descriptor{}, op_flag::completion),
        sendv(ep, iov, descriptors, peer),
        recvv(ep, iov, descriptors, peer),
        tagged_sendv(ep, iov, descriptors, peer, 42),
        tagged_recvv(ep, iov, descriptors, peer, 42),
        send_data(ep, bytes, peer, 7),
        tagged_send_data(ep, bytes, peer, 42, 7),
    };
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == (unavailable == unavailable_operation::endpoint
                                       ? errc::invalid_argument
                                       : errc::not_supported));
    }
    REQUIRE(native.calls == 0);
}

TEST_CASE("UDP rejects unsupported tagged vector submissions", "[message][integration]") {
    using namespace loom;
    fabric_hints hints{};
    hints.provider_name = "udp";
    hints.ep_type = endpoint_types::dgram;
    hints.capabilities = capability::msg;
    auto info = query_fabric(hints);
    if (!info) {
        SKIP("UDP provider unavailable");
    }
    auto fabric = fabric::create(*info).value();
    auto domain = domain::create(fabric, *info).value();
    auto ep = endpoint::create(domain, *info).value();
    std::array<std::byte, 8> bytes{};
    const std::array iov{iovec{bytes.data(), bytes.size()}};
    const std::array<mr_descriptor, 0> descriptors{};
    CHECK(ep.tagged_send(bytes, 42).error() == errc::not_supported);
    CHECK(tagged_sendv(ep, iov, descriptors, fabric_addrs::unspecified, 42).error() ==
          errc::not_supported);
    CHECK(tagged_recvv(ep, iov, descriptors, fabric_addrs::unspecified, 42).error() ==
          errc::not_supported);
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
    const auto iov_count = GENERATE(1UZ, 16UZ, 17UZ, 64UZ);
    const auto count = GENERATE_COPY(0UZ, 1UZ, iov_count, iov_count + 1);
    CAPTURE(operation, iov_count, count);
    message_provider native;
    auto ep = native.endpoint();
    auto counter = native.counter();
    context trigger_context;
    const loom::trigger::threshold_condition trigger{&counter, 1};
    std::array<std::byte, 65> storage{};
    std::array<iovec, 64> iov_storage{};
    const auto iov = std::span{iov_storage}.first(iov_count);
    std::vector<loom::mr_descriptor> descriptors;
    std::vector<void*> expected;
    for (std::size_t i = 0; i < iov.size(); ++i) {
        iov[i] = {storage.data() + i, 1};
    }
    for (std::size_t i = 0; i < count; ++i) {
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
    if (count == 0 || count == iov_count) {
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

TEST_CASE("receive messages validate effective probe flags", "[message][probe][validation]") {
    using namespace loom;
    const auto entry = GENERATE(Catch::Generators::from_range(receive_flag_cases));
    const auto tagged = GENERATE(false, true);
    const auto with_context = GENERATE(false, true);
    const auto from_defaults = GENERATE(false, true);
    const auto policy = GENERATE(operation_defaults::replace,
                                 operation_defaults::inherit,
                                 operation_defaults::inherit_without_more);
    const auto shared = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN);
    CAPTURE(entry.native, tagged, with_context, from_defaults, policy, shared, status);
    message_provider native;
    native.status = status;
    native.rx_defaults = FI_MORE | (from_defaults ? entry.native : 0);
    fi_context2 context{};
    const auto selected_flags =
        (from_defaults ? msg::recv_flag::none : entry.flags) | msg::recv_flag::completion;
    const bool inherited = policy != operation_defaults::replace;
    const bool probe_applies = !from_defaults || inherited;
    const bool valid =
        !probe_applies || (tagged ? entry.tagged_valid && (!entry.needs_context || with_context)
                                  : entry.untagged_valid);
    auto submit = [&]<operation_defaults Defaults>(auto& ep) {
        if (tagged)
            return msg::tagged_recvmsg<Defaults>(
                ep,
                msg::tagged_recv_message<>{.context = with_context ? &context : nullptr},
                selected_flags);
        return msg::recvmsg<Defaults>(
            ep, msg::recv_message<>{.context = with_context ? &context : nullptr}, selected_flags);
    };
    auto run = [&](auto& ep) {
        switch (policy) {
            case operation_defaults::replace:
                return submit.template operator()<operation_defaults::replace>(ep);
            case operation_defaults::inherit:
                return submit.template operator()<operation_defaults::inherit>(ep);
            default:
                return submit.template operator()<operation_defaults::inherit_without_more>(ep);
        }
    };
    const auto result = [&] {
        if (shared) {
            shared_rx_context ep{detail::make_pmr_unique<detail::shared_rx_context_impl>(
                get_default_resource(), &native.handle, native.rx_defaults)};
            return run(ep);
        }
        auto ep = native.endpoint();
        return run(ep);
    }();
    CHECK(native.calls == (valid ? 1 : 0));
    if (!valid) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == errc::invalid_argument);
    } else {
        CHECK(native.flags == (FI_COMPLETION | (probe_applies ? entry.native : 0) |
                               (policy == operation_defaults::inherit ? FI_MORE : 0)));
        CHECK(native.operation_context == (with_context ? &context : nullptr));
        if (status) {
            REQUIRE_FALSE(result);
            CHECK(result.error() == make_error_code_from_fi_errno(status));
        } else {
            CHECK(result);
        }
    }
}

TEST_CASE("tagged discard can inherit its peek flag", "[message][probe][validation]") {
    using namespace loom;
    message_provider native;
    native.rx_defaults = FI_PEEK;
    auto ep = native.endpoint();
    REQUIRE(msg::tagged_recvmsg<operation_defaults::inherit>(
        ep, msg::tagged_recv_message<>{}, msg::recv_flag::discard));
    CHECK(native.calls == 1);
    CHECK(native.flags == (FI_PEEK | FI_DISCARD));
}

TEST_CASE("triggered receives validate probe flags", "[message][trigger][validation]") {
    using namespace loom;
    const auto entry = GENERATE(Catch::Generators::from_range(receive_flag_cases));
    const auto tagged = GENERATE(false, true);
    const auto deferred = GENERATE(false, true);
    const auto with_context = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN);
    CAPTURE(entry.native, tagged, deferred, with_context, status);
    message_provider native;
    native.status = status;
    auto ep = native.endpoint();
    auto domain = native.domain();
    auto counter = native.counter();
    const trigger::threshold_condition condition{&counter, 7};
    context trigger_context{};
    fi_context2 deferred_context{};
    auto* provided_context = with_context ? &deferred_context : nullptr;
    // Deferred work supplies its embedded fi_context2 when none is provided.
    const bool valid =
        (tagged ? entry.tagged_valid : entry.untagged_valid) && (deferred || with_context);
    const auto result = [&]() -> void_result {
        if (deferred) {
            auto work = tagged ? trigger::deferred_work::create_tagged_recv(
                                     domain,
                                     ep,
                                     condition,
                                     nullptr,
                                     msg::tagged_recv_message<>{.context = provided_context},
                                     entry.flags)
                               : trigger::deferred_work::create_recv(
                                     domain,
                                     ep,
                                     condition,
                                     nullptr,
                                     msg::recv_message<>{.context = provided_context},
                                     entry.flags);
            if (!work)
                return std::unexpected{work.error()};
            CHECK(native.operation_context ==
                  (with_context ? provided_context : work->internal_ptr()));
            work->mark_completed();
            return make_success();
        }
        if (tagged)
            return trigger::tagged_recvmsg(
                ep,
                msg::tagged_recv_message<context*>{.context =
                                                       with_context ? &trigger_context : nullptr},
                condition,
                entry.flags);
        return trigger::recvmsg(
            ep,
            msg::recv_message<context*>{.context = with_context ? &trigger_context : nullptr},
            condition,
            entry.flags);
    }();
    CHECK(native.calls == (valid ? 1 : 0));
    if (!valid) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == errc::invalid_argument);
    } else {
        CHECK(native.flags == (entry.native | (deferred ? 0 : FI_TRIGGER)));
        if (!deferred)
            CHECK(native.operation_context == trigger_context.raw());
        if (status) {
            REQUIRE_FALSE(result);
            CHECK(result.error() == make_error_code_from_fi_errno(status));
        } else {
            CHECK(result);
        }
    }
}

TEST_CASE("tagged probe flags and context reach the provider", "[message][probe]") {
    using namespace loom;
    struct flag_case {
        msg::recv_flags flags;
        std::uint64_t native;
    };
    const auto [flags, expected] =
        GENERATE(flag_case{msg::recv_flag::peek, FI_PEEK},
                 flag_case{msg::recv_flag::peek | msg::recv_flag::claim, FI_PEEK | FI_CLAIM},
                 flag_case{msg::recv_flag::claim, FI_CLAIM},
                 flag_case{msg::recv_flag::claim | msg::recv_flag::discard, FI_CLAIM | FI_DISCARD},
                 flag_case{msg::recv_flag::peek | msg::recv_flag::discard, FI_PEEK | FI_DISCARD});
    const auto inherit = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EOPNOTSUPP);
    message_provider native;
    native.rx_defaults = FI_COMPLETION;
    native.status = status;
    auto ep = native.endpoint();
    fi_context2 context{};
    const msg::tagged_recv_message<> message{
        .src_addr = fabric_addr{19}, .tag = 42, .ignore = 0x80, .context = &context};
    const auto result = inherit
                            ? msg::tagged_recvmsg<operation_defaults::inherit>(ep, message, flags)
                            : msg::tagged_recvmsg(ep, message, flags);
    REQUIRE(native.calls == 1);
    REQUIRE(native.flags == (expected | (inherit ? FI_COMPLETION : 0)));
    REQUIRE(native.peer == 19);
    REQUIRE(native.tag == 42);
    REQUIRE(native.ignore == 0x80);
    REQUIRE(native.operation_context == &context);
    if (status) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == make_error_code_from_fi_errno(status));
    } else {
        REQUIRE(result);
    }
}

TEST_CASE("tagged probes reserve receive and discard messages",
          "[message][probe][claim][loopback]") {
    using namespace loom;
    enum class disposition { receive, truncate, empty, discard_claim, discard_peek };
    const auto action = GENERATE(disposition::receive,
                                 disposition::truncate,
                                 disposition::empty,
                                 disposition::discard_claim,
                                 disposition::discard_peek);
    const auto provider = GENERATE("sockets", "tcp;ofi_rxm");
    const auto with_source = GENERATE(false, true);
    const auto with_data = GENERATE(false, true);
    const auto typed = GENERATE(false, true);
    CAPTURE(provider, action, with_source, with_data, typed);
    constexpr std::uint64_t sent_tag = 0xaa, match_tag = 0x2a, ignored_bits = 0x80;
    fabric_hints hints{};
    hints.provider_name = provider;
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::tagged | capability::source;
    auto info = query_fabric(hints);
    if (!info) {
        SKIP("Tagged software provider unavailable");
    }
    // Storage precedes the endpoint so a failed assertion closes it first.
    std::array<std::byte, 8> outgoing{}, incoming{};
    outgoing.fill(std::byte{42});
    fi_context2 send_context{}, probe_context{}, claim_context{};
    auto fab = fabric::create(*info);
    REQUIRE(fab);
    auto dom = domain::create(*fab, *info);
    REQUIRE(dom);
    auto cq = completion_queue::create(*dom, {.source_addresses = with_source});
    REQUIRE(cq);
    auto av = address_vector::create(*dom);
    REQUIRE(av);
    auto send_mr = memory_region::register_memory(*dom, outgoing, mr_access_flags::send);
    REQUIRE(send_mr);
    auto recv_mr = memory_region::register_memory(*dom, incoming, mr_access_flags::recv);
    REQUIRE(recv_mr);
    const auto sent_size = action == disposition::empty ? 0UZ : outgoing.size();
    const auto received_size = action == disposition::truncate ? 4UZ : sent_size;
    const auto received_buffer = std::span{incoming}.first(received_size);
    const std::array send_iov{iovec{outgoing.data(), sent_size}};
    const std::array recv_iov{iovec{incoming.data(), received_size}};
    const std::array send_desc{send_mr->descriptor()}, recv_desc{recv_mr->descriptor()};
    auto other_ep = endpoint::create(*dom, *info);
    REQUIRE(other_ep);
    REQUIRE(other_ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv));
    REQUIRE(other_ep->bind_av(*av));
    REQUIRE(other_ep->enable());
    auto other_address = other_ep->get_local_address();
    REQUIRE(other_address);
    auto other_peer = av->insert(*other_address);
    REQUIRE(other_peer);
    auto ep = endpoint::create(*dom, *info);
    REQUIRE(ep);
    REQUIRE(ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
    REQUIRE(ep->bind_av(*av));
    REQUIRE(ep->enable());
    auto address = ep->get_local_address();
    REQUIRE(address);
    auto peer = av->insert(*address);
    REQUIRE(peer);
    REQUIRE(peer->value != other_peer->value);

    msg::tagged_claim reservation{*ep};
    struct close_on_exit {
        endpoint& ep;
        ~close_on_exit() { (void)ep.close(); }
    } close_guard{*ep};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    auto post = [&](auto submit) {
        auto result = submit();
        while (!result && result.error() == errc::again &&
               std::chrono::steady_clock::now() < deadline) {
            (void)cq->progress();
            std::this_thread::yield();
            result = submit();
        }
        REQUIRE(result);
    };
    auto completion = [&](void* context) {
        std::optional<completion_event> event;
        do {
            event = cq->poll();
            if (event) {
                break;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(event);
        REQUIRE(event->context.get() == context);
        return *event;
    };
    auto peek = [&](fi_context2* context, msg::recv_flags flags) {
        post([&] {
            return msg::tagged_recvmsg(*ep,
                                       msg::tagged_recv_message<>{.tag = match_tag,
                                                                  .ignore = ignored_bits,
                                                                  .context = context},
                                       flags | msg::recv_flag::peek | msg::recv_flag::completion);
        });
        return completion(context);
    };

    REQUIRE(peek(&probe_context, msg::recv_flag::none).error == errc::no_message);
    if (typed) {
        post([&] { return reservation.probe(match_tag, ignored_bits); });
        REQUIRE(reservation.observe(completion(reservation.context().get())) ==
                msg::tagged_claim_result::no_match);
    }
    post([&] {
        return msg::tagged_sendmsg(
            *ep,
            msg::tagged_send_message<>{.iov = send_iov,
                                       .desc = send_desc,
                                       .dest_addr = fabric_addr{peer->value},
                                       .tag = sent_tag,
                                       .context = &send_context,
                                       .data = 73},
            msg::send_flag::completion |
                (with_data ? msg::send_flag::remote_cq_data : msg::send_flag::none));
    });
    REQUIRE_FALSE(completion(&send_context).has_error());
    // Send completion need not imply arrival in the unexpected-message queue.
    auto found = peek(&probe_context, msg::recv_flag::none);
    while (found.error == errc::no_message && std::chrono::steady_clock::now() < deadline) {
        found = peek(&probe_context, msg::recv_flag::none);
    }
    REQUIRE_FALSE(found.has_error());
    REQUIRE(found.tag == sent_tag);
    REQUIRE(found.bytes_transferred == sent_size);
    REQUIRE(found.has_immediate_data() == with_data);
    if (with_data) {
        REQUIRE(found.data == 73);
    }
    if (with_source) {
        REQUIRE(found.source.get() == peer->value);
    }

    if (action == disposition::discard_peek) {
        const auto needs_context =
            info->get_mode().has(mode_bits::context) || info->get_mode().has(mode_bits::context2);
        REQUIRE_FALSE(
            peek(needs_context ? &probe_context : nullptr, msg::recv_flag::discard).has_error());
    } else {
        const auto claimed = [&] {
            if (typed) {
                post([&] { return reservation.probe(match_tag, ignored_bits); });
                auto event = completion(reservation.context().get());
                REQUIRE(reservation.observe(event) == msg::tagged_claim_result::reserved);
                return event;
            }
            return peek(&claim_context, msg::recv_flag::claim);
        }();
        REQUIRE_FALSE(claimed.has_error());
        REQUIRE(claimed.tag == sent_tag);
        REQUIRE(claimed.bytes_transferred == sent_size);
        REQUIRE(claimed.has_immediate_data() == with_data);
        if (with_data) {
            REQUIRE(claimed.data == 73);
        }
        if (with_source) {
            REQUIRE(claimed.source.get() == peer->value);
        }
        // An unrelated probe cannot steal the reserved message.
        REQUIRE(peek(&probe_context, msg::recv_flag::none).error == errc::no_message);
        post([&] {
            const auto discard = action == disposition::discard_claim;
            if (typed) {
                return discard ? reservation.discard()
                               : reservation.receive(received_buffer, recv_mr->descriptor());
            }
            return msg::tagged_recvmsg(
                *ep,
                msg::tagged_recv_message<>{.iov = discard ? std::span<const iovec>{} : recv_iov,
                                           .desc = discard ? std::span<const mr_descriptor>{}
                                                           : recv_desc,
                                           .tag = match_tag,
                                           .ignore = ignored_bits,
                                           .context = &claim_context},
                msg::recv_flag::claim | msg::recv_flag::completion |
                    (discard ? msg::recv_flag::discard : msg::recv_flag::none));
        });
        const auto consumed = completion(typed ? reservation.context().get() : &claim_context);
        if (action == disposition::truncate) {
            REQUIRE(consumed.error == make_error_code_from_fi_errno(FI_ETRUNC));
            if (typed) {
                REQUIRE(reservation.observe(consumed).error() == consumed.error);
            }
        } else {
            REQUIRE_FALSE(consumed.has_error());
            if (typed) {
                REQUIRE(reservation.observe(consumed) ==
                        (action == disposition::discard_claim
                             ? msg::tagged_claim_result::discarded
                             : msg::tagged_claim_result::received));
            }
        }
        if (typed) {
            REQUIRE(reservation.state() == msg::tagged_claim_state::idle);
        }
        if (action != disposition::discard_claim && action != disposition::truncate) {
            REQUIRE(consumed.bytes_transferred == received_size);
            REQUIRE(consumed.has_immediate_data() == with_data);
            if (with_data) {
                REQUIRE(consumed.data == 73);
            }
            if (with_source) {
                REQUIRE(consumed.source.get() == peer->value);
            }
        }
        for (std::size_t i = 0; i < incoming.size(); ++i) {
            REQUIRE(incoming[i] == (action != disposition::discard_claim && i < received_size
                                        ? outgoing[i]
                                        : std::byte{}));
        }
    }
    REQUIRE(peek(&probe_context, msg::recv_flag::none).error == errc::no_message);
    REQUIRE(ep->close());
    REQUIRE(other_ep->close());
}

TEST_CASE("selective message completions accompany sockets transfers", "[message][loopback]") {
    using namespace loom;
    const auto tagged = GENERATE(false, true);
    const auto progress = GENERATE(
        progress_mode::auto_progress, progress_mode::manual, progress_mode::control_unified);
    CAPTURE(tagged, progress);
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::tagged;
    hints.threading = threading_mode::domain;
    hints.control_progress = progress;
    hints.data_progress = progress;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    REQUIRE(info->get_domain_attr()->control_progress == progress);
    REQUIRE(info->get_domain_attr()->data_progress == progress);
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
