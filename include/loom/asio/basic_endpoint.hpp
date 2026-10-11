// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file basic_endpoint.hpp
 * @brief Asio-style I/O object wrapping a loom endpoint.
 *
 * This file provides basic_endpoint, an Asio I/O object that wraps a loom
 * endpoint and provides async_* operations that integrate with Asio's
 * completion token model.
 */

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/asio/asio_context.hpp"
#include "loom/asio/blocking_context.hpp"
#include "loom/asio/fabric_service.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/messaging.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/types.hpp"
#include <asio/any_io_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/buffer.hpp>
#include <asio/io_context.hpp>

namespace loom::asio {

/**
 * @brief Converts Asio buffer sequences to loom spans.
 * @tparam BufferSequence The buffer sequence type.
 * @param buffers The buffer sequence.
 * @return A span over the first buffer's data.
 */
template <typename BufferSequence>
[[nodiscard]] auto buffers_to_span(const BufferSequence& buffers) -> std::span<std::byte> {
    auto begin = ::asio::buffer_sequence_begin(buffers);
    auto end = ::asio::buffer_sequence_end(buffers);
    if (begin == end) {
        return {};
    }
    auto buf = *begin;
    return {static_cast<std::byte*>(buf.data()), buf.size()};
}

/**
 * @brief Converts Asio const buffer sequences to loom const spans.
 * @tparam BufferSequence The buffer sequence type.
 * @param buffers The buffer sequence.
 * @return A const span over the first buffer's data.
 */
template <typename BufferSequence>
[[nodiscard]] auto buffers_to_const_span(const BufferSequence& buffers)
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
 * @brief Asio I/O object wrapping a loom fabric endpoint.
 *
 * This class provides async operations for fabric messaging that integrate
 * with Asio's completion token model, supporting callbacks, coroutines,
 * futures, and other Asio-compatible completion mechanisms.
 * Per-operation cancellation supports Asio's terminal guarantee.
 * Bind the native endpoint to the appropriate TX/RX completion queues before
 * initiating operations. Those queues must outlive accepted work. Completion
 * events can be delivered through completion_queue::dispatch from a manual
 * progress loop; registration is removed before invoking the handler.
 *
 * @tparam Executor The executor type, defaults to any_io_executor.
 */
template <typename Executor = ::asio::any_io_executor>
class basic_endpoint {
public:
    using executor_type = Executor;
    using lowest_layer_type = basic_endpoint;
    using native_handle_type = loom::endpoint*;

    /**
     * @brief Constructs an endpoint wrapper with an executor.
     * @param ex The executor to use for async operations.
     */
    explicit basic_endpoint(const executor_type& ex)
        : executor_(ex), state_(std::make_shared<detail::endpoint_state>()) {}

    /**
     * @brief Constructs an endpoint wrapper with an executor and existing endpoint.
     * @param ex The executor to use.
     * @param ep The loom endpoint to wrap (moved).
     */
    basic_endpoint(const executor_type& ex, loom::endpoint ep)
        : executor_(ex), state_(std::make_shared<detail::endpoint_state>(std::move(ep))) {}

    /**
     * @brief Move constructor.
     */
    basic_endpoint(basic_endpoint&&) noexcept = default;

    /**
     * @brief Move assignment operator.
     */
    auto operator=(basic_endpoint&& other) noexcept -> basic_endpoint& {
        if (this != &other) {
            close();
            executor_ = std::move(other.executor_);
            state_ = std::move(other.state_);
            empty_endpoint_ = std::move(other.empty_endpoint_);
        }
        return *this;
    }

    basic_endpoint(const basic_endpoint&) = delete;
    auto operator=(const basic_endpoint&) -> basic_endpoint& = delete;

    ~basic_endpoint() { close(); }

    /**
     * @brief Returns the associated executor.
     * @return The executor.
     */
    [[nodiscard]] auto get_executor() const noexcept -> executor_type { return executor_; }

