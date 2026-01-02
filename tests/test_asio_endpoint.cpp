// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_endpoint.cpp
 * @brief Tests for loom::asio::basic_endpoint.
 */

#include <loom/asio.hpp>
#include <loom/loom.hpp>

#include <array>
#include <future>

#include "ut.hpp"
#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

using namespace boost::ut;
using namespace std::literals;

suite basic_endpoint_suite = [] {
    "basic_endpoint construction with executor"_test = [] {
        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor()};

        expect(!ep.is_open());
        expect(ep.get_executor() == ioc.get_executor());
    };

    "basic_endpoint has lowest_layer_type"_test = [] {
        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor()};

        // Verify lowest_layer returns self
        auto& lowest = ep.lowest_layer();
        expect(&lowest == &ep);

        const auto& const_ep = ep;
        const auto& const_lowest = const_ep.lowest_layer();
        expect(&const_lowest == &const_ep);
    };

    "basic_endpoint close functionality"_test = [] {
        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor()};

        expect(!ep.is_open());  // Default constructed endpoint is not open

        // close() should work on closed endpoint
        ep.close();
        expect(!ep.is_open());

        // close() with error_code
        std::error_code ec;
        ep.close(ec);
        expect(!ec);
    };

    "basic_endpoint cancel functionality"_test = [] {
        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor()};

        // cancel() on closed endpoint should be safe
        ep.cancel();

        // cancel() with error_code
        std::error_code ec;
        ep.cancel(ec);
        expect(!ec);  // No error on closed endpoint
    };

    skip / "basic_endpoint construction with loom endpoint"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto ep_result = loom::endpoint::create(*domain_result, *info_result);
        expect(static_cast<bool>(ep_result)) << "Failed to create endpoint";

        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

        expect(ep.is_open());
    };

    skip / "basic_endpoint move construction"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto ep_result = loom::endpoint::create(*domain_result, *info_result);
        expect(static_cast<bool>(ep_result)) << "Failed to create endpoint";

        ::asio::io_context ioc;
        loom::asio::endpoint ep1{ioc.get_executor(), std::move(*ep_result)};
        expect(ep1.is_open());

        loom::asio::endpoint ep2{std::move(ep1)};
        expect(ep2.is_open());
    };

    skip / "basic_endpoint native_handle"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto ep_result = loom::endpoint::create(*domain_result, *info_result);
        expect(static_cast<bool>(ep_result)) << "Failed to create endpoint";

        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

        auto* handle = ep.native_handle();
        expect(handle != nullptr);
    };

    skip / "basic_endpoint get returns underlying endpoint"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto ep_result = loom::endpoint::create(*domain_result, *info_result);
        expect(static_cast<bool>(ep_result)) << "Failed to create endpoint";

        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

        auto& underlying = ep.get();
        expect(underlying.impl_valid());
    };

    "buffers_to_span conversion"_test = [] {
        std::array<std::byte, 64> buffer{};
        auto asio_buf = ::asio::buffer(buffer);

        auto span = loom::asio::buffers_to_span(asio_buf);
        expect(span.data() == buffer.data());
        expect(span.size() == 64_ul);
    };

    "buffers_to_const_span conversion"_test = [] {
        std::array<std::byte, 64> buffer{};
        auto asio_buf = ::asio::buffer(buffer);

        auto span = loom::asio::buffers_to_const_span(asio_buf);
        expect(span.data() == buffer.data());
        expect(span.size() == 64_ul);
    };

    "buffers_to_span empty buffer"_test = [] {
        std::vector<::asio::mutable_buffer> empty_seq;

        auto span = loom::asio::buffers_to_span(empty_seq);
        expect(span.empty());
    };

    skip / "make_endpoint helper function"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto cq_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
        expect(static_cast<bool>(cq_result)) << "Failed to create completion queue";

        auto av_result = loom::address_vector::create(*domain_result);
        expect(static_cast<bool>(av_result)) << "Failed to create address vector";

        ::asio::io_context ioc;

        auto ep_result =
            loom::asio::make_endpoint(ioc, *domain_result, *info_result, *cq_result, *av_result);
        expect(static_cast<bool>(ep_result)) << "Failed to create endpoint via make_endpoint";

        expect(ep_result->is_open());
    };
};

suite async_operation_signatures = [] {
    skip / "async_send has correct initiation"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto cq_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
        expect(static_cast<bool>(cq_result)) << "Failed to create completion queue";

        auto av_result = loom::address_vector::create(*domain_result);
        expect(static_cast<bool>(av_result)) << "Failed to create address vector";

        auto ep_result = loom::endpoint::create(*domain_result, *info_result);
        expect(static_cast<bool>(ep_result)) << "Failed to create endpoint";

        ep_result->bind_cq(*cq_result, loom::cq_bind::transmit | loom::cq_bind::recv).value();
        ep_result->bind_av(*av_result).value();
        ep_result->enable().value();

        ::asio::io_context ioc;
        loom::asio::endpoint ep{ioc.get_executor(), std::move(*ep_result)};

        std::array<std::byte, 64> buffer{};
        bool handler_invoked = false;

        ep.async_send(::asio::buffer(buffer),
                      [&](std::error_code, std::size_t) { handler_invoked = true; });
    };
};

auto main() -> int {
    return 0;
}
