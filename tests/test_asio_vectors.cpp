// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/asio.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>

#include <array>
#include <cstring>
#include <span>
#include <string>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct vector_provider {
    std::span<const iovec> buffers;
    void* context{};
    void* attempted_context{};
    std::size_t remote_length{};
    bool throw_on_submit{};
    fi_addr_t peer{};
    std::uint64_t remote_address{};
    std::uint64_t remote_key{};
    std::vector<void*> descriptors;
    int submission_status{};
    std::uint64_t submission_flags{};

    static auto capture_rma(fid_ep* ep, const fi_msg_rma* msg, std::uint64_t flags) -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        self.submission_flags = flags;
        REQUIRE(msg->rma_iov_count == 1);
        self.remote_length = msg->rma_iov[0].len;
        self.remote_address = msg->rma_iov[0].addr;
        self.remote_key = msg->rma_iov[0].key;
        self.peer = msg->addr;
        if (msg->desc)
            self.descriptors.assign(msg->desc, msg->desc + msg->iov_count);
        return capture(ep, msg->msg_iov, msg->iov_count, msg->context);
    }

    static auto capture_message(fid_ep* ep,
                                const auto* msg,
                                std::uint64_t flags,
                                loom::completion_direction direction) -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        self.submission_flags = flags;
        REQUIRE(msg->addr == FI_ADDR_UNSPEC);
        if (msg->desc)
            self.descriptors.assign(msg->desc, msg->desc + msg->iov_count);
        return capture(ep, msg->msg_iov, msg->iov_count, msg->context, direction);
    }

    static auto capture(fid_ep* ep,
                        const iovec* iov,
                        std::size_t count,
                        void* context,
                        loom::completion_direction direction = loom::completion_direction::transmit)
        -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        self.attempted_context = context;
        loom::completion_registration duplicate;
        auto registered =
            duplicate.bind(direction == loom::completion_direction::receive ? self.rx : self.tx,
                           loom::context_ptr<void>{context},
                           &self,
                           [](void*, const loom::completion_event&) {});
        REQUIRE_FALSE(registered);
        REQUIRE(registered.error() == loom::errc::address_in_use);
        if (self.throw_on_submit) {
            throw std::bad_alloc{};
        }
        if (self.submission_status) {
            return self.submission_status;
        }
        self.buffers = {iov, count};
        self.context = context;
        return 0;
    }

    fi_ops ops{.size = sizeof(fi_ops),
               .close = [](fid*) { return 0; },
               .bind = [](fid*, fid*, std::uint64_t) { return 0; }};
    fid_cq tx_handle{.fid = {.ops = &ops}}, rx_handle{.fid = {.ops = &ops}};
    loom::completion_queue tx{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &tx_handle)};
    loom::completion_queue rx{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &rx_handle)};
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvv =
            [](fid_ep* ep, const iovec* iov, void**, std::size_t count, fi_addr_t, void* context) {
                return capture(ep, iov, count, context, loom::completion_direction::receive);
            },
        .recvmsg =
            [](fid_ep* ep, const fi_msg* msg, std::uint64_t flags) {
                return capture_message(ep, msg, flags, loom::completion_direction::receive);
            },
        .sendv =
            [](fid_ep* ep, const iovec* iov, void**, std::size_t count, fi_addr_t, void* context) {
                return capture(ep, iov, count, context);
            },
        .sendmsg =
            [](fid_ep* ep, const fi_msg* msg, std::uint64_t flags) {
                return capture_message(ep, msg, flags, loom::completion_direction::transmit);
            },
    };
    fi_ops_tagged tags{
        .size = sizeof(fi_ops_tagged),
        .recvv =
            [](fid_ep* ep,
               const iovec* iov,
               void**,
               std::size_t count,
               fi_addr_t,
               std::uint64_t tag,
               std::uint64_t ignore,
               void* context) {
                REQUIRE(tag == 42);
                REQUIRE(ignore == 0x80);
                return capture(ep, iov, count, context, loom::completion_direction::receive);
            },
        .recvmsg =
            [](fid_ep* ep, const fi_msg_tagged* msg, std::uint64_t flags) {
                REQUIRE(msg->tag == 42);
                REQUIRE(msg->ignore == 0x80);
                return capture_message(ep, msg, flags, loom::completion_direction::receive);
            },
        .sendv =
            [](fid_ep* ep,
               const iovec* iov,
               void**,
               std::size_t count,
               fi_addr_t,
               std::uint64_t tag,
               void* context) {
                REQUIRE(tag == 42);
                return capture(ep, iov, count, context);
            },
        .sendmsg =
            [](fid_ep* ep, const fi_msg_tagged* msg, std::uint64_t flags) {
                REQUIRE(msg->tag == 42);
                return capture_message(ep, msg, flags, loom::completion_direction::transmit);
            },
    };
    fi_ops_rma rma{
        .size = sizeof(fi_ops_rma),
        .readmsg = [](fid_ep* ep,
                      const fi_msg_rma* msg,
                      std::uint64_t flags) { return capture_rma(ep, msg, flags); },
        .writemsg = [](fid_ep* ep,
                       const fi_msg_rma* msg,
                       std::uint64_t flags) { return capture_rma(ep, msg, flags); },
    };
    fid_ep handle{
        .fid = {.context = this, .ops = &ops}, .msg = &messages, .rma = &rma, .tagged = &tags};

    auto endpoint(loom::cq_bind_flags bindings = loom::cq_bind::transmit | loom::cq_bind::recv,
                  std::uint64_t receive_defaults = FI_MORE) -> loom::endpoint {
        auto ep = loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::msg,
                                                                       loom::address_format::inet,
                                                                       0UZ,
                                                                       0UZ,
                                                                       loom::caps{},
                                                                       FI_FENCE,
                                                                       receive_defaults)};
        if (bindings.has(loom::cq_bind::transmit))
            REQUIRE(ep.bind_cq(tx, loom::cq_bind::transmit));
        if (bindings.has(loom::cq_bind::recv))
            REQUIRE(ep.bind_cq(rx, loom::cq_bind::recv));
        return ep;
    }
};
}  // namespace

