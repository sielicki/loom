// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file asio_context.hpp
 * @brief Context bridge between loom completion events and Asio handlers.
 *
 * This file provides the core mechanism for integrating loom's completion-based
 * async model with Asio's handler-based model. The asio_context wraps an Asio
 * completion handler and manages its lifecycle, invoking it when a loom operation
 * completes.
 *
 * The asio_context implements loom's receiver concepts (full_receiver) allowing
 * it to be used with loom's submission_context infrastructure.
 */

#include <rdma/fabric.h>

#include <cstddef>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/crtp/crtp_tags.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/submission_context.hpp"
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/dispatch.hpp>
#include <asio/execution_context.hpp>
#include <asio/post.hpp>

namespace loom::asio {

/**
 * @class asio_context_base
 * @brief CRTP base class for Asio completion context types.
 * @tparam Derived The derived context type.
 *
 * This class provides the common interface for bridging loom completions
 * to Asio handlers. It implements the full_receiver concept required by
 * loom's submission_context infrastructure.
 *
 * The fi_context2 is stored inline and we use a back-pointer approach to
 * recover the context from the libfabric context.
 */
template <typename Derived>
class asio_context_base {
public:
    using crtp_tag = crtp::asio_context_tag;

    asio_context_base() { fi_ctx_.internal[0] = static_cast<void*>(static_cast<Derived*>(this)); }
    virtual ~asio_context_base() = default;

    asio_context_base(const asio_context_base&) = delete;
    auto operator=(const asio_context_base&) -> asio_context_base& = delete;
    asio_context_base(asio_context_base&&) = delete;
    auto operator=(asio_context_base&&) -> asio_context_base& = delete;

    /**
     * @brief Returns the libfabric context pointer for this context.
     * @return Pointer to the fi_context2 structure.
     */
    [[nodiscard]] auto as_fi_context() noexcept -> void* { return &fi_ctx_; }

    /**
     * @brief Returns the libfabric context pointer (const version).
     * @return Const pointer to the fi_context2 structure.
     */
    [[nodiscard]] auto as_fi_context() const noexcept -> const void* { return &fi_ctx_; }

    /**
     * @brief Recovers the derived context from a libfabric context pointer.
     * @param ctx The libfabric context pointer from a completion event.
     * @return Pointer to the derived context, or nullptr if invalid.
     *
     * Uses the internal[0] field of fi_context2 as a back-pointer.
     */
    [[nodiscard]] static auto from_fi_context(void* ctx) noexcept -> Derived* {
        if (ctx == nullptr) {
            return nullptr;
        }
        auto* fi_ctx = static_cast<::fi_context2*>(ctx);
        return static_cast<Derived*>(fi_ctx->internal[0]);
    }

    /**
     * @brief Called when an operation completes with an event.
     *
     * Implements the completion_receiver concept.
     *
     * @param event The completion event from the fabric.
     */
    void set_value(completion_event& event) { static_cast<Derived*>(this)->impl_set_value(event); }

    /**
     * @brief Called when an operation fails.
     *
     * Implements the error_receiver concept.
     *
     * @param ec The error code describing the failure.
     */
    void set_error(std::error_code ec) { static_cast<Derived*>(this)->impl_set_error(ec); }

    /**
     * @brief Called when an operation is cancelled/stopped.
     *
     * Implements the stoppable_receiver concept.
     */
    void set_stopped() { static_cast<Derived*>(this)->impl_set_stopped(); }

    /**
     * @brief Legacy method - called when an operation completes successfully.
     * @param event The completion event containing bytes transferred.
     */
    void complete(const completion_event& event) {
        completion_event mutable_event = event;
        set_value(mutable_event);
    }

    /**
     * @brief Legacy method - called when an operation fails.
     * @param ec The error code.
     */
    void fail(std::error_code ec) { set_error(ec); }

    /**
     * @brief Legacy method - called when an operation is cancelled.
     */
    void cancel() { set_stopped(); }

protected:
    ::fi_context2 fi_ctx_{};
};

/**
 * @brief Type-erased base for runtime polymorphism when needed.
 */
class asio_context_erased : public asio_context_base<asio_context_erased> {
public:
    asio_context_erased() = default;
    ~asio_context_erased() override = default;

    asio_context_erased(const asio_context_erased&) = delete;
    auto operator=(const asio_context_erased&) -> asio_context_erased& = delete;
    asio_context_erased(asio_context_erased&&) = delete;
    auto operator=(asio_context_erased&&) -> asio_context_erased& = delete;

    virtual void impl_set_value(completion_event& event) = 0;
    virtual void impl_set_error(std::error_code ec) = 0;
    virtual void impl_set_stopped() = 0;
};

/**
 * @brief Cancellation handler that triggers fi_cancel on the endpoint.
 *
 * This class is installed on the handler's cancellation slot and will
 * call fi_cancel when cancellation is requested.
 */
class cancellation_handler {
public:
    cancellation_handler(endpoint* ep, void* ctx) noexcept : ep_(ep), ctx_(ctx) {}

