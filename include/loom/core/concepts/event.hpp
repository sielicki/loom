// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <chrono>
#include <concepts>
#include <optional>
#include <span>

#include "loom/core/concepts/fabric_object.hpp"
#include "loom/core/concepts/result.hpp"
#include "loom/core/crtp/crtp_detection.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept event_like
 * @brief Concept for event types via CRTP detection.
 * @tparam T The type to check.
 *
 * Requires CRTP-tagged event with success(), failed(), and context.
 */
template <typename T>
concept event_like = detail::is_event_v<T> && requires(const T& evt) {
    { evt.success() } noexcept -> std::convertible_to<bool>;
    { evt.failed() } noexcept -> std::convertible_to<bool>;
    { evt.context } -> std::convertible_to<void*>;
};

/**
 * @concept event_like_duck
 * @brief Concept for event types via duck typing.
 * @tparam T The type to check.
 *
 * Checks for event interface without requiring CRTP inheritance.
 */
template <typename T>
concept event_like_duck = requires(const T& evt) {
    { evt.success() } noexcept -> std::convertible_to<bool>;
    { evt.failed() } noexcept -> std::convertible_to<bool>;
    { evt.context } -> std::convertible_to<void*>;
};

/**
 * @concept pollable_queue
 * @brief Concept for queues that support polling.
 * @tparam T The queue type.
 *
 * Requires fabric_object_like interface, event_type alias, and poll() method.
 */
template <typename T>
concept pollable_queue = fabric_object_like<T> && requires(T& q) {
    typename T::event_type;
    { q.poll() } -> std::same_as<std::optional<typename T::event_type>>;
};

/**
 * @concept waitable_queue
 * @brief Concept for queues that support blocking waits.
 * @tparam T The queue type.
 *
 * Extends pollable_queue with wait() method supporting timeout.
 */
template <typename T>
concept waitable_queue =
    pollable_queue<T> && requires(T& q, std::optional<std::chrono::milliseconds> timeout) {
        { q.wait(timeout) } -> result_type;
    };

/**
 * @concept batch_pollable_queue
 * @brief Concept for queues that support batch polling.
 * @tparam T The queue type.
 *
 * Extends pollable_queue with poll_batch() for multiple events.
 */
template <typename T>
concept batch_pollable_queue = pollable_queue<T> && requires(T& q) {
    {
        q.poll_batch(std::declval<std::span<typename T::event_type>>())
    } -> std::convertible_to<std::size_t>;
};

/**
 * @concept fd_waitable
 * @brief Concept for objects that expose a file descriptor for waiting.
 * @tparam T The type to check.
 *
 * Enables integration with epoll, kqueue, poll, or select-based event loops.
 */
template <typename T>
concept fd_waitable = requires(const T& obj) {
    { obj.fd() } noexcept -> std::same_as<int>;
};

/**
 * @concept signal_waitable
 * @brief Concept for objects that can be signaled and waited on.
 * @tparam T The type to check.
 *
 * For condition-variable or eventfd-style synchronization.
 */
template <typename T>
concept signal_waitable = requires(T& obj) {
    { obj.signal() } -> std::same_as<void>;
    { obj.wait() } -> std::same_as<void>;
};

/**
 * @concept timed_waitable
 * @brief Concept for objects supporting timed waits.
 * @tparam T The type to check.
 *
 * Allows waiting with a timeout, returning whether the wait succeeded.
 */
template <typename T>
concept timed_waitable = requires(T& obj, std::chrono::milliseconds timeout) {
    { obj.wait_for(timeout) } -> std::convertible_to<bool>;
};

/**
 * @concept wait_object
 * @brief Concept for any waitable synchronization object.
 * @tparam T The type to check.
 *
 * Satisfied by FD-based, signal-based, or timed waitables.
 */
template <typename T>
concept wait_object = fd_waitable<T> || signal_waitable<T> || timed_waitable<T>;

/**
 * @concept manual_progress
 * @brief Concept for objects requiring manual progress driving.
 * @tparam T The type to check.
 *
 * Objects satisfying this concept need explicit progress() calls to
 * advance communication. Returns the number of events processed.
 */
template <typename T>
concept manual_progress = requires(T& obj) {
    { obj.progress() } -> std::convertible_to<std::size_t>;
};

/**
 * @concept auto_progress
 * @brief Concept for objects with automatic progress.
 * @tparam T The type to check.
 *
 * Objects satisfying this concept make progress automatically
 * (e.g., via background threads or hardware offload).
 */
template <typename T>
concept auto_progress = !manual_progress<T>;

/**
 * @concept progress_engine
 * @brief Concept for progress engine types.
 * @tparam T The type to check.
 *
 * A progress engine can drive progress and reports whether it's running.
 */
template <typename T>
concept progress_engine = requires(T& engine) {
    { engine.run() } -> std::same_as<void>;
    { engine.stop() } -> std::same_as<void>;
    { engine.running() } noexcept -> std::same_as<bool>;
};

/**
 * @concept event_loop_integratable
 * @brief Concept for objects that can integrate with external event loops.
 * @tparam T The type to check.
 *
 * Requires both FD-based waiting and manual progress capability.
 */
template <typename T>
concept event_loop_integratable = fd_waitable<T> && manual_progress<T>;

}  // namespace loom
