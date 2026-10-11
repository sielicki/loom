// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file operation_state.hpp
 * @brief P2300 operation_state for loom fabric operations.
 *
 * This header provides the operation_state types that encapsulate pending
 * fabric operations. When start() is called, the operation is submitted
 * to the fabric and will complete asynchronously.
 */

#include <rdma/fabric.h>

#include <cstddef>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/messaging.hpp"
#include "loom/core/result.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/safe_context.hpp"
#include "loom/core/types.hpp"
#include "loom/execution/concepts.hpp"
#include "loom/execution/receiver.hpp"

namespace loom::execution {

/// Progress stops at the first unregistered event, preserving its full payload.
/// The caller owns that event and can pass it to another dispatcher or handle a
/// remote notification/queue error. No context is dereferenced to identify it.
struct completion_progress {
    std::size_t completed{};
    std::optional<completion_event> unhandled;
};

/// Advance registered operations on a CQ. Bind the endpoint to this queue and
/// keep operation states and buffers alive until completion. Handle any returned
/// unregistered event before continuing the progress loop. Starting a sender
/// does not create a polling thread. Each call reads at most limit native events.
[[nodiscard]] inline auto poll_completions(completion_queue& queue, std::size_t limit = 64)
    -> result<completion_progress> {
    if (!queue) {
        return make_error_result<completion_progress>(errc::invalid_argument);
    }
    completion_progress progress;
    while (progress.completed < limit) {
        auto event = queue.poll();
        if (!event) {
            break;
        }
        if (!queue.dispatch(*event)) {
            progress.unhandled = std::move(event);
            break;
        }
        ++progress.completed;
    }
    return progress;
}

/**
 * @brief Base operation state for fabric messaging operations.
 *
 * This class manages the lifecycle of a fabric operation from submission
 * to completion. It stores the receiver and provides the fi_context for
 * the operation.
 *
 * @tparam Receiver The P2300 receiver type.
 * @tparam ValueTypes The value types for successful completion.
 */
template <typename Derived, typename Receiver, typename... ValueTypes>
class fabric_operation_state_base {
public:
    using operation_state_concept = execution::operation_state_tag;
    fabric_operation_state_base(const fabric_operation_state_base&) = delete;
    auto operator=(const fabric_operation_state_base&) -> fabric_operation_state_base& = delete;
    fabric_operation_state_base(fabric_operation_state_base&&) = delete;
    auto operator=(fabric_operation_state_base&&) -> fabric_operation_state_base& = delete;

protected:
    explicit fabric_operation_state_base(Receiver receiver, completion_queue* cq) noexcept(
        std::is_nothrow_move_constructible_v<Receiver>)
        : receiver_(std::move(receiver)) {
        // Store back-pointer for completion dispatch with validation support
        store_safe_context(fi_ctx_, static_cast<Derived*>(this));
        queue_ = cq;
    }

    ~fabric_operation_state_base() {
        // Invalidate application metadata before releasing the context
        registration_.reset();
        invalidate_safe_context(fi_ctx_);
    }

    /**
     * @brief Returns the libfabric context pointer.
     */
    [[nodiscard]] auto as_fi_context() noexcept -> void* { return &fi_ctx_.provider; }

    [[nodiscard]] auto register_completion(endpoint& ep, completion_direction direction) noexcept
        -> bool {
        if (ep.bound_cq(direction) != queue_) {
            complete_error(make_error_code(errc::invalid_argument));
            return false;
        }
        auto result = registration_.bind(ep,
                                         direction,
                                         context(),
                                         static_cast<Derived*>(this),
                                         [](void* object, const completion_event& event) noexcept {
                                             static_cast<Derived*>(object)->complete_event(event);
                                         });
        if (!result) {
            complete_error(result.error());
            return false;
        }
        return true;
    }

    template <typename Submission>
    void submit(Submission&& submission) noexcept {
        try {
            if (auto result = std::forward<Submission>(submission)(); !result)
                complete_error(result.error());
        } catch (const std::bad_alloc&) {
            complete_error(make_error_code(errc::no_memory));
        }
    }