    void operator()(::asio::cancellation_type type) {
        if (ep_ && (type != ::asio::cancellation_type::none)) {
            // Attempt to cancel the operation. The result indicates whether
            // fi_cancel succeeded, but we cannot propagate errors from here
            // as this is invoked asynchronously from the cancellation signal.
            // The operation's completion handler will receive the appropriate
            // error code when the cancellation takes effect.
            [[maybe_unused]] auto result = ep_->cancel(context_ptr<void>{ctx_});
        }
    }

private:
    endpoint* ep_;
    void* ctx_;
};

/**
 * @brief Concrete context that wraps an Asio completion handler.
 *
 * This template class stores a completion handler and dispatches completion
 * events to it using the handler's associated executor.
 *
 * Implements the full_receiver concept through the CRTP base class.
 *
 * @tparam Handler The completion handler type, typically with signature
 *                 void(std::error_code, std::size_t).
 * @tparam Executor The executor type to use for dispatching.
 */
template <typename Handler, typename Executor>
class asio_context final : public asio_context_erased {
public:
    using handler_type = Handler;
    using executor_type = Executor;
    using allocator_type = typename ::asio::associated_allocator<Handler>::type;
    using cancellation_slot_type = typename ::asio::associated_cancellation_slot<Handler>::type;

    /**
     * @brief Constructs a context with a handler and executor.
     * @param handler The completion handler to invoke.
     * @param executor The executor to dispatch on.
     */
    asio_context(Handler handler, Executor executor)
        : handler_(std::move(handler)), executor_(std::move(executor)) {}

    /**
     * @brief Constructs a context with a handler, executor, and endpoint for cancellation.
     * @param handler The completion handler to invoke.
     * @param executor The executor to dispatch on.
     * @param ep The endpoint for cancellation support.
     */
    asio_context(Handler handler, Executor executor, endpoint* ep)
        : handler_(std::move(handler)), executor_(std::move(executor)), endpoint_(ep) {
        setup_cancellation();
    }

    /**
     * @brief Implements set_value for the completion_receiver concept.
     * @param event The completion event containing bytes transferred.
     */
    void impl_set_value(completion_event& event) override {
        auto bytes = event.bytes_transferred;
        auto ec = event.error;
        auto handler = std::move(handler_);
        auto executor = executor_;

        destroy_self();

        ::asio::dispatch(
            executor, [handler = std::move(handler), ec, bytes]() mutable { handler(ec, bytes); });
    }

    /**
     * @brief Implements set_error for the error_receiver concept.
     * @param ec The error code.
     */
    void impl_set_error(std::error_code ec) override {
        auto handler = std::move(handler_);
        auto executor = executor_;

        destroy_self();

        ::asio::dispatch(executor,
                         [handler = std::move(handler), ec]() mutable { handler(ec, 0); });
    }

    /**
     * @brief Implements set_stopped for the stoppable_receiver concept.
     */
    void impl_set_stopped() override {
        impl_set_error(std::make_error_code(std::errc::operation_canceled));
    }

    /**
     * @brief Returns the associated executor.
     * @return The executor.
     */
    [[nodiscard]] auto get_executor() const -> executor_type { return executor_; }

    /**
     * @brief Returns the associated allocator.
     * @return The allocator.
     */
    [[nodiscard]] auto get_allocator() const -> allocator_type {
        return ::asio::get_associated_allocator(handler_);
    }

    /**
     * @brief Allocates and constructs a new asio_context.
     * @tparam H Handler type (deduced).
     * @tparam E Executor type (deduced).
     * @param handler The handler to wrap.
     * @param executor The executor to use.
     * @param ep Optional endpoint for cancellation support.
     * @return Pointer to the new context.
     */
    template <typename H, typename E>
    [[nodiscard]] static auto create(H&& handler, E&& executor, endpoint* ep = nullptr)
        -> asio_context* {
        using context_type = asio_context<std::decay_t<H>, std::decay_t<E>>;
        using alloc_type = typename ::asio::associated_allocator<std::decay_t<H>>::type;
        using rebound_alloc =
            typename std::allocator_traits<alloc_type>::template rebind_alloc<context_type>;

        alloc_type handler_alloc = ::asio::get_associated_allocator(handler);
        rebound_alloc alloc(handler_alloc);

        auto* ptr = std::allocator_traits<rebound_alloc>::allocate(alloc, 1);
        try {
            if (ep) {
                std::allocator_traits<rebound_alloc>::construct(
                    alloc, ptr, std::forward<H>(handler), std::forward<E>(executor), ep);
            } else {
                std::allocator_traits<rebound_alloc>::construct(
                    alloc, ptr, std::forward<H>(handler), std::forward<E>(executor));
            }
            return ptr;
        } catch (...) {
            std::allocator_traits<rebound_alloc>::deallocate(alloc, ptr, 1);
            throw;
        }
    }

