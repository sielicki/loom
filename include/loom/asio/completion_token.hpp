// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file completion_token.hpp
 * @brief Completion token support for loom async operations.
 *
 * This file provides completion token helpers and traits for integrating
 * loom operations with Asio's completion token model, including support
 * for use_awaitable, use_future, and deferred.
 */

#include <cstddef>
#include <span>
#include <system_error>
#include <tuple>
#include <type_traits>

#include "loom/asio/asio_context.hpp"
#include "loom/async/completion_queue.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/types.hpp"
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/buffer.hpp>
#include <asio/deferred.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

namespace loom::asio {

/**
 * @brief The default completion signature for fabric operations.
 *
 * This signature represents the result of a fabric async operation:
 * - error_code: The operation error (or success)
 * - size_t: The number of bytes transferred
 */
using default_completion_signature = void(std::error_code, std::size_t);

/**
 * @brief Completion signature for operations returning completion events.
 *
 * For operations where the full completion event details are needed.
 */
using event_completion_signature = void(std::error_code, completion_event);

/**
 * @brief Helper to extract the return type from a completion token.
 * @tparam CompletionToken The completion token type.
 * @tparam Signature The completion signature.
 */
template <typename CompletionToken, typename Signature = default_completion_signature>
using completion_return_type =
    typename ::asio::async_result<std::decay_t<CompletionToken>, Signature>::return_type;

/**
 * @brief Concept for valid fabric completion tokens.
 * @tparam T The type to check.
 */
template <typename T>
concept fabric_completion_token =
    ::asio::completion_token_for<T, void(std::error_code, std::size_t)>;

/**
 * @brief Tag type for indicating deferred operation initialization.
 */
struct use_fabric_deferred_t {
    constexpr use_fabric_deferred_t() = default;
};

/**
 * @brief Constant for deferred fabric operations.
 */
inline constexpr use_fabric_deferred_t use_fabric_deferred{};

/**
 * @brief Wrapper that adds cancellation support to a completion handler.
 * @tparam Handler The wrapped handler type.
 */
template <typename Handler>
class cancellable_handler {
public:
    using cancellation_slot_type = typename ::asio::associated_cancellation_slot<Handler>::type;
    using executor_type = typename ::asio::associated_executor<Handler>::type;
    using allocator_type = typename ::asio::associated_allocator<Handler>::type;

    /**
     * @brief Constructs a cancellable handler.
     * @param handler The handler to wrap.
     */
    explicit cancellable_handler(Handler handler) : handler_(std::move(handler)) {}

    /**
     * @brief Invokes the handler with the given arguments.
     * @tparam Args Argument types.
     * @param args The arguments to pass to the handler.
     */
    template <typename... Args>
    void operator()(Args&&... args) {
        std::move(handler_)(std::forward<Args>(args)...);
    }

    /**
     * @brief Returns the associated cancellation slot.
     * @return The cancellation slot.
     */
    [[nodiscard]] auto get_cancellation_slot() const noexcept -> cancellation_slot_type {
        return ::asio::get_associated_cancellation_slot(handler_);
    }

    /**
     * @brief Returns the associated executor.
     * @return The executor.
     */
    [[nodiscard]] auto get_executor() const noexcept -> executor_type {
        return ::asio::get_associated_executor(handler_);
    }

    /**
     * @brief Returns the associated allocator.
     * @return The allocator.
     */
    [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
        return ::asio::get_associated_allocator(handler_);
    }

private:
    Handler handler_;
};

/**
 * @brief Creates a cancellable handler wrapper.
 * @tparam Handler The handler type.
 * @param handler The handler to wrap.
 * @return A cancellable_handler wrapping the handler.
 */
template <typename Handler>
[[nodiscard]] auto make_cancellable(Handler&& handler)
    -> cancellable_handler<std::decay_t<Handler>> {
    return cancellable_handler<std::decay_t<Handler>>(std::forward<Handler>(handler));
}

/**
 * @brief Result type for fabric async operations returning bytes transferred.
 */
struct transfer_result {
    std::error_code ec;
    std::size_t bytes_transferred{0};

