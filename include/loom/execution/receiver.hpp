// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file receiver.hpp
 * @brief P2300 receiver adaptor for loom fabric completions.
 *
 * This header provides the bridge between loom's completion-based async model
 * and P2300's receiver-based model. The fabric_receiver wraps a P2300 receiver
 * and translates fabric completion events into the appropriate set_value,
 * set_error, or set_stopped calls.
 */

#include <rdma/fabric.h>

#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/submission_context.hpp"
#include "loom/execution/concepts.hpp"

namespace loom::execution {

/**
 * @brief Environment type for loom receivers.
 *
 * Provides queryable properties for receivers used with loom senders.
 */
struct fabric_env {
    completion_queue* cq{nullptr};

    /**
     * @brief Query for the completion queue.
     */
    [[nodiscard]] auto query(auto) const noexcept -> completion_queue* { return cq; }
};

/**
 * @brief Wraps a P2300 receiver to handle fabric completions.
 *
 * This class bridges loom's completion event model to P2300's receiver
 * interface. It stores the user's receiver and translates completion
 * events into the appropriate receiver channel calls.
 *
 * @tparam Receiver The wrapped P2300 receiver type.
 * @tparam ValueTypes The value types sent on successful completion.
 */
template <typename Receiver, typename... ValueTypes>
class fabric_receiver {
public:
    using receiver_concept = receiver_t;

    /**
     * @brief Constructs a fabric receiver wrapping the given receiver.
     * @param receiver The P2300 receiver to wrap.
     */
    explicit fabric_receiver(Receiver receiver) noexcept(
        std::is_nothrow_move_constructible_v<Receiver>)
        : receiver_(std::move(receiver)) {
        // Store back-pointer in fi_context for recovery
        fi_ctx_.internal[0] = static_cast<void*>(this);
    }

    fabric_receiver(const fabric_receiver&) = delete;
    auto operator=(const fabric_receiver&) -> fabric_receiver& = delete;
    fabric_receiver(fabric_receiver&&) = default;
    auto operator=(fabric_receiver&&) -> fabric_receiver& = default;
    ~fabric_receiver() = default;

    /**
     * @brief Returns the libfabric context pointer.
     * @return Pointer to the fi_context2 structure.
     */
    [[nodiscard]] auto as_fi_context() noexcept -> void* { return &fi_ctx_; }

    /**
     * @brief Recovers the fabric_receiver from a libfabric context.
     * @param ctx The context pointer from a completion event.
     * @return Pointer to the fabric_receiver, or nullptr if invalid.
     */
    [[nodiscard]] static auto from_fi_context(void* ctx) noexcept -> fabric_receiver* {
        if (ctx == nullptr) {
            return nullptr;
        }
        auto* fi_ctx = static_cast<::fi_context2*>(ctx);
        return static_cast<fabric_receiver*>(fi_ctx->internal[0]);
    }

    /**
     * @brief Called when the fabric operation completes successfully.
     *
     * Translates the completion event to a set_value call on the wrapped
     * receiver with the appropriate value types.
     *
     * @param event The fabric completion event.
     */
    void complete(const completion_event& event) {
        if (event.has_error()) {
            loom::execution::set_error(std::move(receiver_), event.error);
        } else {
            if constexpr (sizeof...(ValueTypes) == 0) {
                loom::execution::set_value(std::move(receiver_));
            } else if constexpr (sizeof...(ValueTypes) == 1) {
                // Single value type - typically bytes_transferred for recv
                using value_type = std::tuple_element_t<0, std::tuple<ValueTypes...>>;
                if constexpr (std::same_as<value_type, std::size_t>) {
                    loom::execution::set_value(std::move(receiver_), event.bytes_transferred);
                } else {
                    loom::execution::set_value(std::move(receiver_), value_type{});
                }
            } else {
                loom::execution::set_value(std::move(receiver_), ValueTypes{}...);
            }
        }
    }

    /**
     * @brief Called when the fabric operation fails.
     * @param ec The error code.
     */
    void fail(std::error_code ec) { loom::execution::set_error(std::move(receiver_), ec); }

    /**
     * @brief Called when the fabric operation is cancelled.
     */
    void cancel() { loom::execution::set_stopped(std::move(receiver_)); }

    /**
     * @brief Implements set_value for loom's completion_receiver concept.
     * @param event The completion event.
     */
    void set_value(completion_event& event) { complete(event); }

    /**
     * @brief Implements set_error for loom's error_receiver concept.
     * @param ec The error code.
     */
    void set_error(std::error_code ec) { fail(ec); }

    /**
     * @brief Implements set_stopped for loom's stoppable_receiver concept.
     */
    void set_stopped() { cancel(); }

    /**
     * @brief Returns the environment for this receiver.
     * @return The fabric environment.
     */
    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return env_; }

    /**
     * @brief Returns a reference to the wrapped receiver.
     * @return Reference to the wrapped receiver.
     */
    [[nodiscard]] auto base() & noexcept -> Receiver& { return receiver_; }

    /**
     * @brief Returns a const reference to the wrapped receiver.
     * @return Const reference to the wrapped receiver.
     */
    [[nodiscard]] auto base() const& noexcept -> const Receiver& { return receiver_; }

    /**
     * @brief Returns an rvalue reference to the wrapped receiver.
     * @return Rvalue reference to the wrapped receiver.
     */
    [[nodiscard]] auto base() && noexcept -> Receiver&& { return std::move(receiver_); }

private:
    ::fi_context2 fi_ctx_{};
    Receiver receiver_;
    fabric_env env_{};
};

/**
 * @brief Deduction guide for fabric_receiver.
 */
template <typename Receiver>
fabric_receiver(Receiver) -> fabric_receiver<Receiver>;

/**
 * @brief Creates a fabric receiver with specified value types.
 * @tparam ValueTypes The value types for successful completion.
 * @param receiver The P2300 receiver to wrap.
 * @return A fabric_receiver wrapping the given receiver.
 */
template <typename... ValueTypes, typename Receiver>
[[nodiscard]] auto make_fabric_receiver(Receiver&& receiver)
    -> fabric_receiver<std::decay_t<Receiver>, ValueTypes...> {
    return fabric_receiver<std::decay_t<Receiver>, ValueTypes...>(std::forward<Receiver>(receiver));
}

}  // namespace loom::execution

// ADL customization points for P2300
namespace loom::execution {

/**
 * @brief ADL customization of get_env for fabric_receiver.
 */
template <typename Receiver, typename... ValueTypes>
[[nodiscard]] auto tag_invoke([[maybe_unused]] decltype(loom::execution::get_env) tag,
                              const fabric_receiver<Receiver, ValueTypes...>& r) noexcept
    -> fabric_env {
    return r.get_env();
}

}  // namespace loom::execution
