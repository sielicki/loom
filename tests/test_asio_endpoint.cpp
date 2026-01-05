// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_endpoint.cpp
 * @brief Tests for loom::asio::basic_endpoint.
 */

#include <loom/asio.hpp>
#include <loom/loom.hpp>

#include <array>
#include <future>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("async_send has correct initiation", "[asio][endpoint][.provider]") {
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

    auto ep_result = loom::endpoint::create(*domain_result, *info_result);
    REQUIRE(ep_result);

    ep_result->bind_cq(*cq_result, loom::cq_bind::transmit | loom::cq_bind::recv).value();
    ep_result->bind_av(*av_result).value();
    ep_result->enable().value();

    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

    std::array<std::byte, 64> buffer{};
    bool handler_invoked = false;

    ep.async_send(::asio::buffer(buffer),
                  [&](std::error_code, std::size_t) { handler_invoked = true; });
}
