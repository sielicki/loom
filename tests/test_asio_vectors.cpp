// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/asio.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

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
    std::size_t remote_length{};
    bool throw_on_submit{};

    static auto capture(fid_ep* ep, const iovec* iov, std::size_t count, void* context) -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        if (self.throw_on_submit) {
            throw std::bad_alloc{};
        }
        self.buffers = {iov, count};
        self.context = context;
        return 0;
    }

    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvv =
            [](fid_ep* ep, const iovec* iov, void**, std::size_t count, fi_addr_t, void* context) {
                return capture(ep, iov, count, context);
            },
        .sendv =
            [](fid_ep* ep, const iovec* iov, void**, std::size_t count, fi_addr_t, void* context) {
                return capture(ep, iov, count, context);
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
                return capture(ep, iov, count, context);
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
    };
    fi_ops_rma rma{
        .size = sizeof(fi_ops_rma),
        .readmsg =
            [](fid_ep* ep, const fi_msg_rma* msg, std::uint64_t) {
                static_cast<vector_provider*>(ep->fid.context)->remote_length = msg->rma_iov[0].len;
                return capture(ep, msg->msg_iov, msg->iov_count, msg->context);
            },
        .writemsg =
            [](fid_ep* ep, const fi_msg_rma* msg, std::uint64_t) {
                static_cast<vector_provider*>(ep->fid.context)->remote_length = msg->rma_iov[0].len;
                return capture(ep, msg->msg_iov, msg->iov_count, msg->context);
            },
    };
    fid_ep handle{
        .fid = {.context = this, .ops = &ops}, .msg = &messages, .rma = &rma, .tagged = &tags};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::msg,
                                                                       loom::address_format::inet)};
    }
};
}  // namespace

TEST_CASE("Asio operations retain and submit every buffer", "[asio][endpoint]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    const auto initiation_helper = GENERATE(false, true);
    CAPTURE(operation, initiation_helper);
    vector_provider native;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    loom::memory_region mr;
    const loom::remote_memory remote{0x1000, 1, 4096};
    std::array<char, 2> first{'a', 'b'};
    std::array<char, 4> second{'c', 'd', 'e', 'f'};
    std::array<::asio::mutable_buffer, 2> buffers{::asio::buffer(first), ::asio::buffer(second)};
    std::array<::asio::const_buffer, 2> const_buffers{buffers[0], buffers[1]};
    bool completed{};
    auto handler = ::asio::bind_executor(ioc.get_executor(),
                                         [&](std::error_code error, std::size_t transferred) {
                                             REQUIRE_FALSE(error);
                                             REQUIRE(transferred == 6);
                                             completed = true;
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
    REQUIRE_FALSE(completed);
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
    event.bytes_transferred = 6;
    loom::asio::dispatch_asio_completion(native.context, event);
    ioc.run();
    REQUIRE(completed);
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
    REQUIRE(ioc.run() == 0);
}
