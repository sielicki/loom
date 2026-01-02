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

#include <cstddef>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/asio/asio_context.hpp"
#include "loom/asio/fabric_service.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/types.hpp"
#include <asio/any_io_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/buffer.hpp>
#include <asio/default_completion_token.hpp>
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
    explicit basic_endpoint(const executor_type& ex) : executor_(ex) {}

    /**
     * @brief Constructs an endpoint wrapper with an executor and existing endpoint.
     * @param ex The executor to use.
     * @param ep The loom endpoint to wrap (moved).
     */
    basic_endpoint(const executor_type& ex, loom::endpoint ep)
        : executor_(ex), endpoint_(std::move(ep)) {}

    /**
     * @brief Move constructor.
     */
    basic_endpoint(basic_endpoint&&) noexcept = default;

    /**
     * @brief Move assignment operator.
     */
    auto operator=(basic_endpoint&&) noexcept -> basic_endpoint& = default;

    basic_endpoint(const basic_endpoint&) = delete;
    auto operator=(const basic_endpoint&) -> basic_endpoint& = delete;

    ~basic_endpoint() = default;

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
    [[nodiscard]] auto native_handle() noexcept -> native_handle_type { return &endpoint_; }

    /**
     * @brief Returns a const pointer to the native endpoint.
     * @return Const pointer to the underlying endpoint.
     */
    [[nodiscard]] auto native_handle() const noexcept -> const loom::endpoint* {
        return &endpoint_;
    }

    /**
     * @brief Returns a reference to the underlying endpoint.
     * @return Reference to the loom endpoint.
     */
    [[nodiscard]] auto get() noexcept -> loom::endpoint& { return endpoint_; }

    /**
     * @brief Returns a const reference to the underlying endpoint.
     * @return Const reference to the loom endpoint.
     */
    [[nodiscard]] auto get() const noexcept -> const loom::endpoint& { return endpoint_; }

    /**
     * @brief Checks if the endpoint is valid/open.
     * @return True if the endpoint is valid.
     */
    [[nodiscard]] auto is_open() const noexcept -> bool { return endpoint_.impl_valid(); }

    /**
     * @brief Closes the endpoint, releasing underlying resources.
     *
     * After closing, is_open() will return false and no further operations
     * should be performed on this endpoint.
     */
    void close() { endpoint_ = loom::endpoint{}; }

    /**
     * @brief Closes the endpoint, with error code output.
     * @param ec Set to the error code if an error occurs.
     */
    void close(std::error_code& ec) {
        ec.clear();
        endpoint_ = loom::endpoint{};
    }

    /**
     * @brief Cancels all pending asynchronous operations on this endpoint.
     *
     * This causes all outstanding async operations to complete with
     * operation_canceled error.
     *
     * Any errors from the cancellation operation itself are silently ignored.
     * Use cancel(std::error_code&) if you need error information.
     */
    void cancel() {
        if (is_open()) {
            [[maybe_unused]] auto result = endpoint_.cancel(context_ptr<void>{nullptr});
            // Errors during cancellation are ignored in this overload.
            // The important outcome is that pending operations will be cancelled.
        }
    }

    /**
     * @brief Cancels all pending asynchronous operations, with error code output.
     * @param ec Set to the error code if an error occurs.
     */
    void cancel(std::error_code& ec) {
        ec.clear();
        if (is_open()) {
            auto result = endpoint_.cancel(context_ptr<void>{nullptr});
            if (!result) {
                ec = result.error();
            }
        }
    }

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
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, &buffers](auto handler) {
                auto span = buffers_to_const_span(buffers);
                auto* ctx = make_asio_context(std::move(handler), executor_, &endpoint_);

                auto result = endpoint_.send(span, context_ptr<void>{ctx->as_fi_context()});
                if (!result) {
                    ctx->fail(result.error());
                }
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
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, &buffers](auto handler) {
                auto span = buffers_to_span(buffers);
                auto* ctx = make_asio_context(std::move(handler), executor_, &endpoint_);

                auto result = endpoint_.recv(span, context_ptr<void>{ctx->as_fi_context()});
                if (!result) {
                    ctx->fail(result.error());
                }
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
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, &buffers, tag](auto handler) {
                auto span = buffers_to_const_span(buffers);
                auto* ctx = make_asio_context(std::move(handler), executor_, &endpoint_);

                auto result = endpoint_.tagged_send(span, tag, ctx->as_fi_context());
                if (!result) {
                    ctx->fail(result.error());
                }
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
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, &buffers, tag, ignore](auto handler) {
                auto span = buffers_to_span(buffers);
                auto* ctx = make_asio_context(std::move(handler), executor_, &endpoint_);

                auto result = endpoint_.tagged_recv(span, tag, ignore, ctx->as_fi_context());
                if (!result) {
                    ctx->fail(result.error());
                }
            },
            token);
    }

    /**
     * @brief Asynchronously writes to remote memory (RMA write).
     *
     * @tparam ConstBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The local data to write.
     * @param local_mr The memory region for the local buffer.
     * @param remote The remote memory descriptor.
     * @param token The completion token.
     * @return Depends on the completion token type.
     */
    template <typename ConstBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_write_to(const ConstBufferSequence& buffers,
                   memory_region& local_mr,
                   remote_memory remote,
                   CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, &buffers, &local_mr, remote](auto handler) {
                auto span = buffers_to_const_span(buffers);
                auto* ctx = make_asio_context(std::move(handler), executor_, &endpoint_);

                auto result = rma::write(endpoint_, span, local_mr, remote, ctx->as_fi_context());
                if (!result) {
                    ctx->fail(result.error());
                }
            },
            token);
    }

    /**
     * @brief Asynchronously reads from remote memory (RMA read).
     *
     * @tparam MutableBufferSequence The buffer sequence type.
     * @tparam CompletionToken The completion token type.
     * @param buffers The local buffer to read into.
     * @param local_mr The memory region for the local buffer.
     * @param remote The remote memory descriptor.
     * @param token The completion token.
     * @return Depends on the completion token type.
     */
    template <typename MutableBufferSequence,
              typename CompletionToken = ::asio::default_completion_token_t<executor_type>>
    auto
    async_read_from(const MutableBufferSequence& buffers,
                    memory_region& local_mr,
                    remote_memory remote,
                    CompletionToken&& token = ::asio::default_completion_token_t<executor_type>{}) {
        return ::asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
            [this, &buffers, &local_mr, remote](auto handler) {
                auto span = buffers_to_span(buffers);
                auto* ctx = make_asio_context(std::move(handler), executor_, &endpoint_);

                auto result = rma::read(endpoint_, span, local_mr, remote, ctx->as_fi_context());
                if (!result) {
                    ctx->fail(result.error());
                }
            },
            token);
    }

    /**
     * @brief Sends data synchronously (blocking).
     * @param buffers The data to send.
     * @return Error code and bytes transferred.
     */
    template <typename ConstBufferSequence>
    auto send(const ConstBufferSequence& buffers) -> std::pair<std::error_code, std::size_t> {
        auto span = buffers_to_const_span(buffers);
        auto result = endpoint_.send(span);
        if (!result) {
            return {result.error(), 0};
        }
        return {{}, span.size()};
    }

    /**
     * @brief Receives data synchronously (blocking).
     * @param buffers The buffer to receive into.
     * @return Error code and bytes transferred.
     */
    template <typename MutableBufferSequence>
    auto receive(const MutableBufferSequence& buffers) -> std::pair<std::error_code, std::size_t> {
        auto span = buffers_to_span(buffers);
        auto result = endpoint_.recv(span);
        if (!result) {
            return {result.error(), 0};
        }
        return {{}, span.size()};
    }

private:
    executor_type executor_;
    loom::endpoint endpoint_;
};

/**
 * @brief Type alias for basic_endpoint with the default executor.
 */
using endpoint = basic_endpoint<>;

}  // namespace loom::asio