    /**
     * @brief Checks if the operation succeeded.
     * @return True if no error occurred.
     */
    [[nodiscard]] explicit operator bool() const noexcept { return !ec; }

    /**
     * @brief Throws if an error occurred.
     * @throws std::system_error if ec is set.
     */
    void throw_if_error() const {
        if (ec) {
            throw std::system_error(ec);
        }
    }
};

/**
 * @brief Converts Asio const buffer sequences to spans.
 */
template <typename BufferSequence>
[[nodiscard]] inline auto to_const_span(const BufferSequence& buffers)
    -> std::span<const std::byte> {
    auto begin = ::asio::buffer_sequence_begin(buffers);
    auto end = ::asio::buffer_sequence_end(buffers);
    if (begin == end) {
        return {};
    }
    auto buf = *begin;
    return {static_cast<const std::byte*>(buf.data()), buf.size()};
}

/**
 * @brief Converts Asio mutable buffer sequences to spans.
 */
template <typename BufferSequence>
[[nodiscard]] inline auto to_mutable_span(const BufferSequence& buffers) -> std::span<std::byte> {
    auto begin = ::asio::buffer_sequence_begin(buffers);
    auto end = ::asio::buffer_sequence_end(buffers);
    if (begin == end) {
        return {};
    }
    auto buf = *begin;
    return {static_cast<std::byte*>(buf.data()), buf.size()};
}

/**
 * @brief Initiation function object for fabric send operations.
 *
 * This class template provides the async_initiate compatible initiation
 * for fabric send operations.
 */
struct initiate_fabric_send {
    loom::endpoint* ep_;

    explicit initiate_fabric_send(loom::endpoint* ep) noexcept : ep_(ep) {}

    /**
     * @brief Initiates the send operation.
     * @tparam Handler The handler type.
     * @tparam ConstBufferSequence The buffer sequence type.
     * @param handler The completion handler.
     * @param buffers The buffers to send.
     */
    template <typename Handler, typename ConstBufferSequence>
    void operator()(Handler&& handler, const ConstBufferSequence& buffers) const {
        auto executor = ::asio::get_associated_executor(handler);
        auto* ctx = make_asio_context(std::forward<Handler>(handler), executor, ep_);

        auto span = to_const_span(buffers);
        auto result = ep_->send(span, context_ptr<void>{ctx->as_fi_context()});
        if (!result) {
            ctx->fail(result.error());
        }
    }
};

/**
 * @brief Initiation function object for fabric receive operations.
 */
struct initiate_fabric_recv {
    loom::endpoint* ep_;

    explicit initiate_fabric_recv(loom::endpoint* ep) noexcept : ep_(ep) {}

    /**
     * @brief Initiates the receive operation.
     * @tparam Handler The handler type.
     * @tparam MutableBufferSequence The buffer sequence type.
     * @param handler The completion handler.
     * @param buffers The buffers to receive into.
     */
    template <typename Handler, typename MutableBufferSequence>
    void operator()(Handler&& handler, const MutableBufferSequence& buffers) const {
        auto executor = ::asio::get_associated_executor(handler);
        auto* ctx = make_asio_context(std::forward<Handler>(handler), executor, ep_);

        auto span = to_mutable_span(buffers);
        auto result = ep_->recv(span, context_ptr<void>{ctx->as_fi_context()});
        if (!result) {
            ctx->fail(result.error());
        }
    }
};

/**
 * @brief Initiation function object for fabric tagged send operations.
 */
struct initiate_fabric_tagged_send {
    loom::endpoint* ep_;

    explicit initiate_fabric_tagged_send(loom::endpoint* ep) noexcept : ep_(ep) {}

