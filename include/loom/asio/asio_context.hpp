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

#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "loom/asio/registered_buffer.hpp"
#include "loom/async/completion_queue.hpp"
#include "loom/core/crtp/crtp_tags.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/safe_context.hpp"
#include "loom/core/submission_context.hpp"
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/dispatch.hpp>
#include <asio/execution_context.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>

namespace loom::asio {

namespace detail {
struct asio_buffers {
    std::span<const iovec> iov;
    std::span<const mr_descriptor> descriptors;
    std::size_t bytes{};

    [[nodiscard]] auto within(const memory_region& region) const noexcept -> bool {
        if (!region)
            return false;
        for (const auto& buffer : iov)
            if (!region.contains(buffer.iov_base, buffer.iov_len))
                return false;
        return true;
    }
};

template <typename Allocator = std::allocator<std::byte>>
class asio_buffer_storage {
    using iov_allocator = typename std::allocator_traits<Allocator>::template rebind_alloc<iovec>;
    using descriptor_allocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<mr_descriptor>;

public:
    explicit asio_buffer_storage(const Allocator& allocator = {})
        : buffer_iov_(iov_allocator{allocator}),
          buffer_descriptors_(descriptor_allocator{allocator}) {}

    [[nodiscard]] auto buffers() const noexcept -> asio_buffers {
        return {buffer_iov_, buffer_descriptors_, buffer_bytes_};
    }

    template <bool Mutable, typename BufferSequence>
    void prepare_buffers(const BufferSequence& buffers, std::optional<mr_descriptor> descriptor) {
        using buffer_type =
            std::conditional_t<Mutable, ::asio::mutable_buffer, ::asio::const_buffer>;
        const auto end = ::asio::buffer_sequence_end(buffers);
        for (auto it = ::asio::buffer_sequence_begin(buffers); it != end; ++it) {
            const buffer_type buffer{*it};
            if (buffer.size() > std::numeric_limits<std::size_t>::max() - buffer_bytes_) {
                throw std::length_error("Asio buffer sequence size overflows size_t");
            }
            buffer_iov_.push_back(
                {const_cast<void*>(static_cast<const void*>(buffer.data())), buffer.size()});
            auto local_descriptor = descriptor.value_or(mr_descriptor{});
            using element_type = std::remove_cvref_t<decltype(*it)>;
            if constexpr (std::same_as<element_type, mutable_registered_buffer> ||
                          std::same_as<element_type, const_registered_buffer>) {
                if (!descriptor)
                    local_descriptor = (*it).descriptor();
            }
            if (local_descriptor || !buffer_descriptors_.empty()) {
                // Keep one descriptor per IOV, including unregistered elements
                // before the first registration and between registered ranges.
                buffer_descriptors_.resize(buffer_iov_.size() - 1);
                buffer_descriptors_.push_back(local_descriptor);
            }
            buffer_bytes_ += buffer.size();
        }
    }

private:
    std::vector<iovec, iov_allocator> buffer_iov_;
    std::vector<mr_descriptor, descriptor_allocator> buffer_descriptors_;
    std::size_t buffer_bytes_{};
};

// Pending operations share ownership so their native endpoint and cancellation
// address survive moves and destruction of the public I/O object.
class endpoint_state {
public:
    explicit endpoint_state(loom::endpoint ep = {}) : endpoint(std::move(ep)) {}

    void add(void* context) {
        const std::lock_guard lock{mutex_};
        pending_.push_back(context);
    }

    void remove(void* context) {
        const std::lock_guard lock{mutex_};
        std::erase(pending_, context);
    }

    [[nodiscard]] auto cancel() -> std::error_code {
        const std::lock_guard lock{mutex_};
        std::error_code error;
        for (auto* context : pending_) {
            const auto result = endpoint.cancel(context_ptr<void>{context});
            if (!result && !error) {
                error = result.error();
            }
        }
        return error;
    }

    loom::endpoint endpoint;

private:
    std::mutex mutex_;
    std::vector<void*> pending_;
};
}  // namespace detail

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

    asio_context_base() {
        // The derived object does not exist during base construction.
        store_safe_context(fi_ctx_, this);
    }

    virtual ~asio_context_base() {
        // Invalidate application metadata before releasing the context
        invalidate_safe_context(fi_ctx_);
    }

    asio_context_base(const asio_context_base&) = delete;
    auto operator=(const asio_context_base&) -> asio_context_base& = delete;
    asio_context_base(asio_context_base&&) = delete;
    auto operator=(asio_context_base&&) -> asio_context_base& = delete;

    /**
     * @brief Returns the libfabric context pointer for this context.
     * @return Pointer to the fi_context2 structure.
     */
    [[nodiscard]] auto as_fi_context() noexcept -> void* { return &fi_ctx_.provider; }