    /**
     * @brief Returns the associated cancellation slot.
     * @return The cancellation slot.
     */
    [[nodiscard]] auto get_cancellation_slot() const noexcept -> cancellation_slot_type {
        return ::asio::get_associated_cancellation_slot(handler_);
    }

private:
    void setup_cancellation() {
        auto slot = ::asio::get_associated_cancellation_slot(handler_);
        if (slot.is_connected()) {
            slot.assign(cancellation_handler(endpoint_, this->as_fi_context()));
        }
    }

    void destroy_self() {
        using context_type = asio_context<Handler, Executor>;
        using alloc_type = typename ::asio::associated_allocator<Handler>::type;
        using rebound_alloc =
            typename std::allocator_traits<alloc_type>::template rebind_alloc<context_type>;

        rebound_alloc alloc(get_allocator());
        auto* self = this;
        std::allocator_traits<rebound_alloc>::destroy(alloc, self);
        std::allocator_traits<rebound_alloc>::deallocate(alloc, self, 1);
    }

    Handler handler_;
    Executor executor_;
    endpoint* endpoint_{nullptr};
};

/**
 * @brief Creates an asio_context for a handler using its associated executor.
 * @tparam Handler The handler type.
 * @param handler The handler to wrap.
 * @param ep Optional endpoint for cancellation support.
 * @return Pointer to the created context.
 */
template <typename Handler>
[[nodiscard]] auto make_asio_context(Handler&& handler, endpoint* ep = nullptr)
    -> asio_context_erased* {
    auto executor = ::asio::get_associated_executor(handler);
    return asio_context<std::decay_t<Handler>, decltype(executor)>::create(
        std::forward<Handler>(handler), std::move(executor), ep);
}

/**
 * @brief Creates an asio_context with an explicit executor.
 * @tparam Handler The handler type.
 * @tparam Executor The executor type.
 * @param handler The handler to wrap.
 * @param executor The executor to use.
 * @param ep Optional endpoint for cancellation support.
 * @return Pointer to the created context.
 */
template <typename Handler, typename Executor>
[[nodiscard]] auto make_asio_context(Handler&& handler, Executor&& executor, endpoint* ep = nullptr)
    -> asio_context_erased* {
    return asio_context<std::decay_t<Handler>, std::decay_t<Executor>>::create(
        std::forward<Handler>(handler), std::forward<Executor>(executor), ep);
}

/**
 * @brief Dispatches a completion event to the appropriate handler.
 * @param ctx The context pointer from the completion event.
 * @param event The completion event.
 *
 * Uses loom's receiver pattern (set_value/set_error) for dispatch.
 */
inline void dispatch_asio_completion(void* ctx, const completion_event& event) {
    if (auto* asio_ctx = asio_context_erased::from_fi_context(ctx)) {
        completion_event mutable_event = event;
        if (mutable_event.has_error()) {
            asio_ctx->set_error(mutable_event.error);
        } else {
            asio_ctx->set_value(mutable_event);
        }
    }
}

}  // namespace loom::asio

// Asio associated type specializations for asio_context
namespace asio {

/**
 * @brief Specialization of associated_executor for asio_context.
 */
template <typename Handler, typename Executor, typename Executor1>
struct associated_executor<loom::asio::asio_context<Handler, Executor>, Executor1> {
    using type = Executor;

    static auto get(const loom::asio::asio_context<Handler, Executor>& ctx,
                    const Executor1& = Executor1()) noexcept -> type {
        return ctx.get_executor();
    }
};

/**
 * @brief Specialization of associated_allocator for asio_context.
 */
template <typename Handler, typename Executor, typename Allocator>
struct associated_allocator<loom::asio::asio_context<Handler, Executor>, Allocator> {
    using type = typename loom::asio::asio_context<Handler, Executor>::allocator_type;

    static auto get(const loom::asio::asio_context<Handler, Executor>& ctx,
                    const Allocator& = Allocator()) noexcept -> type {
        return ctx.get_allocator();
    }
};

/**
 * @brief Specialization of associated_cancellation_slot for asio_context.
 */
template <typename Handler, typename Executor, typename CancellationSlot>
struct associated_cancellation_slot<loom::asio::asio_context<Handler, Executor>, CancellationSlot> {
    using type = typename loom::asio::asio_context<Handler, Executor>::cancellation_slot_type;

    static auto get(const loom::asio::asio_context<Handler, Executor>& ctx,
                    const CancellationSlot& = CancellationSlot()) noexcept -> type {
        return ctx.get_cancellation_slot();
    }
};

}  // namespace asio
