// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/messaging.hpp>
#include <loom/core/shared_context.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/shared_context_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <chrono>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
template <typename T>
concept scalar_receiver = requires(T& target, std::span<std::byte> buffer) {
    loom::recv(target, buffer, loom::fabric_addrs::unspecified);
    loom::tagged_recv(target, buffer, loom::fabric_addrs::unspecified, 7);
    loom::msg::recv_from(target, buffer, loom::fabric_addrs::unspecified);
};
template <typename T>
concept vector_receiver = requires(
    T& target, std::span<const iovec> buffers, std::span<const loom::mr_descriptor> descriptors) {
    loom::recvv(target, buffers, descriptors, loom::fabric_addrs::unspecified);
    loom::tagged_recvv(target, buffers, descriptors, loom::fabric_addrs::unspecified, 7);
};
template <typename T>
concept message_receiver = requires(
    T& target, loom::msg::recv_message<> message, loom::msg::tagged_recv_message<> tagged) {
    loom::msg::recvmsg(target, message);
    loom::msg::tagged_recvmsg(target, tagged);
};
template <typename T>
concept multi_receiver = requires(T& target, std::span<std::byte> buffer) {
    loom::msg::post_multi_recv(target, buffer);
};
}  // namespace

TEST_CASE("receive helpers accept shared receive contexts", "[shared_receive]") {
    CHECK(scalar_receiver<loom::shared_rx_context>);
    CHECK(vector_receiver<loom::shared_rx_context>);
    CHECK(message_receiver<loom::shared_rx_context>);
    CHECK(multi_receiver<loom::shared_rx_context>);
    CHECK(scalar_receiver<loom::endpoint>);
    CHECK(vector_receiver<loom::endpoint>);
    CHECK(message_receiver<loom::endpoint>);
    CHECK(multi_receiver<loom::endpoint>);
    CHECK_FALSE(scalar_receiver<loom::shared_tx_context>);
    CHECK_FALSE(scalar_receiver<loom::domain>);
    CHECK_FALSE(scalar_receiver<const loom::shared_rx_context>);
}

namespace {
struct shared_provider {
    int opened{}, closed{}, submitted{}, cancelled{};
    int open_error{}, submit_error{}, close_error{};
    std::uint64_t flags{}, tag{}, ignore{};
    fi_addr_t peer{};
    void* context{};
    std::vector<iovec> buffers;
    std::vector<void*> descriptors;
    static constexpr std::uint64_t defaults = FI_COMPLETION | FI_MORE;
    fi_rx_attr attributes{.op_flags = defaults};