    /**
     * @brief Initiates the tagged send operation.
     * @tparam Handler The handler type.
     * @tparam ConstBufferSequence The buffer sequence type.
     * @param handler The completion handler.
     * @param buffers The buffers to send.
     * @param tag The message tag.
     */
    template <typename Handler, typename ConstBufferSequence>
    void
    operator()(Handler&& handler, const ConstBufferSequence& buffers, std::uint64_t tag) const {
        auto executor = ::asio::get_associated_executor(handler);
        auto* ctx = make_asio_context(std::forward<Handler>(handler), executor, ep_);

        auto span = to_const_span(buffers);
        auto result = ep_->tagged_send(span, tag, ctx->as_fi_context());
        if (!result) {
            ctx->fail(result.error());
        }
    }
};

/**
 * @brief Initiation function object for fabric tagged receive operations.
 */
struct initiate_fabric_tagged_recv {
    loom::endpoint* ep_;

    explicit initiate_fabric_tagged_recv(loom::endpoint* ep) noexcept : ep_(ep) {}

    /**
     * @brief Initiates the tagged receive operation.
     * @tparam Handler The handler type.
     * @tparam MutableBufferSequence The buffer sequence type.
     * @param handler The completion handler.
     * @param buffers The buffers to receive into.
     * @param tag The expected message tag.
     * @param ignore Bits to ignore in tag matching.
     */
    template <typename Handler, typename MutableBufferSequence>
    void operator()(Handler&& handler,
                    const MutableBufferSequence& buffers,
                    std::uint64_t tag,
                    std::uint64_t ignore) const {
        auto executor = ::asio::get_associated_executor(handler);
        auto* ctx = make_asio_context(std::forward<Handler>(handler), executor, ep_);

        auto span = to_mutable_span(buffers);
        auto result = ep_->tagged_recv(span, tag, ignore, ctx->as_fi_context());
        if (!result) {
            ctx->fail(result.error());
        }
    }
};

/**
 * @brief Initiation function object for fabric RMA write operations.
 */
struct initiate_fabric_write {
    loom::endpoint* ep_;

    explicit initiate_fabric_write(loom::endpoint* ep) noexcept : ep_(ep) {}

    /**
     * @brief Initiates the RMA write operation.
     * @tparam Handler The handler type.
     * @tparam ConstBufferSequence The buffer sequence type.
     * @param handler The completion handler.
     * @param buffers The local buffers.
     * @param mr The memory region.
     * @param remote The remote memory descriptor.
     */
    template <typename Handler, typename ConstBufferSequence>
    void operator()(Handler&& handler,
                    const ConstBufferSequence& buffers,
                    memory_region& mr,
                    remote_memory remote) const {
        auto executor = ::asio::get_associated_executor(handler);
        auto* ctx = make_asio_context(std::forward<Handler>(handler), executor, ep_);

        auto span = to_const_span(buffers);
        auto result = rma::write(*ep_, span, mr, remote, ctx->as_fi_context());
        if (!result) {
            ctx->fail(result.error());
        }
    }
};

/**
 * @brief Initiation function object for fabric RMA read operations.
 */
struct initiate_fabric_read {
    loom::endpoint* ep_;

    explicit initiate_fabric_read(loom::endpoint* ep) noexcept : ep_(ep) {}

    /**
     * @brief Initiates the RMA read operation.
     * @tparam Handler The handler type.
     * @tparam MutableBufferSequence The buffer sequence type.
     * @param handler The completion handler.
     * @param buffers The local buffers.
     * @param mr The memory region.
     * @param remote The remote memory descriptor.
     */
    template <typename Handler, typename MutableBufferSequence>
    void operator()(Handler&& handler,
                    const MutableBufferSequence& buffers,
                    memory_region& mr,
                    remote_memory remote) const {
        auto executor = ::asio::get_associated_executor(handler);
        auto* ctx = make_asio_context(std::forward<Handler>(handler), executor, ep_);

        auto span = to_mutable_span(buffers);
        auto result = rma::read(*ep_, span, mr, remote, ctx->as_fi_context());
        if (!result) {
            ctx->fail(result.error());
        }
    }
};

}  // namespace loom::asio