    /**
     * @brief Returns a reference to the lowest layer.
     * @return Reference to this object.
     */
    [[nodiscard]] auto lowest_layer() noexcept -> lowest_layer_type& { return *this; }

    /**
     * @brief Returns a const reference to the lowest layer.
     * @return Const reference to this object.
     */
    [[nodiscard]] auto lowest_layer() const noexcept -> const lowest_layer_type& { return *this; }

    /**
     * @brief Returns the native loom endpoint handle.
     * @return Pointer to the underlying endpoint.
     */
    [[nodiscard]] auto native_handle() noexcept -> native_handle_type { return &get(); }

    /**
     * @brief Returns a const pointer to the native endpoint.
     * @return Const pointer to the underlying endpoint.
     */
    [[nodiscard]] auto native_handle() const noexcept -> const loom::endpoint* { return &get(); }

    /**
     * @brief Returns a reference to the underlying endpoint.
     * @return Reference to the loom endpoint.
     */
    [[nodiscard]] auto get() noexcept -> loom::endpoint& {
        return state_ ? state_->endpoint : empty_endpoint_;
    }

    /**
     * @brief Returns a const reference to the underlying endpoint.
     * @return Const reference to the loom endpoint.
     */
    [[nodiscard]] auto get() const noexcept -> const loom::endpoint& {
        return state_ ? state_->endpoint : empty_endpoint_;
    }

    /**
     * @brief Checks if the endpoint is valid/open.
     * @return True if the endpoint is valid.
     */
    [[nodiscard]] auto is_open() const noexcept -> bool { return get().impl_valid(); }

    /**
     * @brief Closes the endpoint, releasing underlying resources.
     *
     * After closing, is_open() will return false and no further operations
     * should be performed on this endpoint.
     * Pending operations are offered for cancellation and retain the native
     * endpoint until their completions are consumed. Keep progressing their
     * completion queues and retain their buffers until the handlers run.
     */
    void close() {
        std::error_code ignored;
        close(ignored);
    }

    /**
     * @brief Closes the endpoint, with error code output.
     * @param ec Set to the error code if an error occurs.
     */
    void close(std::error_code& ec) {
        cancel(ec);
        state_.reset();
        empty_endpoint_ = loom::endpoint{};
    }

    /**
     * @brief Cancels all pending asynchronous operations on this endpoint.
     *
     * Attempts to cancel operations started by this wrapper. Completion may
     * still succeed if the operation has finished or cancellation is unsupported.
     *
     * Any errors from the cancellation operation itself are silently ignored.
     * Use cancel(std::error_code&) if you need error information.
     */
    void cancel() {
        std::error_code ignored;
        cancel(ignored);
    }

    /**
     * @brief Cancels all pending asynchronous operations, with error code output.
     * @param ec Set to the error code if an error occurs.
     */
    void cancel(std::error_code& ec) { ec = state_ ? state_->cancel() : std::error_code{}; }

    /**
     * @brief Asynchronously sends data.
     *
     * @tparam ConstBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The data to send.
     * @param token The completion token (callback, awaitable, etc.).
     * @return Depends on the completion token type.
     *
     * The completion handler signature is: void(std::error_code, std::size_t)
     */
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto async_send(const ConstBufferSequence& buffers,
                    CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return async_send_to(
            buffers, fabric_addrs::unspecified, std::forward<CompletionToken>(token));
    }