    static auto owner(fid* handle) -> shared_provider& {
        return *static_cast<shared_provider*>(handle->context);
    }
    static auto receive(fid_ep* handle,
                        const iovec* buffers,
                        void** descriptors,
                        std::size_t count,
                        fi_addr_t peer,
                        void* context,
                        std::uint64_t flags = 0,
                        std::uint64_t tag = 0,
                        std::uint64_t ignore = 0) -> ssize_t {
        auto& self = owner(&handle->fid);
        CHECK(handle->fid.fclass == FI_CLASS_SRX_CTX);
        ++self.submitted;
        self.buffers.assign(buffers, buffers + count);
        self.descriptors.clear();
        if (descriptors)
            self.descriptors.assign(descriptors, descriptors + count);
        self.peer = peer;
        self.context = context;
        self.flags = flags;
        self.tag = tag;
        self.ignore = ignore;
        return self.submit_error;
    }
    fi_ops lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = owner(handle);
                if (handle->fclass == FI_CLASS_SRX_CTX) {
                    if (self.close_error)
                        return self.close_error;
                    ++self.closed;
                }
                return 0;
            },
    };
    fi_ops_ep operations{
        .size = sizeof(fi_ops_ep),
        .cancel = [](fid* handle, void* context) -> ssize_t {
            auto& self = owner(handle);
            ++self.cancelled;
            self.context = context;
            return self.submit_error;
        },
    };
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recv = [](fid_ep* handle,
                   void* buffer,
                   std::size_t size,
                   void* descriptor,
                   fi_addr_t peer,
                   void* context) -> ssize_t {
            const iovec entry{buffer, size};
            return receive(handle, &entry, descriptor ? &descriptor : nullptr, 1, peer, context);
        },
        .recvv = [](fid_ep* handle,
                    const iovec* buffers,
                    void** descriptors,
                    std::size_t count,
                    fi_addr_t peer,
                    void* context) -> ssize_t {
            return receive(handle, buffers, descriptors, count, peer, context);
        },
        .recvmsg = [](fid_ep* handle, const fi_msg* message, std::uint64_t flags) -> ssize_t {
            return receive(handle,
                           message->msg_iov,
                           message->desc,
                           message->iov_count,
                           message->addr,
                           message->context,
                           flags);
        },
    };
    fi_ops_tagged tagged{
        .size = sizeof(fi_ops_tagged),
        .recv = [](fid_ep* handle,
                   void* buffer,
                   std::size_t size,
                   void* descriptor,
                   fi_addr_t peer,
                   std::uint64_t tag,
                   std::uint64_t ignore,
                   void* context) -> ssize_t {
            const iovec entry{buffer, size};
            return receive(handle,
                           &entry,
                           descriptor ? &descriptor : nullptr,
                           1,
                           peer,
                           context,
                           0,
                           tag,
                           ignore);
        },
        .recvv = [](fid_ep* handle,
                    const iovec* buffers,
                    void** descriptors,
                    std::size_t count,
                    fi_addr_t peer,
                    std::uint64_t tag,
                    std::uint64_t ignore,
                    void* context) -> ssize_t {
            return receive(handle, buffers, descriptors, count, peer, context, 0, tag, ignore);
        },
        .recvmsg =
            [](fid_ep* handle, const fi_msg_tagged* message, std::uint64_t flags) -> ssize_t {
            return receive(handle,
                           message->msg_iov,
                           message->desc,
                           message->iov_count,
                           message->addr,
                           message->context,
                           flags,
                           message->tag,
                           message->ignore);
        },
    };
    fid_ep native{.fid = {.fclass = FI_CLASS_SRX_CTX, .context = this, .ops = &lifetime},
                  .ops = &operations,
                  .msg = &messages,
                  .tagged = &tagged};
    fi_ops_domain domain_operations{
        .size = sizeof(fi_ops_domain),
        .srx_ctx =
            [](fid_domain* domain, fi_rx_attr* attributes, fid_ep** result, void*) {
                auto& self = owner(&domain->fid);
                ++self.opened;
                if (self.open_error)
                    return self.open_error;
                CHECK(attributes);
                if (attributes)
                    CHECK(attributes->op_flags == defaults);
                *result = &self.native;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.fclass = FI_CLASS_DOMAIN, .context = this, .ops = &lifetime},
                             .ops = &domain_operations};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native_domain)};
    loom::fabric_info info =
        loom::fabric_info::copy_native(fi_info{.rx_attr = &attributes}).value();

    auto create() -> loom::shared_rx_context {
        return loom::shared_rx_context::create(domain, info).value();
    }
};

enum class receive_form {
    scalar,
    scalar_flags,
    from,
    vector,
    message,
    inherit,
    without_more,
    tagged_scalar,
    tagged_flags,
    tagged_vector,
    tagged_message,
    tagged_inherit,
    tagged_without_more,
    multi
};
enum class native_support { available, no_table, short_table, no_entry };
}  // namespace

