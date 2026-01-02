// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file asio.hpp
 * @brief Main header for loom's Asio integration.
 *
 * This header provides integration between loom's libfabric bindings and
 * Boost.Asio/standalone Asio, enabling the use of loom endpoints with
 * Asio's async model, including support for:
 *
 * - Completion tokens (callbacks, use_awaitable, use_future, deferred)
 * - io_context integration via fabric_service
 * - DynamicBuffer support via registered_buffer
 * - Standard Asio I/O object patterns via basic_endpoint
 *
 * ## Quick Start
 *
 * ```cpp
 * #include <asio.hpp>
 * #include <loom/asio.hpp>
 *
 * asio::awaitable<void> example() {
 *     asio::io_context ioc;
 *
 *     // Setup fabric
 *     auto info = loom::query_fabric({.ep_type = loom::endpoint_types::rdm}).value();
 *     auto fabric = loom::fabric::create(info).value();
 *     auto domain = loom::domain::create(fabric, info).value();
 *     auto cq = loom::completion_queue::create(domain, {.size = 256}).value();
 *
 *     // Register CQ with Asio
 *     auto& service = loom::asio::fabric_service::use(ioc);
 *     service.register_cq(cq);
 *     service.start();
 *
 *     // Create Asio endpoint wrapper
 *     auto ep_raw = loom::endpoint::create(domain, info).value();
 *     ep_raw.bind_cq(cq, loom::cq_bind::transmit | loom::cq_bind::recv).value();
 *     ep_raw.enable().value();
 *
 *     loom::asio::endpoint ep{ioc.get_executor(), std::move(ep_raw)};
 *
 *     // Use with coroutines
 *     std::array<std::byte, 1024> buf{};
 *     auto n = co_await ep.async_receive(asio::buffer(buf), asio::use_awaitable);
 * }
 * ```
 *
 * @see loom::asio::fabric_service for io_context integration
 * @see loom::asio::basic_endpoint for the I/O object
 * @see loom::asio::registered_buffer for RDMA-aware buffers
 */

#include "loom/asio/asio_context.hpp"
#include "loom/asio/basic_endpoint.hpp"
#include "loom/asio/completion_token.hpp"
#include "loom/asio/fabric_service.hpp"
#include "loom/asio/registered_buffer.hpp"

/**
 * @namespace loom::asio
 * @brief Asio integration for loom fabric operations.
 *
 * This namespace contains types and functions for integrating loom's
 * libfabric bindings with Asio's async I/O framework.
 *
 * ## Key Components
 *
 * - `fabric_service`: Manages completion queue polling within io_context
 * - `basic_endpoint<>`: Asio I/O object wrapping a loom endpoint
 * - `registered_buffer`: DynamicBuffer adapter for RDMA memory regions
 * - `asio_context`: Internal context bridge for handler dispatch
 *
 * ## Thread Safety
 *
 * - `fabric_service` is thread-safe for registration/deregistration
 * - `basic_endpoint` operations should be serialized (typical Asio pattern)
 * - Completion handlers are dispatched via the associated executor
 *
 * ## Supported Completion Tokens
 *
 * All async operations support standard Asio completion tokens:
 * - Callbacks: `[](std::error_code ec, std::size_t n) { ... }`
 * - Coroutines: `asio::use_awaitable`
 * - Futures: `asio::use_future`
 * - Deferred: `asio::deferred`
 */
namespace loom::asio {

/**
 * @brief Creates a fabric endpoint integrated with an io_context.
 *
 * This is a convenience function that creates a properly configured
 * loom::asio::endpoint from fabric components.
 *
 * @param ioc The io_context to integrate with.
 * @param domain The loom domain.
 * @param info The fabric info for endpoint creation.
 * @param cq The completion queue to use.
 * @return A result containing the endpoint or an error.
 *
 * @code
 * auto result = loom::asio::make_endpoint(ioc, domain, info, cq);
 * if (result) {
 *     auto& ep = *result;
 *     co_await ep.async_send(buffer, asio::use_awaitable);
 * }
 * @endcode
 */
template <typename Executor>
[[nodiscard]] inline auto make_endpoint(::asio::io_context& ioc,
                                        loom::domain& domain,
                                        const loom::fabric_info& info,
                                        loom::completion_queue& cq,
                                        loom::address_vector& av)
    -> loom::result<basic_endpoint<Executor>> {
    auto ep_result = loom::endpoint::create(domain, info);
    if (!ep_result) {
        return loom::make_error_result<basic_endpoint<Executor>>(ep_result.error());
    }

    auto& ep = *ep_result;

    auto bind_result = ep.bind_cq(cq, loom::cq_bind::transmit | loom::cq_bind::recv);
    if (!bind_result) {
        return loom::make_error_result<basic_endpoint<Executor>>(bind_result.error());
    }

    auto av_result = ep.bind_av(av);
    if (!av_result) {
        return loom::make_error_result<basic_endpoint<Executor>>(av_result.error());
    }

    auto enable_result = ep.enable();
    if (!enable_result) {
        return loom::make_error_result<basic_endpoint<Executor>>(enable_result.error());
    }

    return basic_endpoint<Executor>{ioc.get_executor(), std::move(ep)};
}

/**
 * @brief Overload using the default executor type.
 */
[[nodiscard]] inline auto make_endpoint(::asio::io_context& ioc,
                                        loom::domain& domain,
                                        const loom::fabric_info& info,
                                        loom::completion_queue& cq,
                                        loom::address_vector& av) -> loom::result<endpoint> {
    return make_endpoint<::asio::any_io_executor>(ioc, domain, info, cq, av);
}

}  // namespace loom::asio