TEST_CASE("Asio RMA preserves registered peers and remote bounds", "[asio][endpoint]") {
    const auto read = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    const auto short_remote = GENERATE(false, true);
    const auto null_descriptor = GENERATE(false, true);
    vector_provider native;
    native.submission_status = status;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 8> storage{};
    fid_mr handle{.fid = {.ops = &native.ops}, .mem_desc = null_descriptor ? nullptr : &handle};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &handle, storage.data(), storage.size())};
    const loom::fabric_addr peer{23};
    const loom::remote_memory remote{32, 0x123456789abcdef0ULL, short_remote ? 7u : 8u};
    // The explicit RMA registration takes precedence over descriptors carried
    // by the buffer views, including when its native descriptor is null.
    fid_mr view_handle{.fid = {.ops = &native.ops}, .mem_desc = &view_handle};
    loom::memory_region view_mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &view_handle, storage.data(), storage.size())};
    std::array buffers{
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(storage.data(), 3), &view_mr, loom::asio::registered_buffer_id{0}},
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(storage.data() + 3, 5), &view_mr, loom::asio::registered_buffer_id{1}}};
    int completed{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            ++completed;
            if (short_remote)
                REQUIRE(error == loom::errc::invalid_argument);
            else if (status)
                REQUIRE(error == loom::make_error_code_from_fi_errno(-status));
            else
                REQUIRE_FALSE(error);
            REQUIRE(bytes == (short_remote || status ? 0 : storage.size()));
        });
    if (helper) {
        if (read)
            loom::asio::initiate_fabric_read{ep.native_handle()}(
                handler, buffers, mr, remote, peer);
        else
            loom::asio::initiate_fabric_write{ep.native_handle()}(
                handler, buffers, mr, remote, peer);
    } else if (read) {
        ep.async_read_from(buffers, mr, peer, remote, handler);
    } else {
        ep.async_write_to(buffers, mr, peer, remote, handler);
    }
    REQUIRE(completed == 0);
    if (!short_remote) {
        CHECK(native.submission_flags == (FI_COMPLETION | FI_FENCE));
        REQUIRE(native.peer == peer.get());
        REQUIRE(native.remote_address == remote.addr);
        REQUIRE(native.remote_key == remote.key);
        REQUIRE(native.remote_length == storage.size());
        if (null_descriptor)
            REQUIRE(native.descriptors.empty());
        else
            REQUIRE(native.descriptors == std::vector<void*>{&handle, &handle});
    }
    buffers = {};
    if (!short_remote && !status) {
        REQUIRE(native.buffers.size() == 2);
        REQUIRE(native.buffers[0].iov_base == storage.data());
        REQUIRE(native.buffers[1].iov_base == storage.data() + 3);
        loom::completion_event event;
        event.context = loom::context_ptr<void>{native.context};
        REQUIRE_FALSE(native.rx.dispatch(event));
        REQUIRE(native.tx.dispatch(event));
        REQUIRE_FALSE(native.tx.dispatch(event));
    } else {
        REQUIRE(native.context == nullptr);
    }
    ioc.run();
    REQUIRE(completed == 1);
}