TEST_CASE("shared receives preserve native arguments and operation support", "[shared_receive]") {
    using namespace loom;
    const auto form = GENERATE(receive_form::scalar,
                               receive_form::scalar_flags,
                               receive_form::from,
                               receive_form::vector,
                               receive_form::message,
                               receive_form::inherit,
                               receive_form::without_more,
                               receive_form::tagged_scalar,
                               receive_form::tagged_flags,
                               receive_form::tagged_vector,
                               receive_form::tagged_message,
                               receive_form::tagged_inherit,
                               receive_form::tagged_without_more,
                               receive_form::multi);
    const auto support = GENERATE(native_support::available,
                                  native_support::no_table,
                                  native_support::short_table,
                                  native_support::no_entry);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    shared_provider provider;
    auto receiver = provider.create();
    CHECK(receiver.impl_operation_flags(completion_direction::receive) ==
          shared_provider::defaults);
    CHECK(receiver.impl_operation_flags(completion_direction::transmit) == 0);
    provider.submit_error = status;
    if (support == native_support::no_table) {
        provider.native.msg = nullptr;
        provider.native.tagged = nullptr;
    } else if (support == native_support::short_table) {
        provider.messages.size = provider.tagged.size = 0;
    } else if (support == native_support::no_entry) {
        provider.messages = {.size = sizeof(fi_ops_msg)};
        provider.tagged = {.size = sizeof(fi_ops_tagged)};
    }
    std::array<std::byte, 32> bytes{};
    std::array buffers{iovec{bytes.data(), 11}, iovec{bytes.data() + 11, 21}};
    int first_descriptor{}, second_descriptor{};
    std::array descriptors{mr_descriptor{&first_descriptor}, mr_descriptor{&second_descriptor}};
    fi_context2 storage{};
    const fabric_addr peer{0xfedcba9876543210};
    constexpr std::uint64_t tag = 0xabcdef0123456789, ignore = 0x1100000011;
    const msg::recv_message message{
        .iov = buffers, .desc = descriptors, .src_addr = peer, .context = &storage};
    const msg::tagged_recv_message tagged{.iov = buffers,
                                          .desc = descriptors,
                                          .src_addr = peer,
                                          .tag = tag,
                                          .ignore = ignore,
                                          .context = &storage};
    auto submit = [&](auto& target) -> void_result {
        switch (form) {
            case receive_form::scalar:
                return recv(target, bytes, peer, descriptors[0], &storage);
            case receive_form::scalar_flags:
                return recv(target, bytes, peer, descriptors[0], op_flag::completion, &storage);
            case receive_form::from:
                return msg::recv_from(target, bytes, peer, &storage);
            case receive_form::vector:
                return recvv(target, buffers, descriptors, peer, &storage);
            case receive_form::message:
                return msg::recvmsg(target, message, msg::recv_flag::completion);
            case receive_form::inherit:
                return msg::recvmsg<operation_defaults::inherit>(target, message);
            case receive_form::without_more:
                return msg::recvmsg<operation_defaults::inherit_without_more>(target, message);
            case receive_form::tagged_scalar:
                return tagged_recv(target, bytes, peer, tag, ignore, descriptors[0], &storage);
            case receive_form::tagged_flags:
                return tagged_recv(target,
                                   bytes,
                                   peer,
                                   tag,
                                   ignore,
                                   descriptors[0],
                                   op_flag::completion,
                                   &storage);
            case receive_form::tagged_vector:
                return tagged_recvv(target, buffers, descriptors, peer, tag, ignore, &storage);
            case receive_form::tagged_message:
                return msg::tagged_recvmsg(
                    target, tagged, msg::recv_flag::peek | msg::recv_flag::claim);
            case receive_form::tagged_inherit:
                return msg::tagged_recvmsg<operation_defaults::inherit>(target, tagged);
            case receive_form::tagged_without_more:
                return msg::tagged_recvmsg<operation_defaults::inherit_without_more>(target,
                                                                                     tagged);
            case receive_form::multi:
                return msg::post_multi_recv(target, bytes, &storage);
        }
        std::unreachable();
    };
    auto moved = std::move(receiver);
    const auto empty = submit(receiver);
    REQUIRE_FALSE(empty);
    CHECK(empty.error() == errc::invalid_argument);
    CHECK(provider.submitted == 0);
    const auto result = submit(moved);
    if (support != native_support::available) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == errc::not_supported);
        CHECK(provider.submitted == 0);
        return;
    }
    CHECK(result.has_value() == (status == 0));
    if (!result)
        CHECK(result.error() == make_error_code_from_fi_errno(-status));
    CHECK(provider.submitted == 1);
    CHECK(provider.context == &storage);
    CHECK(provider.peer == (form == receive_form::multi ? FI_ADDR_UNSPEC : peer.get()));
    const bool vector =
        form == receive_form::vector || form == receive_form::message ||
        form == receive_form::inherit || form == receive_form::without_more ||
        form == receive_form::tagged_vector || form == receive_form::tagged_message ||
        form == receive_form::tagged_inherit || form == receive_form::tagged_without_more;
    REQUIRE(provider.buffers.size() == (vector ? 2 : 1));
    CHECK(provider.buffers[0].iov_base == bytes.data());
    CHECK(provider.buffers[0].iov_len == (vector ? 11 : bytes.size()));
    if (vector) {
        CHECK(provider.buffers[1].iov_base == bytes.data() + 11);
        CHECK(provider.buffers[1].iov_len == 21);
        CHECK(provider.descriptors == std::vector<void*>{&first_descriptor, &second_descriptor});
    } else if (form == receive_form::from || form == receive_form::multi) {
        CHECK(provider.descriptors.empty());
    } else {
        CHECK(provider.descriptors == std::vector<void*>{&first_descriptor});
    }
    const bool is_tagged =
        form >= receive_form::tagged_scalar && form <= receive_form::tagged_without_more;
    CHECK(provider.tag == (is_tagged ? tag : 0));
    CHECK(provider.ignore == (is_tagged ? ignore : 0));
    std::uint64_t expected_flags{};
    switch (form) {
        case receive_form::scalar_flags:
        case receive_form::message:
        case receive_form::without_more:
        case receive_form::tagged_flags:
        case receive_form::tagged_without_more:
            expected_flags = FI_COMPLETION;
            break;
        case receive_form::inherit:
        case receive_form::tagged_inherit:
            expected_flags = shared_provider::defaults;
            break;
        case receive_form::tagged_message:
            expected_flags = FI_PEEK | FI_CLAIM;
            break;
        case receive_form::multi:
            expected_flags = FI_MULTI_RECV;
            break;
        default:
            break;
    }
    CHECK(provider.flags == expected_flags);
}