    /**
     * @brief Returns the libfabric context pointer (const version).
     * @return Const pointer to the fi_context2 structure.
     */
    [[nodiscard]] auto as_fi_context() const noexcept -> const void* { return &fi_ctx_.provider; }

    /**
     * @brief Recovers the derived context from a libfabric context pointer.
     *
     * ctx must be null or have been returned by as_fi_context on a live object
     * of this context type. Foreign and dangling pointers violate this
     * precondition. Use completion_queue::dispatch to route arbitrary CQ events.
     *
     * @param ctx A context pointer with known ownership.
     * @return Pointer to the derived context, or nullptr for a null input.
     */
    [[nodiscard]] static auto from_fi_context(void* ctx) noexcept -> Derived* {
        return static_cast<Derived*>(recover_safe_context<asio_context_base>(ctx));
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
    safe_context fi_ctx_{};
};

/**
 * @brief Type-erased base for runtime polymorphism when needed.
 */
class asio_context_erased : public asio_context_base<asio_context_erased> {
public:
    asio_context_erased() = default;
    ~asio_context_erased() override {
        if (endpoint_state_) {
            endpoint_state_->remove(as_fi_context());
        }
    }

    asio_context_erased(const asio_context_erased&) = delete;
    auto operator=(const asio_context_erased&) -> asio_context_erased& = delete;
    asio_context_erased(asio_context_erased&&) = delete;
    auto operator=(asio_context_erased&&) -> asio_context_erased& = delete;

    virtual void impl_set_value(completion_event& event) = 0;
    virtual void impl_set_error(std::error_code ec) = 0;
    virtual void impl_set_stopped() = 0;
    virtual void discard() = 0;

    [[nodiscard]] virtual auto buffers() const noexcept -> detail::asio_buffers = 0;

    // Send and local RMA completions do not carry a received-message length.
    void set_transfer_size(std::size_t bytes) noexcept { transfer_size_ = bytes; }

    void retain_endpoint(std::shared_ptr<detail::endpoint_state> state) {
        state->add(as_fi_context());
        endpoint_state_ = std::move(state);
    }

    /// Register before native submission; the queue must outlive accepted work.
    /// Rejection or context destruction removes the registration automatically.
    [[nodiscard]] auto bind_completion(endpoint& ep, completion_direction direction) noexcept
        -> void_result {
        // Asio's handler retires on one event. A multi-receive buffer and
        // context must survive until the separate provider-release event.
        if (direction == completion_direction::receive) {
            const auto* queue = ep.bound_cq(direction);
            if ((queue && queue->format() == cq_format::context) ||
                (ep.impl_operation_flags(direction) & FI_MULTI_RECV)) {
                return make_error_result<void>(errc::not_supported);
            }
        }
        return registration_.bind(
            ep, direction, context_ptr<void>{as_fi_context()}, this, complete_registered);
    }

    [[nodiscard]] auto bind_completion(completion_queue& queue) noexcept -> void_result {
        return registration_.bind(
            queue, context_ptr<void>{as_fi_context()}, this, complete_registered);
    }

protected:
    [[nodiscard]] auto completion_size(const completion_event& event) const noexcept
        -> std::size_t {
        return transfer_size_ ? (event.error ? 0 : *transfer_size_) : event.bytes_transferred;
    }

private:
    static void complete_registered(void* object, const completion_event& event) {
        static_cast<asio_context_erased*>(object)->complete(event);
    }

    std::optional<std::size_t> transfer_size_;
    std::shared_ptr<detail::endpoint_state> endpoint_state_;
    completion_registration registration_;
};

/**
 * @brief Cancellation handler that triggers fi_cancel on the endpoint.
 *
 * This class is installed on the handler's cancellation slot and will
 * call fi_cancel when terminal cancellation is requested. Libfabric does not
 * promise the side-effect guarantees required for partial or total cancellation.
 */
class cancellation_handler {
public:
    cancellation_handler(endpoint* ep, void* ctx) noexcept : ep_(ep), ctx_(ctx) {}