TEST_CASE("Asio operations register their buffer sequences before submission", "[asio][endpoint]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    const auto initiation_helper = GENERATE(false, true);
    const auto bound = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    const bool receive = operation == 1 || operation == 3;
    CAPTURE(operation, initiation_helper, bound, status);
    vector_provider native;
    native.submission_status = status;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(),
                            native.endpoint(bound ? loom::cq_bind::transmit | loom::cq_bind::recv
                                            : receive ? loom::cq_bind::transmit
                                                      : loom::cq_bind::recv)};
    loom::memory_region mr;
    const loom::remote_memory remote{0x1000, 1, 4096};
    std::array<char, 2> first{'a', 'b'};
    std::array<char, 4> second{'c', 'd', 'e', 'f'};
    std::array<::asio::mutable_buffer, 2> buffers{::asio::buffer(first), ::asio::buffer(second)};
    std::array<::asio::const_buffer, 2> const_buffers{buffers[0], buffers[1]};
    int completions{};
    const auto expected = !bound   ? loom::make_error_code(loom::errc::invalid_argument)
                          : status ? loom::make_error_code_from_fi_errno(-status)
                                   : std::error_code{};
    auto handler = ::asio::bind_executor(ioc.get_executor(),
                                         [&](std::error_code error, std::size_t transferred) {
                                             REQUIRE(error == expected);
                                             REQUIRE(transferred == (expected ? 0 : 6));
                                             ++completions;
                                         });
    if (initiation_helper) {
        switch (operation) {
            case 0:
                loom::asio::initiate_fabric_send{ep.native_handle()}(handler, const_buffers);
                break;
            case 1:
                loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, buffers);
                break;
            case 2:
                loom::asio::initiate_fabric_tagged_send{ep.native_handle()}(
                    handler, const_buffers, 42);
                break;
            case 3:
                loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                    handler, buffers, 42, 0x80);
                break;
            case 4:
                loom::asio::initiate_fabric_write{ep.native_handle()}(
                    handler, const_buffers, mr, remote);
                break;
            case 5:
                loom::asio::initiate_fabric_read{ep.native_handle()}(handler, buffers, mr, remote);
                break;
        }
    } else {
        switch (operation) {
            case 0:
                ep.async_send(const_buffers, handler);
                break;
            case 1:
                ep.async_receive(buffers, handler);
                break;
            case 2:
                ep.async_tagged_send(const_buffers, 42, handler);
                break;
            case 3:
                ep.async_tagged_receive(buffers, 42, 0x80, handler);
                break;
            case 4:
                ep.async_write_to(const_buffers, mr, remote, handler);
                break;
            case 5:
                ep.async_read_from(buffers, mr, remote, handler);
                break;
        }
    }
    buffers = {};
    const_buffers = {};
    REQUIRE(completions == 0);
    if (bound && operation < 4) {
        CHECK(native.submission_flags == (FI_COMPLETION | (receive ? FI_MORE : FI_FENCE)));
        CHECK(native.descriptors.empty());
    }
    if (expected) {
        REQUIRE(native.context == nullptr);
        REQUIRE((native.attempted_context != nullptr) == bound);
        loom::completion_event event;
        event.context = loom::context_ptr<void>{native.attempted_context};
        REQUIRE_FALSE(native.tx.dispatch(event));
        REQUIRE_FALSE(native.rx.dispatch(event));
        ioc.run();
        REQUIRE(completions == 1);
        return;
    }
    REQUIRE(native.context != nullptr);
    REQUIRE(native.buffers.size() == 2);
    std::string sent;
    for (const auto& buffer : native.buffers) {
        sent.append(static_cast<const char*>(buffer.iov_base), buffer.iov_len);
    }
    REQUIRE(sent == "abcdef");
    if (operation == 1 || operation == 3 || operation == 5) {
        for (const auto& buffer : native.buffers) {
            std::memset(buffer.iov_base, 'x', buffer.iov_len);
        }
        REQUIRE(std::string(first.data(), first.size()) == "xx");
        REQUIRE(std::string(second.data(), second.size()) == "xxxx");
    }
    if (operation >= 4) {
        REQUIRE(native.remote_length == 6);
    }
    loom::completion_event event;
    event.bytes_transferred = operation == 1 || operation == 3 ? 6 : 0;
    event.context = loom::context_ptr<void>{native.context};
    auto& queue = receive ? native.rx : native.tx;
    auto& other = receive ? native.tx : native.rx;
    // The provider owns every scratch slot; routing must not inspect them.
    auto& scratch = *static_cast<fi_context2*>(native.context);
    for (auto& slot : scratch.internal)
        slot = &scratch;
    REQUIRE_FALSE(other.dispatch(event));
    REQUIRE(queue.dispatch(event));
    REQUIRE_FALSE(queue.dispatch(event));
    ioc.run();
    REQUIRE(completions == 1);
}