TEST_CASE("shared receive lifecycle preserves failures for retry", "[shared_receive]") {
    using namespace loom;
    shared_provider provider;
    auto receiver = provider.create();
    fi_context2 storage{};
    provider.native.fid.ops = nullptr;
    CHECK(receiver.close().error() == errc::not_supported);
    provider.native.fid.ops = &provider.lifetime;
    provider.lifetime.size = 0;
    CHECK(receiver.close().error() == errc::not_supported);
    provider.lifetime.size = sizeof(fi_ops);
    CHECK(receiver.cancel({}).error() == errc::invalid_argument);
    CHECK(provider.cancelled == 0);
    provider.submit_error = -FI_EAGAIN;
    CHECK(receiver.cancel(context_ptr<void>{&storage}).error() == errc::again);
    CHECK(provider.context == &storage);
    provider.submit_error = 0;
    REQUIRE(receiver.cancel(context_ptr<void>{&storage}));
    CHECK(provider.cancelled == 2);
    CHECK(receiver);
    provider.native.ops = nullptr;
    CHECK(receiver.cancel(context_ptr<void>{&storage}).error() == errc::not_supported);
    provider.native.ops = &provider.operations;
    provider.operations.size = 0;
    CHECK(receiver.cancel(context_ptr<void>{&storage}).error() == errc::not_supported);
    provider.operations.size = sizeof(fi_ops_ep);
    provider.operations.cancel = nullptr;
    CHECK(receiver.cancel(context_ptr<void>{&storage}).error() == errc::not_supported);
    CHECK(provider.cancelled == 2);
    provider.close_error = -FI_EBUSY;
    CHECK(receiver.close().error() == errc::busy);
    CHECK(receiver);
    CHECK(provider.closed == 0);
    provider.close_error = 0;
    REQUIRE(receiver.close());
    CHECK_FALSE(receiver);
    CHECK(receiver.impl_operation_flags(completion_direction::receive) == 0);
    CHECK(receiver.cancel(context_ptr<void>{&storage}).error() == errc::invalid_argument);
    REQUIRE(receiver.close());
    CHECK(provider.closed == 1);
}