    /// Send to an explicit peer using the registrations carried by the buffers.
    /// Buffers and registrations remain borrowed through completion.
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_send_to(const ConstBufferSequence& buffers,
                  fabric_addr peer,
                  CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, buffers, peer](auto handler) {
                auto state = operation_state();
                auto ctx = make_buffered_asio_context<false>(
                    std::move(handler), executor_, &state->endpoint, buffers);
                ctx->retain_endpoint(state);
                ctx->set_transfer_size(ctx->buffers().bytes);

                auto result = ctx->bind_completion(state->endpoint, completion_direction::transmit);
                if (result) {
                    const msg::send_message<> message{.iov = ctx->buffers().iov,
                                                      .desc = ctx->buffers().descriptors,
                                                      .dest_addr = peer,
                                                      .context = ctx->as_fi_context()};
                    result = msg::sendmsg<operation_defaults::inherit_without_more>(
                        state->endpoint, message, msg::send_flag::completion);
                }
                finish_asio_submission(std::move(ctx), std::move(result));
            },
            token);
    }

    /**
     * @brief Asynchronously receives data.
     *
     * @tparam MutableBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The buffer to receive into.
     * @param token The completion token.
     * @return Depends on the completion token type.
     *
     * The completion handler signature is: void(std::error_code, std::size_t)
     */
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_receive(const MutableBufferSequence& buffers,
                  CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return async_receive_from(
            buffers, fabric_addrs::unspecified, std::forward<CompletionToken>(token));
    }

    /// Receive from a specific peer using the registrations carried by the buffers.
    /// A specific source requires directed-receive support for this message type.
    /// Use fabric_addrs::unspecified to accept any source.
    /// Buffers and registrations remain borrowed through completion.
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto async_receive_from(
        const MutableBufferSequence& buffers,
        fabric_addr peer,
        CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, buffers, peer](auto handler) {
                auto state = operation_state();
                auto ctx = make_buffered_asio_context<true>(
                    std::move(handler), executor_, &state->endpoint, buffers);
                ctx->retain_endpoint(state);

                auto result = ctx->bind_completion(state->endpoint, completion_direction::receive);
                if (result) {
                    const msg::recv_message<> message{.iov = ctx->buffers().iov,
                                                      .desc = ctx->buffers().descriptors,
                                                      .src_addr = peer,
                                                      .context = ctx->as_fi_context()};
                    result = msg::recvmsg<operation_defaults::inherit_without_more>(
                        state->endpoint, message, msg::recv_flag::completion);
                }
                finish_asio_submission(std::move(ctx), std::move(result));
            },
            token);
    }

    /**
     * @brief Asynchronously sends data with a tag.
     *
     * @tparam ConstBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The data to send.
     * @param tag The message tag.
     * @param token The completion token.
     * @return Depends on the completion token type.
     */
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto async_tagged_send(
        const ConstBufferSequence& buffers,
        std::uint64_t tag,
        CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return async_tagged_send_to(
            buffers, fabric_addrs::unspecified, tag, std::forward<CompletionToken>(token));
    }

    /// Send to an explicit peer using the registrations carried by the buffers.
    /// Buffers and registrations remain borrowed through completion.
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto async_tagged_send_to(
        const ConstBufferSequence& buffers,
        fabric_addr peer,
        std::uint64_t tag,
        CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, buffers, peer, tag](auto handler) {
                auto state = operation_state();
                auto ctx = make_buffered_asio_context<false>(
                    std::move(handler), executor_, &state->endpoint, buffers);
                ctx->retain_endpoint(state);
                ctx->set_transfer_size(ctx->buffers().bytes);

                auto result = ctx->bind_completion(state->endpoint, completion_direction::transmit);
                if (result) {
                    const msg::tagged_send_message<> message{.iov = ctx->buffers().iov,
                                                             .desc = ctx->buffers().descriptors,
                                                             .dest_addr = peer,
                                                             .tag = tag,
                                                             .context = ctx->as_fi_context()};
                    result = msg::tagged_sendmsg<operation_defaults::inherit_without_more>(
                        state->endpoint, message, msg::send_flag::completion);
                }
                finish_asio_submission(std::move(ctx), std::move(result));
            },
            token);
    }

    /**
     * @brief Asynchronously receives data with tag matching.
     *
     * @tparam MutableBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The buffer to receive into.
     * @param tag The expected message tag.
     * @param ignore Bits to ignore in tag matching.
     * @param token The completion token.
     * @return Depends on the completion token type.
     */
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto async_tagged_receive(
        const MutableBufferSequence& buffers,
        std::uint64_t tag,
        std::uint64_t ignore,
        CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return async_tagged_receive_from(
            buffers, fabric_addrs::unspecified, tag, ignore, std::forward<CompletionToken>(token));
    }

    /// Receive from a specific peer using the registrations carried by the buffers.
    /// A specific source requires directed-receive support for this message type.
    /// Use fabric_addrs::unspecified to accept any source.
    /// Buffers and registrations remain borrowed through completion.
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto async_tagged_receive_from(
        const MutableBufferSequence& buffers,
        fabric_addr peer,
        std::uint64_t tag,
        std::uint64_t ignore,
        CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, buffers, peer, tag, ignore](auto handler) {
                auto state = operation_state();
                auto ctx = make_buffered_asio_context<true>(
                    std::move(handler), executor_, &state->endpoint, buffers);
                ctx->retain_endpoint(state);

                auto result = ctx->bind_completion(state->endpoint, completion_direction::receive);
                if (result) {
                    const msg::tagged_recv_message<> message{.iov = ctx->buffers().iov,
                                                             .desc = ctx->buffers().descriptors,
                                                             .src_addr = peer,
                                                             .tag = tag,
                                                             .ignore = ignore,
                                                             .context = ctx->as_fi_context()};
                    result = msg::tagged_recvmsg<operation_defaults::inherit_without_more>(
                        state->endpoint, message, msg::recv_flag::completion);
                }
                finish_asio_submission(std::move(ctx), std::move(result));
            },
            token);
    }

    /**
     * @brief Asynchronously writes to remote memory (RMA write).
     *
     * @tparam ConstBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The local data to write.
     * @param local_mr The memory region, kept alive through completion.
     * @param peer Destination peer (ignored by connected endpoints).
     * @param remote The remote memory descriptor.
     * @param token The completion token.
     * @return Depends on the completion token type.
     */
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_write_to(const ConstBufferSequence& buffers,
                   memory_region& local_mr,
                   fabric_addr peer,
                   remote_memory remote,
                   CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, buffers, &local_mr, peer, remote](auto handler) {
                auto state = operation_state();
                auto ctx = make_buffered_asio_context<false>(std::move(handler),
                                                             executor_,
                                                             &state->endpoint,
                                                             buffers,
                                                             local_mr.descriptor());
                ctx->retain_endpoint(state);
                ctx->set_transfer_size(ctx->buffers().bytes);

                if (!remote.contains(0, ctx->buffers().bytes) || !ctx->buffers().within(local_mr)) {
                    finish_asio_submission(std::move(ctx),
                                           make_error_result<void>(errc::invalid_argument));
                    return;
                }

                const std::array remote_iov{
                    rma::rma_iov{remote.addr, ctx->buffers().bytes, remote.key}};
                auto result = ctx->bind_completion(state->endpoint, completion_direction::transmit);
                if (result) {
                    result = rma::writev_to<operation_defaults::inherit_without_more>(
                        state->endpoint,
                        ctx->buffers().iov,
                        ctx->buffers().descriptors,
                        peer,
                        remote_iov,
                        op_flag::completion,
                        ctx->as_fi_context());
                }
                finish_asio_submission(std::move(ctx), std::move(result));
            },
            token);
    }

    /// Connected-endpoint convenience. Buffers and registration remain borrowed
    /// until completion. Use the peer overload for connectionless endpoints.
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_write_to(const ConstBufferSequence& buffers,
                   memory_region& local_mr,
                   remote_memory remote,
                   CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return async_write_to(buffers,
                              local_mr,
                              fabric_addrs::unspecified,
                              remote,
                              std::forward<CompletionToken>(token));
    }

    /**
     * @brief Asynchronously reads from remote memory (RMA read).
     *
     * @tparam MutableBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The local buffer to read into.
     * @param local_mr The memory region, kept alive through completion.
     * @param peer Destination peer (ignored by connected endpoints).
     * @param remote The remote memory descriptor.
     * @param token The completion token.
     * @return Depends on the completion token type.
     */
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_read_from(const MutableBufferSequence& buffers,
                    memory_region& local_mr,
                    fabric_addr peer,
                    remote_memory remote,
                    CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, buffers, &local_mr, peer, remote](auto handler) {
                auto state = operation_state();
                auto ctx = make_buffered_asio_context<true>(std::move(handler),
                                                            executor_,
                                                            &state->endpoint,
                                                            buffers,
                                                            local_mr.descriptor());
                ctx->retain_endpoint(state);
                ctx->set_transfer_size(ctx->buffers().bytes);

                if (!remote.contains(0, ctx->buffers().bytes) || !ctx->buffers().within(local_mr)) {
                    finish_asio_submission(std::move(ctx),
                                           make_error_result<void>(errc::invalid_argument));
                    return;
                }

                const std::array remote_iov{
                    rma::rma_iov{remote.addr, ctx->buffers().bytes, remote.key}};
                auto result = ctx->bind_completion(state->endpoint, completion_direction::transmit);
                if (result) {
                    result = rma::readv_to<operation_defaults::inherit_without_more>(
                        state->endpoint,
                        ctx->buffers().iov,
                        ctx->buffers().descriptors,
                        peer,
                        remote_iov,
                        op_flag::completion,
                        ctx->as_fi_context());
                }
                finish_asio_submission(std::move(ctx), std::move(result));
            },
            token);
    }

    /// Connected-endpoint convenience. Buffers and registration remain borrowed
    /// until completion. Use the peer overload for connectionless endpoints.
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_read_from(const MutableBufferSequence& buffers,
                    memory_region& local_mr,
                    remote_memory remote,
                    CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return async_read_from(buffers,
                               local_mr,
                               fabric_addrs::unspecified,
                               remote,
                               std::forward<CompletionToken>(token));
    }

    /**
     * @brief Sends the entire buffer sequence and waits for native completion.
     * Drives the bound CQ without running the executor or unrelated handlers.
     * Other completions remain queued. A CQ read failure requests cancellation
     * and still waits for native retirement before returning borrowed storage.
     * @param buffers The data to send.
     * @return Error code and bytes transferred.
     */
    template <typename ConstBufferSequence>
    auto send(const ConstBufferSequence& buffers) -> std::pair<std::error_code, std::size_t> {
        return detail::blocking_message<false>(operation_state(), buffers);
    }

    /**
     * @brief Receives into the buffer sequence and returns the completed length.
     * Drives the bound CQ without running the executor or unrelated handlers.
     * Other completions remain queued. Buffer and context storage remain live
     * until the provider reports terminal success or failure.
     * @param buffers The buffer to receive into.
     * @return Error code and bytes transferred.
     */
    template <typename MutableBufferSequence>
    auto receive(const MutableBufferSequence& buffers) -> std::pair<std::error_code, std::size_t> {
        return detail::blocking_message<true>(operation_state(), buffers);
    }

private:
    executor_type executor_;
    std::shared_ptr<detail::endpoint_state> state_;
    // Keep get() usable without allocating after move or close. New operations
    // adopt any endpoint assigned through this reference into shared state.
    loom::endpoint empty_endpoint_;

    [[nodiscard]] auto operation_state() -> std::shared_ptr<detail::endpoint_state> {
        if (!state_) {
            state_ = std::make_shared<detail::endpoint_state>(std::move(empty_endpoint_));
        }
        return state_;
    }
};

/**
 * @brief Type alias for basic_endpoint with the default executor.
 */
using endpoint = basic_endpoint<>;

}  // namespace loom::asio