TEST_CASE("Asio messages preserve registered buffer descriptors", "[asio][endpoint][buffer]") {
    const auto receive = GENERATE(false, true);
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto single = GENERATE(false, true);
    const auto first_registered = GENERATE(false, true);
    vector_provider native;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 8> first{}, second{};
    fid_mr first_handle{.fid = {.ops = &native.ops}, .mem_desc = &first_handle};
    fid_mr second_handle{.fid = {.ops = &native.ops}, .mem_desc = &second_handle};
    loom::memory_region first_mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &first_handle, first.data(), first.size())};
    loom::memory_region second_mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(), &second_handle, second.data(), second.size())};
    const auto first_buffer = loom::asio::buffer(
        loom::asio::mutable_registered_buffer{::asio::buffer(first),
                                              first_registered ? &first_mr : nullptr,
                                              loom::asio::registered_buffer_id{0}} +
            1,
        2);
    const auto second_buffer = loom::asio::buffer(
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(second), &second_mr, loom::asio::registered_buffer_id{1}} +
            2,
        4);
    int calls{};
    const auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            ++calls;
            REQUIRE_FALSE(error);
            REQUIRE(bytes == (single ? 2 : receive ? 6 : 8));
        });
    const auto submit = [&](const auto& buffers) {
        if constexpr (::asio::is_mutable_buffer_sequence<
                          std::remove_cvref_t<decltype(buffers)>>::value) {
            if (receive) {
                if (helper) {
                    if (tagged)
                        loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                            handler, buffers, 42, 0x80);
                    else
                        loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, buffers);
                } else if (tagged)
                    ep.async_tagged_receive(buffers, 42, 0x80, handler);
                else
                    ep.async_receive(buffers, handler);
                return;
            }
        }
        if (helper) {
            if (tagged)
                loom::asio::initiate_fabric_tagged_send{ep.native_handle()}(handler, buffers, 42);
            else
                loom::asio::initiate_fabric_send{ep.native_handle()}(handler, buffers);
        } else if (tagged)
            ep.async_tagged_send(buffers, 42, handler);
        else
            ep.async_send(buffers, handler);
    };
    if (receive) {
        if (single)
            submit(first_buffer);
        else
            submit(std::array{first_buffer, second_buffer, first_buffer});
    } else {
        // Both const and mutable registered buffers must model ConstBufferSequence.
        const auto as_const = GENERATE(false, true);
        if (as_const) {
            if (single)
                submit(loom::asio::const_registered_buffer{first_buffer});
            else
                submit(std::array{loom::asio::const_registered_buffer{first_buffer},
                                  loom::asio::const_registered_buffer{second_buffer},
                                  loom::asio::const_registered_buffer{first_buffer}});
        } else if (single)
            submit(first_buffer);
        else
            submit(std::array{first_buffer, second_buffer, first_buffer});
    }
    REQUIRE(calls == 0);
    REQUIRE(native.buffers.size() == (single ? 1 : 3));
    CHECK(native.buffers[0].iov_base == first.data() + 1);
    CHECK(native.buffers[0].iov_len == 2);
    const auto first_descriptor = first_registered ? first_mr.descriptor().raw() : nullptr;
    if (single && !first_registered) {
        CHECK(native.descriptors.empty());
    } else {
        REQUIRE(native.descriptors.size() == native.buffers.size());
        CHECK(native.descriptors[0] == first_descriptor);
        if (!single) {
            CHECK(native.buffers[1].iov_base == second.data() + 2);
            CHECK(native.buffers[1].iov_len == 4);
            CHECK(native.descriptors[1] == second_mr.descriptor().raw());
            CHECK(native.descriptors[2] == first_descriptor);
        }
    }
    loom::completion_event event{.context = loom::context_ptr<void>{native.context},
                                 .bytes_transferred = single ? 2UZ : 6UZ};
    REQUIRE((receive ? native.rx : native.tx).dispatch(event));
    ioc.run();
    REQUIRE(calls == 1);
}