TEST_CASE("shared receive creation validates native support", "[shared_receive]") {
    using namespace loom;
    shared_provider provider;
    domain empty_domain;
    fabric_info empty_info;
    CHECK(shared_rx_context::create(empty_domain, provider.info).error() == errc::invalid_argument);
    CHECK(shared_rx_context::create(provider.domain, empty_info).error() == errc::invalid_argument);
    provider.open_error = -FI_ENOMEM;
    CHECK(shared_rx_context::create(provider.domain, provider.info).error() == errc::no_memory);
    CHECK(provider.closed == 0);
    CHECK(provider.opened == 1);
    provider.native_domain.ops = nullptr;
    CHECK(shared_rx_context::create(provider.domain, provider.info).error() == errc::not_supported);
    provider.native_domain.ops = &provider.domain_operations;
    provider.domain_operations.size = 0;
    CHECK(shared_rx_context::create(provider.domain, provider.info).error() == errc::not_supported);
    provider.domain_operations.size = sizeof(fi_ops_domain);
    provider.domain_operations.srx_ctx = nullptr;
    CHECK(shared_rx_context::create(provider.domain, provider.info).error() == errc::not_supported);
    CHECK(provider.opened == 1);
}

TEST_CASE("shared receives reject mismatched registration descriptors", "[shared_receive]") {
    using namespace loom;
    shared_provider provider;
    auto receiver = provider.create();
    std::array<std::byte, 8> bytes{};
    std::array buffers{iovec{bytes.data(), bytes.size()}};
    std::array<mr_descriptor, 2> descriptors{};
    const msg::recv_message<> message{.iov = buffers, .desc = descriptors};
    const msg::tagged_recv_message<> tagged{.iov = buffers, .desc = descriptors};
    CHECK(msg::recvmsg(receiver, message).error() == errc::invalid_argument);
    CHECK(msg::tagged_recvmsg(receiver, tagged).error() == errc::invalid_argument);
    CHECK(recvv(receiver, buffers, descriptors, fabric_addrs::unspecified).error() ==
          errc::invalid_argument);
    CHECK(tagged_recvv(receiver, buffers, descriptors, fabric_addrs::unspecified, 1).error() ==
          errc::invalid_argument);
    CHECK(provider.submitted == 0);
}

TEST_CASE("discovery distinguishes shared receive contexts from context counts",
          "[shared_receive]") {
    const auto count = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{2}, FI_SHARED_CONTEXT);
    fi_ep_attr endpoint{.tx_ctx_cnt = FI_SHARED_CONTEXT, .rx_ctx_cnt = count};
    const auto info = loom::fabric_info::copy_native(fi_info{.ep_attr = &endpoint}).value();
    REQUIRE(info.get_endpoint_attr());
    CHECK(info.get_endpoint_attr()->shared_receive_context == (count == FI_SHARED_CONTEXT));
    CHECK(info.get_endpoint_attr()->rx_ctx_cnt.get() == count);
}

