// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept completion_queue_type
 * @brief Concept for completion queue types.
 * @tparam T The completion queue type.
 *
 * Requires an event_type alias, poll() returning optional event,
 * and poll_batch() accepting a span of events.
 */
template <typename T>
concept completion_queue_type = requires(T& cq) {
    typename T::event_type;
    { cq.poll() } -> std::same_as<std::optional<typename T::event_type>>;
    {
        cq.poll_batch(std::declval<std::span<typename T::event_type>>())
    } -> std::convertible_to<std::size_t>;
};

}  // namespace loom