    /**
     * @brief Handles successful completion.
     * @param bytes_transferred Number of bytes transferred.
     */
    void complete_value(std::size_t bytes_transferred = 0) noexcept {
        if constexpr (sizeof...(ValueTypes) == 0) {
            loom::execution::set_value(std::move(receiver_));
        } else if constexpr (sizeof...(ValueTypes) == 1) {
            using value_type = std::tuple_element_t<0, std::tuple<ValueTypes...>>;
            if constexpr (std::same_as<value_type, std::size_t>) {
                loom::execution::set_value(std::move(receiver_), bytes_transferred);
            } else {
                loom::execution::set_value(std::move(receiver_), value_type{});
            }
        }
    }

    /**
     * @brief Handles error completion.
     * @param ec The error code.
     */
    void complete_error(std::error_code ec) noexcept {
        registration_.reset();
        loom::execution::set_error(std::move(receiver_), ec);
    }

    /**
     * @brief Handles stopped/cancelled completion.
     */
    void complete_stopped() noexcept { loom::execution::set_stopped(std::move(receiver_)); }

    /**
     * @brief Returns the context pointer for use in fabric operations.
     */
    [[nodiscard]] auto context() noexcept -> context_ptr<void> {
        return context_ptr<void>{as_fi_context()};
    }

    void complete_event(const completion_event& event) noexcept {
        if (event.error) {
            complete_error(event.error);
        } else if constexpr (requires(Derived& operation) { operation.get_result(); }) {
            loom::execution::set_value(std::move(receiver_),
                                       static_cast<Derived*>(this)->get_result());
        } else {
            complete_value(event.bytes_transferred);
        }
    }

    safe_context fi_ctx_{};
    completion_registration registration_;
    completion_queue* queue_{};
    Receiver receiver_;
};

/**
 * @brief Operation state for send operations.
 *
 * Submits a send operation when start() is called. The receiver is
 * completed when the send finishes.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class send_operation_state final
    : public fabric_operation_state_base<send_operation_state<Receiver>, Receiver> {
    using base = fabric_operation_state_base<send_operation_state<Receiver>, Receiver>;

public:
    send_operation_state(endpoint* ep,
                         std::span<const std::byte> buffer,
                         completion_queue* cq,
                         Receiver receiver,
                         mr_descriptor descriptor = {},
                         fabric_addr peer = fabric_addrs::unspecified) noexcept
        : base(std::move(receiver), cq), ep_(ep), buffer_(buffer), cq_(cq), descriptor_(descriptor),
          peer_(peer) {}

    /**
     * @brief Starts the send operation.
     *
     * Submits the send to the fabric. The operation will complete
     * asynchronously via the completion queue.
     */
    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid() || cq_ == nullptr || !*cq_) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        if (!this->register_completion(*ep_, completion_direction::transmit)) {
            return;
        }
        const iovec buffer{const_cast<std::byte*>(buffer_.data()), buffer_.size()};
        const msg::send_message<> message{.iov = {&buffer, 1},
                                          .desc = {&descriptor_, descriptor_ ? 1UZ : 0UZ},
                                          .dest_addr = peer_,
                                          .context = this->as_fi_context()};
        this->submit([&] {
            return msg::sendmsg<operation_defaults::inherit_without_more>(
                *ep_, message, msg::send_flag::completion);
        });
        // On success, completion will come via the completion queue
    }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    completion_queue* cq_;
    mr_descriptor descriptor_;
    fabric_addr peer_;
};

/**
 * @brief Operation state for receive operations.
 *
 * Submits a receive operation when start() is called. The receiver is
 * completed with the number of bytes received.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class recv_operation_state final
    : public fabric_operation_state_base<recv_operation_state<Receiver>, Receiver, std::size_t> {
    using base = fabric_operation_state_base<recv_operation_state<Receiver>, Receiver, std::size_t>;

public:
    recv_operation_state(endpoint* ep,
                         std::span<std::byte> buffer,
                         completion_queue* cq,
                         Receiver receiver,
                         mr_descriptor descriptor = {},
                         fabric_addr peer = fabric_addrs::unspecified) noexcept
        : base(std::move(receiver), cq), ep_(ep), buffer_(buffer), cq_(cq), descriptor_(descriptor),
          peer_(peer) {}

    /**
     * @brief Starts the receive operation.
     */
    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid() || cq_ == nullptr || !*cq_) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        // This sender completes once; a multi-receive buffer remains owned by
        // the provider after ordinary receive events and needs another API.
        if (cq_->format() == cq_format::context ||
            (ep_->impl_operation_flags(completion_direction::receive) & FI_MULTI_RECV)) {
            this->complete_error(make_error_code(errc::not_supported));
            return;
        }

        if (!this->register_completion(*ep_, completion_direction::receive)) {
            return;
        }
        const iovec buffer{buffer_.data(), buffer_.size()};
        const msg::recv_message<> message{.iov = {&buffer, 1},
                                          .desc = {&descriptor_, descriptor_ ? 1UZ : 0UZ},
                                          .src_addr = peer_,
                                          .context = this->as_fi_context()};
        this->submit([&] {
            return msg::recvmsg<operation_defaults::inherit_without_more>(
                *ep_, message, msg::recv_flag::completion);
        });
    }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    completion_queue* cq_;
    mr_descriptor descriptor_;
    fabric_addr peer_;
};