TEST_CASE("Asio receives reject multi-receive defaults", "[asio][endpoint]") {
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto defaults = GENERATE(FI_MULTI_RECV, FI_MULTI_RECV | FI_MORE);
    vector_provider native;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{
        ioc.get_executor(),
        native.endpoint(loom::cq_bind::transmit | loom::cq_bind::recv, defaults)};
    std::array<std::byte, 16> storage{};
    int calls{};
    std::error_code result;
    std::size_t bytes{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t count) {
            ++calls;
            result = error;
            bytes = count;
        });
    if (helper) {
        if (tagged)
            loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                handler, ::asio::buffer(storage), 42, 0x80);
        else
            loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, ::asio::buffer(storage));
    } else if (tagged) {
        ep.async_tagged_receive(::asio::buffer(storage), 42, 0x80, handler);
    } else {
        ep.async_receive(::asio::buffer(storage), handler);
    }
    CHECK(native.attempted_context == nullptr);
    REQUIRE(calls == 0);
    // Clean up an erroneous pre-fix submission with its first receive event.
    // That event must not release a real multi-receive buffer or its context.
    if (native.context) {
        loom::completion_event event{.context = loom::context_ptr<void>{native.context},
                                     .bytes_transferred = 7};
        REQUIRE(native.rx.dispatch(event));
    }
    ioc.run();
    REQUIRE(calls == 1);
    CHECK(result == loom::errc::not_supported);
    CHECK(bytes == 0);
    REQUIRE(ioc.poll() == 0);

    calls = 0;
    ioc.restart();
    ep.async_send(::asio::buffer(storage), handler);
    REQUIRE(native.submission_flags == (FI_COMPLETION | FI_FENCE));
    REQUIRE(calls == 0);
    loom::completion_event sent{.context = loom::context_ptr<void>{native.context}};
    REQUIRE(native.tx.dispatch(sent));
    ioc.run();
    REQUIRE(calls == 1);
    REQUIRE_FALSE(result);
    REQUIRE(bytes == storage.size());
}

TEST_CASE("Asio initiation exceptions release the pending context", "[asio][endpoint]") {
    vector_provider native;
    native.throw_on_submit = true;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    ::asio::cancellation_signal cancellation;
    std::array<std::byte, 1> storage{};
    auto handler =
        ::asio::bind_cancellation_slot(cancellation.slot(), [](std::error_code, std::size_t) {
            FAIL("No operation was submitted");
        });
    REQUIRE_THROWS_AS(ep.async_send(::asio::buffer(storage), handler), std::bad_alloc);
    REQUIRE_FALSE(cancellation.slot().has_handler());
    REQUIRE(native.attempted_context != nullptr);
    loom::completion_event event;
    event.context = loom::context_ptr<void>{native.attempted_context};
    REQUIRE_FALSE(native.tx.dispatch(event));
    REQUIRE(ioc.run() == 0);
}

TEST_CASE("Asio initiation rejects missing native endpoints asynchronously", "[asio][endpoint]") {
    ::asio::io_context ioc;
    std::array<std::byte, 1> storage{};
    int completions{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            REQUIRE(error == loom::errc::invalid_argument);
            REQUIRE(bytes == 0);
            ++completions;
        });
    loom::asio::initiate_fabric_send{nullptr}(handler, ::asio::buffer(storage));
    REQUIRE(completions == 0);
    ioc.run();
    REQUIRE(completions == 1);
}
