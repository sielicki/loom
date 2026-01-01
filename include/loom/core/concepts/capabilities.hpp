// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class endpoint
 * @brief Forward declaration of endpoint class.
 */
class endpoint;
/**
 * @struct remote_memory
 * @brief Forward declaration of remote memory descriptor.
 */
struct remote_memory;

/**
 * @concept msg_capable
 * @brief Concept for types supporting basic messaging.
 * @tparam T The endpoint type.
 *
 * Requires send() and recv() methods.
 */
template <typename T>
concept msg_capable = requires(T& t,
                               std::span<const std::byte> send_buf,
                               std::span<std::byte> recv_buf,
                               context_ptr<void> ctx) {
    { t.send(send_buf, ctx) } -> std::same_as<void_result>;
    { t.recv(recv_buf, ctx) } -> std::same_as<void_result>;
};

/**
 * @concept tagged_capable
 * @brief Concept for types supporting tagged messaging.
 * @tparam T The endpoint type.
 *
 * Requires tagged_send() and tagged_recv() methods.
 */
template <typename T>
concept tagged_capable = requires(T& t,
                                  std::span<const std::byte> send_buf,
                                  std::span<std::byte> recv_buf,
                                  std::uint64_t tag,
                                  std::uint64_t ignore,
                                  context_ptr<void> ctx) {
    { t.tagged_send(send_buf, tag, ctx) } -> std::same_as<void_result>;
    { t.tagged_recv(recv_buf, tag, ignore, ctx) } -> std::same_as<void_result>;
};

/**
 * @concept inject_capable
 * @brief Concept for types supporting inject operations.
 * @tparam T The endpoint type.
 *
 * Requires inject() method for small inline sends.
 */
template <typename T>
concept inject_capable = requires(T& t, std::span<const std::byte> data, fabric_addr dest) {
    { t.inject(data, dest) } -> std::same_as<void_result>;
};

/**
 * @concept inject_data_capable
 * @brief Concept for types supporting inject with immediate data.
 * @tparam T The endpoint type.
 *
 * Requires inject_data() method.
 */
template <typename T>
concept inject_data_capable =
    requires(T& t, std::span<const std::byte> data, std::uint64_t imm, fabric_addr dest) {
        { t.inject_data(data, imm, dest) } -> std::same_as<void_result>;
    };

/**
 * @concept rma_read_capable
 * @brief Concept for types supporting RMA read operations.
 * @tparam T The endpoint type.
 *
 * Requires read() method for remote memory access.
 */
template <typename T>
concept rma_read_capable =
    requires(T& t, std::span<std::byte> local, rma_addr remote, mr_key key, context_ptr<void> ctx) {
        { t.read(local, remote, key, ctx) } -> std::same_as<void_result>;
    };

/**
 * @concept rma_write_capable
 * @brief Concept for types supporting RMA write operations.
 * @tparam T The endpoint type.
 *
 * Requires write() method for remote memory access.
 */
template <typename T>
concept rma_write_capable = requires(
    T& t, std::span<const std::byte> local, rma_addr remote, mr_key key, context_ptr<void> ctx) {
    { t.write(local, remote, key, ctx) } -> std::same_as<void_result>;
};

/**
 * @concept rma_capable
 * @brief Concept for types supporting full RMA operations.
 * @tparam T The endpoint type.
 *
 * Requires both read and write RMA capabilities.
 */
template <typename T>
concept rma_capable = rma_read_capable<T> && rma_write_capable<T>;

/**
 * @concept connectable
 * @brief Concept for types supporting connection operations.
 * @tparam T The endpoint type.
 *
 * Requires connect() and accept() methods.
 */
template <typename T>
concept connectable = requires(T& t) {
    { t.connect(std::declval<const address&>()) } -> std::same_as<void_result>;
    { t.accept() } -> std::same_as<void_result>;
};

/**
 * @concept addressable
 * @brief Concept for types with local address information.
 * @tparam T The endpoint type.
 *
 * Requires get_local_address() method.
 */
template <typename T>
concept addressable = requires(const T& t) {
    { t.get_local_address() } -> std::same_as<result<address>>;
};

/**
 * @concept peer_addressable
 * @brief Concept for types with peer address information.
 * @tparam T The endpoint type.
 *
 * Requires get_peer_address() method.
 */
template <typename T>
concept peer_addressable = requires(const T& t) {
    { t.get_peer_address() } -> std::same_as<result<address>>;
};

/**
 * @concept cancellable
 * @brief Concept for types supporting operation cancellation.
 * @tparam T The endpoint type.
 *
 * Requires cancel() method.
 */
template <typename T>
concept cancellable = requires(T& t, context_ptr<void> ctx) {
    { t.cancel(ctx) } -> std::same_as<void_result>;
};

/**
 * @concept basic_endpoint
 * @brief Concept for basic endpoint capabilities.
 * @tparam T The endpoint type.
 *
 * Requires messaging and addressing capabilities.
 */
template <typename T>
concept basic_endpoint = msg_capable<T> && addressable<T>;

/**
 * @concept full_endpoint
 * @brief Concept for fully featured endpoints.
 * @tparam T The endpoint type.
 *
 * Requires all endpoint capabilities.
 */
template <typename T>
concept full_endpoint = basic_endpoint<T> && tagged_capable<T> && inject_capable<T> &&
                        rma_capable<T> && connectable<T> && cancellable<T>;

}  // namespace loom
