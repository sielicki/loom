// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include <sys/uio.h>

#include "loom/core/concepts/fabric_object.hpp"
#include "loom/core/concepts/result.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace loom::detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct rma_addr_placeholder
 * @brief Placeholder type for RMA address in concept requirements.
 */
struct rma_addr_placeholder {};
/**
 * @struct mr_key_placeholder
 * @brief Placeholder type for memory region key in concept requirements.
 */
struct mr_key_placeholder {};
}  // namespace detail

/**
 * @concept messaging_endpoint
 * @brief Concept for endpoints supporting send/recv messaging.
 * @tparam T The endpoint type.
 *
 * Requires fabric_object_like interface plus send() and recv() methods.
 */
template <typename T>
concept messaging_endpoint = fabric_object_like<T> && requires(T& ep) {
    { ep.send(std::declval<std::span<const std::byte>>(), std::declval<void*>()) } -> result_type;
    { ep.recv(std::declval<std::span<std::byte>>(), std::declval<void*>()) } -> result_type;
};

/**
 * @concept connectable_endpoint
 * @brief Concept for endpoints supporting connection acceptance.
 * @tparam T The endpoint type.
 *
 * Requires fabric_object_like interface plus accept() method.
 */
template <typename T>
concept connectable_endpoint = fabric_object_like<T> && requires(T& ep) {
    { ep.accept() } -> result_type;
};

/**
 * @concept tagged_endpoint
 * @brief Concept for endpoints supporting tagged messaging.
 * @tparam T The endpoint type.
 *
 * Extends messaging_endpoint with tagged_send() and tagged_recv() methods.
 */
template <typename T>
concept tagged_endpoint = messaging_endpoint<T> && requires(T& ep) {
    {
        ep.tagged_send(std::declval<std::span<const std::byte>>(),
                       std::declval<std::uint64_t>(),
                       std::declval<void*>())
    } -> result_type;
    {
        ep.tagged_recv(std::declval<std::span<std::byte>>(),
                       std::declval<std::uint64_t>(),
                       std::declval<std::uint64_t>(),
                       std::declval<void*>())
    } -> result_type;
};

/**
 * @concept rma_capable_endpoint
 * @brief Concept for endpoints supporting remote memory access.
 * @tparam T The endpoint type.
 *
 * Requires fabric_object_like interface plus read() and write() RMA methods.
 */
template <typename T>
concept rma_capable_endpoint = fabric_object_like<T> && requires(T& ep) {
    {
        ep.read(std::declval<std::span<std::byte>>(),
                std::declval<detail::rma_addr_placeholder>(),
                std::declval<detail::mr_key_placeholder>(),
                std::declval<void*>())
    };
    {
        ep.write(std::declval<std::span<const std::byte>>(),
                 std::declval<detail::rma_addr_placeholder>(),
                 std::declval<detail::mr_key_placeholder>(),
                 std::declval<void*>())
    };
};

/**
 * @concept vectored_endpoint
 * @brief Concept for endpoints supporting scatter-gather I/O.
 * @tparam T The endpoint type.
 *
 * Extends messaging_endpoint with sendv() and recvv() for iovec arrays.
 */
template <typename T>
concept vectored_endpoint = messaging_endpoint<T> && requires(T& ep) {
    {
        ep.sendv(std::declval<std::span<const struct iovec>>(), std::declval<void*>())
    } -> result_type;
    {
        ep.recvv(std::declval<std::span<const struct iovec>>(), std::declval<void*>())
    } -> result_type;
};

/**
 * @concept rdma_endpoint
 * @brief Concept for RDMA-capable endpoints.
 * @tparam T The endpoint type.
 *
 * Alias for messaging_endpoint.
 */
template <typename T>
concept rdma_endpoint = messaging_endpoint<T>;

}  // namespace loom
