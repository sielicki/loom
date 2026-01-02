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
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"
#include "loom/execution/concepts.hpp"
#include "loom/execution/receiver.hpp"

namespace loom::execution {

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
template <typename Receiver, typename... ValueTypes>
class fabric_operation_state_base {
public:
    fabric_operation_state_base(const fabric_operation_state_base&) = delete;
    auto operator=(const fabric_operation_state_base&) -> fabric_operation_state_base& = delete;
    fabric_operation_state_base(fabric_operation_state_base&&) = delete;
    auto operator=(fabric_operation_state_base&&) -> fabric_operation_state_base& = delete;

protected:
    explicit fabric_operation_state_base(Receiver receiver) noexcept(
        std::is_nothrow_move_constructible_v<Receiver>)
        : receiver_(std::move(receiver)) {
        // Store back-pointer for completion dispatch
        fi_ctx_.internal[0] = static_cast<void*>(this);
    }

    ~fabric_operation_state_base() = default;

    /**
     * @brief Returns the libfabric context pointer.
     */
    [[nodiscard]] auto as_fi_context() noexcept -> void* { return &fi_ctx_; }

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

    ::fi_context2 fi_ctx_{};
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
class send_operation_state final : public fabric_operation_state_base<Receiver> {
    using base = fabric_operation_state_base<Receiver>;

public:
    send_operation_state(endpoint* ep,
                         std::span<const std::byte> buffer,
                         completion_queue* cq,
                         Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), buffer_(buffer), cq_(cq) {}

    /**
     * @brief Starts the send operation.
     *
     * Submits the send to the fabric. The operation will complete
     * asynchronously via the completion queue.
     */
    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        auto result = ep_->send(buffer_, this->context());
        if (!result) {
            this->complete_error(result.error());
        }
        // On success, completion will come via the completion queue
    }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    completion_queue* cq_;
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
class recv_operation_state final : public fabric_operation_state_base<Receiver, std::size_t> {
    using base = fabric_operation_state_base<Receiver, std::size_t>;

public:
    recv_operation_state(endpoint* ep,
                         std::span<std::byte> buffer,
                         completion_queue* cq,
                         Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), buffer_(buffer), cq_(cq) {}

    /**
     * @brief Starts the receive operation.
     */
    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        auto result = ep_->recv(buffer_, this->context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for tagged send operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class tagged_send_operation_state final : public fabric_operation_state_base<Receiver> {
    using base = fabric_operation_state_base<Receiver>;

public:
    tagged_send_operation_state(endpoint* ep,
                                std::span<const std::byte> buffer,
                                std::uint64_t tag,
                                completion_queue* cq,
                                Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), buffer_(buffer), tag_(tag), cq_(cq) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        auto result = ep_->tagged_send(buffer_, tag_, this->context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    std::uint64_t tag_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for tagged receive operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class tagged_recv_operation_state final
    : public fabric_operation_state_base<Receiver, std::size_t> {
    using base = fabric_operation_state_base<Receiver, std::size_t>;

public:
    tagged_recv_operation_state(endpoint* ep,
                                std::span<std::byte> buffer,
                                std::uint64_t tag,
                                std::uint64_t ignore,
                                completion_queue* cq,
                                Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), buffer_(buffer), tag_(tag), ignore_(ignore), cq_(cq) {
    }

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        auto result = ep_->tagged_recv(buffer_, tag_, ignore_, this->context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    std::uint64_t tag_;
    std::uint64_t ignore_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for RMA read operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class read_operation_state final : public fabric_operation_state_base<Receiver, std::size_t> {
    using base = fabric_operation_state_base<Receiver, std::size_t>;

public:
    read_operation_state(endpoint* ep,
                         std::span<std::byte> buffer,
                         rma_addr remote_addr,
                         mr_key key,
                         completion_queue* cq,
                         Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), buffer_(buffer), remote_addr_(remote_addr), key_(key),
          cq_(cq) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        auto result = ep_->read(buffer_, remote_addr_, key_, this->context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    rma_addr remote_addr_;
    mr_key key_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for RMA write operations.
 *
 * @tparam Receiver The P2300 receiver type.
 */
template <typename Receiver>
class write_operation_state final : public fabric_operation_state_base<Receiver> {
    using base = fabric_operation_state_base<Receiver>;

public:
    write_operation_state(endpoint* ep,
                          std::span<const std::byte> buffer,
                          rma_addr remote_addr,
                          mr_key key,
                          completion_queue* cq,
                          Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), buffer_(buffer), remote_addr_(remote_addr), key_(key),
          cq_(cq) {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        auto result = ep_->write(buffer_, remote_addr_, key_, this->context());
        if (!result) {
            this->complete_error(result.error());
        }
    }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    rma_addr remote_addr_;
    mr_key key_;
    completion_queue* cq_;
};

/**
 * @brief Recovers an operation state from a completion event context.
 *
 * This function is used by the completion queue polling loop to dispatch
 * completions to the appropriate operation state.
 *
 * @tparam OpState The operation state type.
 * @param ctx The context pointer from the completion event.
 * @return Pointer to the operation state, or nullptr if invalid.
 */
template <typename OpState>
[[nodiscard]] auto recover_operation_state(void* ctx) noexcept -> OpState* {
    if (ctx == nullptr) {
        return nullptr;
    }
    auto* fi_ctx = static_cast<::fi_context2*>(ctx);
    return static_cast<OpState*>(fi_ctx->internal[0]);
}

}  // namespace loom::execution
