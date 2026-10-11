// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_endpoint.cpp
 * @brief Tests for loom::asio::basic_endpoint.
 */

#include <loom/asio.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <future>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("deferred operations retain their buffers and peers", "[asio][endpoint]") {
    const auto explicit_peer = GENERATE(false, true);
    ::asio::io_context ioc;
    struct submission {
        std::size_t bytes{};
        fi_addr_t peer{};
    } submitted;
    fi_ops ops{.size = sizeof(fi_ops),
               .close = [](fid*) { return 0; },
               .bind = [](fid*, fid*, std::uint64_t) { return 0; }};
    fid_cq cq_handle{.fid = {.ops = &ops}};
    loom::completion_queue cq{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &cq_handle)};
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .sendmsg = [](fid_ep* ep, const fi_msg* message, std::uint64_t) -> ssize_t {
            auto& posted = *static_cast<submission*>(ep->fid.context);
            posted.peer = message->addr;
            for (std::size_t i = 0; i < message->iov_count; ++i) {
                posted.bytes += message->msg_iov[i].iov_len;
            }
            return -FI_EIO;
        },
    };
    fid_ep native{.fid = {.context = &submitted, .ops = &ops}, .msg = &messages};
    loom::endpoint fabric{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
        loom::get_default_resource(),
        &native,
        loom::endpoint_types::rdm,
        loom::address_format::unspecified)};
    REQUIRE(fabric.bind_cq(cq, loom::cq_bind::transmit));
    loom::asio::endpoint endpoint{ioc.get_executor(), std::move(fabric)};
    std::array<std::byte, 64> storage{};
    std::array<::asio::mutable_buffer, 1> buffers{::asio::buffer(storage)};
    loom::fabric_addr peer{19};
    auto operation = explicit_peer ? endpoint.async_send_to(buffers, peer, ::asio::deferred)
                                   : endpoint.async_send(buffers, ::asio::deferred);
    buffers[0] = {};
    peer = loom::fabric_addr{41};
    bool completed{};
    std::move(operation)([&](std::error_code error, std::size_t) {
        REQUIRE(error == loom::errc::io_error);
        completed = true;
    });
    ioc.run();
    REQUIRE(completed);
    REQUIRE(submitted.bytes == storage.size());
    REQUIRE(submitted.peer == (explicit_peer ? 19 : FI_ADDR_UNSPEC));
}

using namespace std::literals;

TEST_CASE("basic_endpoint construction with executor", "[asio][endpoint]") {
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor()};

    REQUIRE_FALSE(ep.is_open());
    REQUIRE(ep.get_executor() == ioc.get_executor());
}

TEST_CASE("basic_endpoint has lowest_layer_type", "[asio][endpoint]") {
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor()};

    auto& lowest = ep.lowest_layer();
    REQUIRE(&lowest == &ep);

    const auto& const_ep = ep;
    const auto& const_lowest = const_ep.lowest_layer();
    REQUIRE(&const_lowest == &const_ep);
}

TEST_CASE("basic_endpoint close functionality", "[asio][endpoint]") {
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor()};

    REQUIRE_FALSE(ep.is_open());

    ep.close();
    REQUIRE_FALSE(ep.is_open());

    std::error_code ec;
    ep.close(ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("basic_endpoint cancel functionality", "[asio][endpoint]") {
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor()};

    ep.cancel();

    std::error_code ec;
    ep.cancel(ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("basic_endpoint construction with loom endpoint", "[asio][endpoint][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto ep_result = loom::endpoint::create(*domain_result, *info_result);
    REQUIRE(ep_result);

    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

    REQUIRE(ep.is_open());
}

TEST_CASE("basic_endpoint move construction", "[asio][endpoint][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto ep_result = loom::endpoint::create(*domain_result, *info_result);
    REQUIRE(ep_result);

    ::asio::io_context ioc;
    loom::asio::endpoint ep1{ioc.get_executor(), std::move(*ep_result)};
    REQUIRE(ep1.is_open());

    loom::asio::endpoint ep2{std::move(ep1)};
    REQUIRE(ep2.is_open());
}

TEST_CASE("basic_endpoint native_handle", "[asio][endpoint][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto ep_result = loom::endpoint::create(*domain_result, *info_result);
    REQUIRE(ep_result);

    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

    auto* handle = ep.native_handle();
    REQUIRE(handle != nullptr);
}

TEST_CASE("basic_endpoint get returns underlying endpoint", "[asio][endpoint][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto ep_result = loom::endpoint::create(*domain_result, *info_result);
    REQUIRE(ep_result);

    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

    auto& underlying = ep.get();
    REQUIRE(underlying.impl_valid());
}

TEST_CASE("buffers_to_span conversion", "[asio][endpoint]") {
    std::array<std::byte, 64> buffer{};
    auto asio_buf = ::asio::buffer(buffer);

    auto span = loom::asio::buffers_to_span(asio_buf);
    REQUIRE(span.data() == buffer.data());
    REQUIRE(span.size() == 64UL);
}

TEST_CASE("buffers_to_const_span conversion", "[asio][endpoint]") {
    std::array<std::byte, 64> buffer{};
    auto asio_buf = ::asio::buffer(buffer);

    auto span = loom::asio::buffers_to_const_span(asio_buf);
    REQUIRE(span.data() == buffer.data());
    REQUIRE(span.size() == 64UL);
}

TEST_CASE("buffers_to_span empty buffer", "[asio][endpoint]") {
    std::vector<::asio::mutable_buffer> empty_seq;

    auto span = loom::asio::buffers_to_span(empty_seq);
    REQUIRE(span.empty());
}

TEST_CASE("make_endpoint helper function", "[asio][endpoint][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    auto av_result = loom::address_vector::create(*domain_result);
    REQUIRE(av_result);

    ::asio::io_context ioc;

    auto ep_result =
        loom::asio::make_endpoint(ioc, *domain_result, *info_result, *cq_result, *av_result);
    REQUIRE(ep_result);

    REQUIRE(ep_result->is_open());
}

TEST_CASE("Asio rejects an unavailable native send operation", "[asio][endpoint]") {
    fi_ops ops{.size = sizeof(fi_ops),
               .close = [](fid*) { return 0; },
               .bind = [](fid*, fid*, std::uint64_t) { return 0; }};
    fid_cq cq_handle{.fid = {.ops = &ops}};
    loom::completion_queue cq{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &cq_handle)};
    fid_ep native{.fid = {.ops = &ops}};
    loom::endpoint fabric{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
        loom::get_default_resource(),
        &native,
        loom::endpoint_types::rdm,
        loom::address_format::unspecified)};
    REQUIRE(fabric.bind_cq(cq, loom::cq_bind::transmit));
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), std::move(fabric)};
    std::array<std::byte, 64> buffer{};
    int callbacks{};
    ep.async_send(::asio::buffer(buffer), [&](std::error_code error, std::size_t bytes) {
        REQUIRE(error == loom::errc::not_supported);
        REQUIRE(bytes == 0);
        ++callbacks;
    });
    REQUIRE(callbacks == 0);
    ioc.run();
    REQUIRE(callbacks == 1);
}
