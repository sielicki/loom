// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file send_recv.hpp
 * @brief P2300 senders for loom messaging operations.
 *
 * This header provides sender types for fabric messaging operations including
 * send, receive, and tagged variants. These senders satisfy the P2300 sender
 * concept and can be composed with standard sender algorithms.
 *
 * ## Usage
 *
 * ```cpp
 * #include <loom/execution.hpp>
 *
 * // Create a sender factory for an endpoint
 * auto ops = loom::async(ep);
 *
 * // Create and compose senders
 * auto send_then_recv = ops.send(buffer, &cq)
 *                     | loom::execution::then([] { return 42; });
 *
 * // Or use when_all for parallel operations
 * auto both = loom::execution::when_all(
 *     ops.send(buf1, &cq),
 *     ops.recv(buf2, &cq)
 * );
 * ```
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/execution/concepts.hpp"
#include "loom/execution/operation_state.hpp"

namespace loom::execution {

// Forward declarations
class send_sender;
class recv_sender;
class tagged_send_sender;
class tagged_recv_sender;

}  // namespace loom::execution

namespace loom::execution {

/**
 * @brief Sender for fabric send operations.
 *
 * This sender represents an asynchronous send operation. When connected
 * to a receiver and started, it submits the send to the fabric and
 * completes the receiver when the operation finishes.
 *
 * ## Completion Signatures
 * - set_value() - on successful send
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 */
class send_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(), set_error_t(std::error_code), set_stopped_t()>;

    /**
     * @brief Constructs a send sender.
     * @param ep Pointer to the endpoint.
     * @param buffer The data to send.
     * @param cq Pointer to the completion queue.
     */
    send_sender(endpoint* ep, std::span<const std::byte> buffer, completion_queue* cq) noexcept
        : ep_(ep), buffer_(buffer), cq_(cq) {}

    /**
     * @brief Connects this sender to a receiver.
     * @tparam Receiver The receiver type.
     * @param receiver The receiver to connect.
     * @return An operation state for the send operation.
     */
    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> send_operation_state<std::decay_t<Receiver>> {
        return send_operation_state<std::decay_t<Receiver>>(ep_, buffer_, cq_, std::move(receiver));
    }

    /**
     * @brief Returns the sender's environment.
     */
    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    completion_queue* cq_;
};