TEST_CASE("sockets routes shared receives to each receiving endpoint queue",
          "[shared_receive][integration]") {
    using namespace loom;
    const auto tagged = GENERATE(false, true);
    const auto vectored = GENERATE(false, true);
    fabric_hints hints;
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::tagged;
    hints.shared_receive_context = true;
    const auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider with shared receives unavailable");
    REQUIRE(info->get_endpoint_attr()->shared_receive_context);
    auto fabric = fabric::create(*info).value();
    auto domain = domain::create(fabric, *info).value();
    std::array<std::array<std::byte, 24>, 2> source{}, target{};
    source[0].fill(std::byte{0x31});
    source[1].fill(std::byte{0x72});
    std::array<fi_context2, 2> receives{}, sends{};
    auto av = address_vector::create(domain).value();
    auto first_queue = completion_queue::create(domain).value();
    auto second_queue = completion_queue::create(domain).value();
    auto receiver = shared_rx_context::create(domain, *info).value();
    auto first = endpoint::create(domain, *info).value();
    auto second = endpoint::create(domain, *info).value();
    REQUIRE(first.bind_cq(first_queue, cq_bind::transmit | cq_bind::recv));
    REQUIRE(second.bind_cq(second_queue, cq_bind::transmit | cq_bind::recv));
    REQUIRE(first.bind_av(av));
    REQUIRE(second.bind_av(av));
    REQUIRE(first.bind_srx(receiver));
    REQUIRE(second.bind_srx(receiver));
    REQUIRE(first.enable());
    REQUIRE(second.enable());
    const auto first_peer = fabric_addr{av.insert(first.get_local_address().value()).value().value};
    const auto second_peer =
        fabric_addr{av.insert(second.get_local_address().value()).value().value};
    const std::array<mr_descriptor, 0> descriptors{};
    for (std::size_t i = 0; i < target.size(); ++i) {
        std::array buffers{iovec{target[i].data(), 7}, iovec{target[i].data() + 7, 17}};
        if (tagged) {
            if (vectored)
                REQUIRE(tagged_recvv(receiver,
                                     buffers,
                                     descriptors,
                                     fabric_addrs::unspecified,
                                     0x1234 + i,
                                     0,
                                     &receives[i]));
            else
                REQUIRE(tagged_recv(receiver,
                                    target[i],
                                    fabric_addrs::unspecified,
                                    0x1234 + i,
                                    0,
                                    {},
                                    &receives[i]));
        } else {
            if (vectored)
                REQUIRE(
                    recvv(receiver, buffers, descriptors, fabric_addrs::unspecified, &receives[i]));
            else
                REQUIRE(recv(receiver, target[i], fabric_addrs::unspecified, {}, &receives[i]));
        }
    }
    CHECK_FALSE(first_queue.poll());
    CHECK_FALSE(second_queue.poll());
    std::array<bool, 2> received{}, sent{};
    std::array<int, 2> queue_receives{};
    auto poll = [&](completion_queue& queue, std::size_t index) {
        if (auto event = queue.poll()) {
            REQUIRE_FALSE(event->error);
            bool matched{};
            for (std::size_t i = 0; i < 2; ++i) {
                if (event->context.get() == &receives[i]) {
                    CHECK_FALSE(received[i]);
                    CHECK(event->len == source[index].size());
                    CHECK(target[i] == source[index]);
                    received[i] = true;
                    ++queue_receives[index];
                    matched = true;
                } else if (event->context.get() == &sends[i]) {
                    CHECK_FALSE(sent[i]);
                    CHECK(index == 0);
                    sent[i] = true;
                    matched = true;
                }
            }
            REQUIRE(matched);
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    for (std::size_t i = 0; i < 2; ++i) {
        void_result result;
        do {
            const auto peer = i ? second_peer : first_peer;
            result = tagged ? tagged_send(first, source[i], peer, 0x1234 + i, {}, &sends[i])
                            : send(first, source[i], peer, {}, &sends[i]);
            if (result || result.error() != errc::again)
                break;
            poll(first_queue, 0);
            poll(second_queue, 1);
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(result);
    }
    while ((!received[0] || !received[1] || !sent[0] || !sent[1]) &&
           std::chrono::steady_clock::now() < deadline) {
        poll(first_queue, 0);
        poll(second_queue, 1);
        std::this_thread::yield();
    }
    CHECK(std::ranges::all_of(received, std::identity{}));
    CHECK(std::ranges::all_of(sent, std::identity{}));
    CHECK(queue_receives == std::array{1, 1});
    REQUIRE(first.close());
    REQUIRE(second.close());
    REQUIRE(receiver.close());
}