    void operator()(::asio::cancellation_type type) {
        if (ep_ &&
            (type & ::asio::cancellation_type::terminal) != ::asio::cancellation_type::none) {
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
    using iov_allocator =
        typename std::allocator_traits<allocator_type>::template rebind_alloc<iovec>;
    using descriptor_allocator =
        typename std::allocator_traits<allocator_type>::template rebind_alloc<mr_descriptor>;

    /**
     * @brief Constructs a context with a handler and executor.
     * @param handler The completion handler to invoke.
     * @param executor The executor to dispatch on.
     */
    asio_context(Handler handler, Executor executor)
        : handler_(std::move(handler)), executor_(std::move(executor)), work_(executor_),
          allocator_(::asio::get_associated_allocator(handler_)),
          cancellation_slot_(::asio::get_associated_cancellation_slot(handler_)),
          buffers_(allocator_) {}

    /**
     * @brief Constructs a context with a handler, executor, and endpoint for cancellation.
     * @param handler The completion handler to invoke.
     * @param executor The executor to dispatch on.
     * @param ep The endpoint for cancellation support.
     */
    asio_context(Handler handler, Executor executor, endpoint* ep)
        : handler_(std::move(handler)), executor_(std::move(executor)), work_(executor_),
          allocator_(::asio::get_associated_allocator(handler_)),
          cancellation_slot_(::asio::get_associated_cancellation_slot(handler_)), endpoint_(ep),
          buffers_(allocator_) {
        setup_cancellation();
    }

    ~asio_context() override {
        if (endpoint_ && cancellation_slot_.is_connected()) {
            cancellation_slot_.clear();
        }
    }

    void discard() override { destroy_self(); }

    [[nodiscard]] auto buffers() const noexcept -> detail::asio_buffers override {
        return buffers_.buffers();
    }

    template <bool Mutable, typename BufferSequence>
    void prepare_buffers(const BufferSequence& buffers, std::optional<mr_descriptor> descriptor) {
        buffers_.template prepare_buffers<Mutable>(buffers, descriptor);
    }

    /**
     * @brief Implements set_value for the completion_receiver concept.
     * @param event The completion event containing bytes transferred.
     */
    void impl_set_value(completion_event& event) override {
        auto bytes = this->completion_size(event);
        auto ec = event.error;
        auto handler = std::move(handler_);
        auto executor = executor_;
        auto work = std::move(work_);

        destroy_self();

        ::asio::dispatch(
            executor, [handler = std::move(handler), work = std::move(work), ec, bytes]() mutable {
                handler(ec, bytes);
            });
    }

    /**
     * @brief Implements set_error for the error_receiver concept.
     * @param ec The error code.
     */
    void impl_set_error(std::error_code ec) override {
        auto handler = std::move(handler_);
        auto executor = executor_;
        auto work = std::move(work_);

        destroy_self();

        ::asio::post(executor,
                     [handler = std::move(handler), work = std::move(work), ec]() mutable {
                         handler(ec, 0);
                     });
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
    [[nodiscard]] auto get_allocator() const -> allocator_type { return allocator_; }

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
        return cancellation_slot_;
    }

private:
    void setup_cancellation() {
        if (cancellation_slot_.is_connected()) {
            cancellation_slot_.assign(cancellation_handler(endpoint_, this->as_fi_context()));
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
    ::asio::executor_work_guard<Executor> work_;
    allocator_type allocator_;
    cancellation_slot_type cancellation_slot_;
    endpoint* endpoint_{nullptr};
    detail::asio_buffer_storage<allocator_type> buffers_;
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
 * @brief Creates an asio_context with a fallback executor.
 * @tparam Handler The handler type.
 * @tparam Executor The executor type.
 * @param handler The handler to wrap.
 * @param executor Executor used when the handler has no associated executor.
 * @param ep Optional endpoint for cancellation support.
 * @return Pointer to the created context.
 */
template <typename Handler, typename Executor>
[[nodiscard]] auto make_asio_context(Handler&& handler, Executor&& executor, endpoint* ep = nullptr)
    -> asio_context_erased* {
    auto associated = ::asio::get_associated_executor(handler, executor);
    return asio_context<std::decay_t<Handler>, decltype(associated)>::create(
        std::forward<Handler>(handler), std::move(associated), ep);
}

struct asio_context_deleter {
    void operator()(asio_context_erased* context) const { context->discard(); }
};
using owned_asio_context = std::unique_ptr<asio_context_erased, asio_context_deleter>;

template <bool Mutable, typename Handler, typename Executor, typename BufferSequence>
[[nodiscard]] auto
make_buffered_asio_context(Handler&& handler,
                           Executor&& executor,
                           endpoint* ep,
                           const BufferSequence& buffers,
                           std::optional<mr_descriptor> descriptor = std::nullopt)
    -> owned_asio_context {
    auto associated = ::asio::get_associated_executor(handler, executor);
    using context_type = asio_context<std::decay_t<Handler>, decltype(associated)>;
    auto* concrete =
        context_type::create(std::forward<Handler>(handler), std::move(associated), ep);
    owned_asio_context context{concrete};
    concrete->template prepare_buffers<Mutable>(buffers, descriptor);
    return context;
}

inline void finish_asio_submission(owned_asio_context context, void_result result) {
    auto* pending = context.release();
    if (!result) {
        pending->fail(result.error());
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