/**
 * @brief Sender for fabric receive operations.
 *
 * This sender represents an asynchronous receive operation. On completion,
 * it provides the number of bytes received.
 *
 * ## Completion Signatures
 * - set_value(std::size_t) - bytes received on success
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 */
class recv_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures =
        loom::execution::completion_signatures<set_value_t(std::size_t),
                                               set_error_t(std::error_code),
                                               set_stopped_t()>;

    /**
     * @brief Constructs a recv sender.
     * @param ep Pointer to the endpoint.
     * @param buffer The buffer to receive into.
     * @param cq Pointer to the completion queue.
     */
    recv_sender(endpoint* ep, std::span<std::byte> buffer, completion_queue* cq) noexcept
        : ep_(ep), buffer_(buffer), cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> recv_operation_state<std::decay_t<Receiver>> {
        return recv_operation_state<std::decay_t<Receiver>>(ep_, buffer_, cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    completion_queue* cq_;
};

/**
 * @brief Sender for tagged send operations.
 *
 * Tagged sends allow receivers to selectively receive messages based on
 * a tag value.
 *
 * ## Completion Signatures
 * - set_value() - on successful send
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 */
class tagged_send_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(), set_error_t(std::error_code), set_stopped_t()>;

    tagged_send_sender(endpoint* ep,
                       std::span<const std::byte> buffer,
                       std::uint64_t tag,
                       completion_queue* cq) noexcept
        : ep_(ep), buffer_(buffer), tag_(tag), cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> tagged_send_operation_state<std::decay_t<Receiver>> {
        return tagged_send_operation_state<std::decay_t<Receiver>>(
            ep_, buffer_, tag_, cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    std::uint64_t tag_;
    completion_queue* cq_;
};

/**
 * @brief Sender for tagged receive operations.
 *
 * Tagged receives can selectively receive messages matching a tag and mask.
 *
 * ## Completion Signatures
 * - set_value(std::size_t) - bytes received on success
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 */
class tagged_recv_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures =
        loom::execution::completion_signatures<set_value_t(std::size_t),
                                               set_error_t(std::error_code),
                                               set_stopped_t()>;

    tagged_recv_sender(endpoint* ep,
                       std::span<std::byte> buffer,
                       std::uint64_t tag,
                       std::uint64_t ignore,
                       completion_queue* cq) noexcept
        : ep_(ep), buffer_(buffer), tag_(tag), ignore_(ignore), cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> tagged_recv_operation_state<std::decay_t<Receiver>> {
        return tagged_recv_operation_state<std::decay_t<Receiver>>(
            ep_, buffer_, tag_, ignore_, cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    std::uint64_t tag_;
    std::uint64_t ignore_;
    completion_queue* cq_;
};

/**
 * @brief Factory for creating messaging senders for an endpoint.
 *
 * This class provides a convenient interface for creating senders
 * for various messaging operations on a specific endpoint.
 *
 * ## Usage
 *
 * ```cpp
 * loom::endpoint ep = ...;
 * loom::completion_queue cq = ...;
 *
 * auto ops = loom::async(ep);
 * auto s1 = ops.send(buffer, &cq);
 * auto s2 = ops.recv(buffer, &cq);
 * auto s3 = ops.tagged_send(buffer, 0x1234, &cq);
 * ```
 */
class endpoint_sender_factory {
public:
    /**
     * @brief Constructs a sender factory for an endpoint.
     * @param ep Reference to the endpoint.
     */
    explicit endpoint_sender_factory(endpoint& ep) noexcept : ep_(&ep) {}

    /**
     * @brief Creates a send sender.
     * @param buffer The data to send.
     * @param cq Pointer to the completion queue.
     * @return A send_sender.
     */
    [[nodiscard]] auto send(std::span<const std::byte> buffer, completion_queue* cq) const noexcept
        -> send_sender {
        return send_sender(ep_, buffer, cq);
    }

    /**
     * @brief Creates a send sender from a typed buffer.
     * @tparam T The element type.
     * @tparam N The array size.
     * @param buffer The data to send.
     * @param cq Pointer to the completion queue.
     * @return A send_sender.
     */
    template <typename T, std::size_t N>
    [[nodiscard]] auto send(const std::array<T, N>& buffer, completion_queue* cq) const noexcept
        -> send_sender {
        return send_sender(
            ep_,
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()),
                                       buffer.size() * sizeof(T)),
            cq);
    }

    /**
     * @brief Creates a recv sender.
     * @param buffer The buffer to receive into.
     * @param cq Pointer to the completion queue.
     * @return A recv_sender.
     */
    [[nodiscard]] auto recv(std::span<std::byte> buffer, completion_queue* cq) const noexcept
        -> recv_sender {
        return recv_sender(ep_, buffer, cq);
    }

    /**
     * @brief Creates a recv sender from a typed buffer.
     */
    template <typename T, std::size_t N>
    [[nodiscard]] auto recv(std::array<T, N>& buffer, completion_queue* cq) const noexcept
        -> recv_sender {
        return recv_sender(ep_,
                           std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()),
                                                buffer.size() * sizeof(T)),
                           cq);
    }

    /**
     * @brief Creates a tagged send sender.
     * @param buffer The data to send.
     * @param tag The message tag.
     * @param cq Pointer to the completion queue.
     * @return A tagged_send_sender.
     */
    [[nodiscard]] auto tagged_send(std::span<const std::byte> buffer,
                                   std::uint64_t tag,
                                   completion_queue* cq) const noexcept -> tagged_send_sender {
        return tagged_send_sender(ep_, buffer, tag, cq);
    }

    template <typename T, std::size_t N>
    [[nodiscard]] auto tagged_send(const std::array<T, N>& buffer,
                                   std::uint64_t tag,
                                   completion_queue* cq) const noexcept -> tagged_send_sender {
        return tagged_send_sender(
            ep_,
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()),
                                       buffer.size() * sizeof(T)),
            tag,
            cq);
    }

    /**
     * @brief Creates a tagged recv sender.
     * @param buffer The buffer to receive into.
     * @param tag The expected message tag.
     * @param ignore Bits to ignore in tag matching.
     * @param cq Pointer to the completion queue.
     * @return A tagged_recv_sender.
     */
    [[nodiscard]] auto tagged_recv(std::span<std::byte> buffer,
                                   std::uint64_t tag,
                                   std::uint64_t ignore,
                                   completion_queue* cq) const noexcept -> tagged_recv_sender {
        return tagged_recv_sender(ep_, buffer, tag, ignore, cq);
    }

    template <typename T, std::size_t N>
    [[nodiscard]] auto tagged_recv(std::array<T, N>& buffer,
                                   std::uint64_t tag,
                                   std::uint64_t ignore,
                                   completion_queue* cq) const noexcept -> tagged_recv_sender {
        return tagged_recv_sender(ep_,
                                  std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()),
                                                       buffer.size() * sizeof(T)),
                                  tag,
                                  ignore,
                                  cq);
    }

private:
    endpoint* ep_;
};

}  // namespace loom::execution

namespace loom {

/**
 * @brief Creates a sender factory for the given endpoint.
 * @param ep Reference to the endpoint.
 * @return An endpoint_sender_factory.
 *
 * ## Usage
 *
 * ```cpp
 * auto senders = loom::async(ep);
 * auto send_op = senders.send(buffer, &cq);
 * ```
 */
[[nodiscard]] inline auto async(endpoint& ep) noexcept -> execution::endpoint_sender_factory {
    return execution::endpoint_sender_factory(ep);
}

}  // namespace loom