/**
 * @brief Operation state for tagged send operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class tagged_send_operation_state final
    : public fabric_operation_state_base<tagged_send_operation_state<Receiver>, Receiver> {
    using base = fabric_operation_state_base<tagged_send_operation_state<Receiver>, Receiver>;

public:
    tagged_send_operation_state(endpoint* ep,
                                std::span<const std::byte> buffer,
                                std::uint64_t tag,
                                completion_queue* cq,
                                Receiver receiver,
                                mr_descriptor descriptor = {},
                                fabric_addr peer = fabric_addrs::unspecified) noexcept
        : base(std::move(receiver), cq), ep_(ep), buffer_(buffer), tag_(tag), cq_(cq),
          descriptor_(descriptor), peer_(peer) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid() || cq_ == nullptr || !*cq_) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        if (!this->register_completion(*ep_, completion_direction::transmit)) {
            return;
        }
        const iovec buffer{const_cast<std::byte*>(buffer_.data()), buffer_.size()};
        const msg::tagged_send_message<> message{.iov = {&buffer, 1},
                                                 .desc = {&descriptor_, descriptor_ ? 1UZ : 0UZ},
                                                 .dest_addr = peer_,
                                                 .tag = tag_,
                                                 .context = this->as_fi_context()};
        this->submit([&] {
            return msg::tagged_sendmsg<operation_defaults::inherit_without_more>(
                *ep_, message, msg::send_flag::completion);
        });
    }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    std::uint64_t tag_;
    completion_queue* cq_;
    mr_descriptor descriptor_;
    fabric_addr peer_;
};

/**
 * @brief Operation state for tagged receive operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class tagged_recv_operation_state final
    : public fabric_operation_state_base<tagged_recv_operation_state<Receiver>,
                                         Receiver,
                                         std::size_t> {
    using base =
        fabric_operation_state_base<tagged_recv_operation_state<Receiver>, Receiver, std::size_t>;

public:
    tagged_recv_operation_state(endpoint* ep,
                                std::span<std::byte> buffer,
                                std::uint64_t tag,
                                std::uint64_t ignore,
                                completion_queue* cq,
                                Receiver receiver,
                                mr_descriptor descriptor = {},
                                fabric_addr peer = fabric_addrs::unspecified) noexcept
        : base(std::move(receiver), cq), ep_(ep), buffer_(buffer), tag_(tag), ignore_(ignore),
          cq_(cq), descriptor_(descriptor), peer_(peer) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid() || cq_ == nullptr || !*cq_) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        if (cq_->format() == cq_format::context ||
            (ep_->impl_operation_flags(completion_direction::receive) & FI_MULTI_RECV)) {
            this->complete_error(make_error_code(errc::not_supported));
            return;
        }

        if (!this->register_completion(*ep_, completion_direction::receive)) {
            return;
        }
        const iovec buffer{buffer_.data(), buffer_.size()};
        const msg::tagged_recv_message<> message{.iov = {&buffer, 1},
                                                 .desc = {&descriptor_, descriptor_ ? 1UZ : 0UZ},
                                                 .src_addr = peer_,
                                                 .tag = tag_,
                                                 .ignore = ignore_,
                                                 .context = this->as_fi_context()};
        this->submit([&] {
            return msg::tagged_recvmsg<operation_defaults::inherit_without_more>(
                *ep_, message, msg::recv_flag::completion);
        });
    }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    std::uint64_t tag_;
    std::uint64_t ignore_;
    completion_queue* cq_;
    mr_descriptor descriptor_;
    fabric_addr peer_;
};

/**
 * @brief Operation state for RMA read operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class read_operation_state final
    : public fabric_operation_state_base<read_operation_state<Receiver>, Receiver, std::size_t> {
    using base = fabric_operation_state_base<read_operation_state<Receiver>, Receiver, std::size_t>;

public:
    read_operation_state(endpoint* ep,
                         std::span<std::byte> buffer,
                         rma_addr remote_addr,
                         mr_key key,
                         completion_queue* cq,
                         Receiver receiver) noexcept
        : read_operation_state(ep,
                               buffer,
                               mr_descriptor{},
                               fabric_addrs::unspecified,
                               remote_memory{remote_addr.get(), key.get(), buffer.size()},
                               cq,
                               std::move(receiver)) {}

    read_operation_state(endpoint* ep,
                         std::span<std::byte> buffer,
                         mr_descriptor descriptor,
                         fabric_addr peer,
                         remote_memory remote,
                         completion_queue* cq,
                         Receiver receiver) noexcept
        : base(std::move(receiver), cq), ep_(ep), buffer_(buffer), descriptor_(descriptor),
          peer_(peer), remote_(remote), cq_(cq) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid() || cq_ == nullptr || !*cq_) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        if (buffer_.size() > remote_.length) {
            this->complete_error(make_error_code(errc::invalid_argument));
            return;
        }
        if (!this->register_completion(*ep_, completion_direction::transmit)) {
            return;
        }
        auto result = rma::read_to<operation_defaults::inherit_without_more>(
            *ep_, buffer_, descriptor_, peer_, remote_, op_flag::completion, this->as_fi_context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

    [[nodiscard]] auto get_result() const noexcept -> std::size_t { return buffer_.size(); }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    mr_descriptor descriptor_;
    fabric_addr peer_;
    remote_memory remote_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for RMA write operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class write_operation_state final
    : public fabric_operation_state_base<write_operation_state<Receiver>, Receiver> {
    using base = fabric_operation_state_base<write_operation_state<Receiver>, Receiver>;

public:
    write_operation_state(endpoint* ep,
                          std::span<const std::byte> buffer,
                          rma_addr remote_addr,
                          mr_key key,
                          completion_queue* cq,
                          Receiver receiver) noexcept
        : write_operation_state(ep,
                                buffer,
                                mr_descriptor{},
                                fabric_addrs::unspecified,
                                remote_memory{remote_addr.get(), key.get(), buffer.size()},
                                cq,
                                std::move(receiver)) {}

    write_operation_state(endpoint* ep,
                          std::span<const std::byte> buffer,
                          mr_descriptor descriptor,
                          fabric_addr peer,
                          remote_memory remote,
                          completion_queue* cq,
                          Receiver receiver) noexcept
        : base(std::move(receiver), cq), ep_(ep), buffer_(buffer), descriptor_(descriptor),
          peer_(peer), remote_(remote), cq_(cq) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid() || cq_ == nullptr || !*cq_) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        if (buffer_.size() > remote_.length) {
            this->complete_error(make_error_code(errc::invalid_argument));
            return;
        }
        if (!this->register_completion(*ep_, completion_direction::transmit)) {
            return;
        }
        auto result = rma::write_to<operation_defaults::inherit_without_more>(
            *ep_, buffer_, descriptor_, peer_, remote_, op_flag::completion, this->as_fi_context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    mr_descriptor descriptor_;
    fabric_addr peer_;
    remote_memory remote_;
    completion_queue* cq_;
};

/**
 * @brief Recovers an operation state from a completion event context.
 *
 * This function is used by the completion queue polling loop to dispatch
 * completions to the appropriate operation state.
 *
 * The operation state must remain alive until its completion is consumed.
 *
 * @tparam OpState The operation state type.
 * @param ctx The context pointer from the completion event.
 * @return Pointer to the operation state, or nullptr if invalid.
 */
template <typename OpState>
[[nodiscard]] auto recover_operation_state(void* ctx) noexcept -> OpState* {
    return recover_safe_context<OpState>(ctx);
}

}  // namespace loom::execution
