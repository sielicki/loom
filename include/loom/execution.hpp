// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file execution.hpp
 * @brief Main header for loom's P2300 std::execution integration.
 *
 * This header provides integration between loom's libfabric bindings and
 * P2300-style sender/receiver async programming. It works with multiple
 * P2300 implementations:
 *
 * - stdexec (NVIDIA's reference implementation)
 * - beman::execution (Beman project implementation)
 * - std::execution (future C++26 standard)
 *
 * The implementation is designed to be generic so that code written against
 * this interface will work with any conforming P2300 implementation with
 * minimal changes when std::execution becomes available in C++26.
 *
 * ## Quick Start
 *
 * ```cpp
 * #include <loom/execution.hpp>
 * #include <stdexec/execution.hpp>  // Or beman::execution
 *
 * // Create senders for fabric operations
 * auto send_op = loom::async(ep).send(buffer, &cq);
 * auto recv_op = loom::async(ep).recv(buffer, &cq);
 *
 * // Compose with P2300 algorithms
 * auto pipeline = send_op
 *               | loom::execution::then([] { std::println("sent!"); });
 *
 * // Parallel operations
 * auto both = loom::execution::when_all(
 *     loom::async(ep).send(buf1, &cq),
 *     loom::async(ep).recv(buf2, &cq)
 * );
 *
 * // RMA operations
 * auto read_op = loom::rma_async(ep).read(buffer, remote_addr, key, &cq);
 * auto write_op = loom::rma_async(ep).write(buffer, remote_addr, key, &cq);
 * ```
 *
 * ## Scheduler
 *
 * ```cpp
 * // Create a scheduler from a completion queue
 * auto cq_ptr = std::make_shared<loom::completion_queue>(std::move(cq));
 * loom::scheduler sched{cq_ptr};
 *
 * // Schedule work on the CQ context
 * auto work = loom::execution::schedule(sched)
 *           | loom::execution::then([] { return 42; });
 * ```
 *
 * ## Backend Detection
 *
 * The backend is automatically detected at compile time. You can query
 * which backend is active:
 *
 * ```cpp
 * if constexpr (loom::execution::has_execution_backend) {
 *     std::println("Using: {}", loom::execution::execution_backend_name);
 * }
 * ```
 *
 * @see loom::execution::concepts.hpp for concept definitions
 * @see loom::execution::send_recv.hpp for messaging senders
 * @see loom::execution::rma.hpp for RMA senders
 * @see loom::execution::scheduler.hpp for the scheduler type
 */

#include "loom/execution/concepts.hpp"
#include "loom/execution/operation_state.hpp"
#include "loom/execution/receiver.hpp"
#include "loom/execution/rma.hpp"
#include "loom/execution/scheduler.hpp"
#include "loom/execution/send_recv.hpp"

/**
 * @namespace loom::execution
 * @brief P2300-compatible execution primitives for loom.
 *
 * This namespace provides types and algorithms for composable async
 * programming with loom's libfabric bindings using the P2300 sender/receiver
 * model.
 *
 * ## Key Types
 *
 * - `scheduler` - Schedules work on a completion queue's context
 * - `send_sender` - Sender for async send operations
 * - `recv_sender` - Sender for async receive operations
 * - `read_sender` - Sender for RMA read operations
 * - `write_sender` - Sender for RMA write operations
 * - `atomic_sender<T>` - Sender for atomic operations
 *
 * ## Factory Functions
 *
 * - `loom::async(endpoint&)` - Creates a sender factory for messaging
 * - `loom::rma_async(endpoint&)` - Creates a sender factory for RMA
 *
 * ## Thread Safety
 *
 * - Senders are thread-safe to compose and connect
 * - Operation states must be started from a single thread
 * - Completion callbacks may be invoked from any thread doing CQ polling
 */
namespace loom::execution {

/**
 * @brief Checks if the execution backend supports a given feature.
 */
struct features {
    /// True if when_all is available
    static constexpr bool has_when_all =
#if defined(LOOM_EXECUTION_BACKEND_NONE)
        false;
#else
        true;
#endif

    /// True if then/let_value are available
    static constexpr bool has_continuations =
#if defined(LOOM_EXECUTION_BACKEND_NONE)
        false;
#else
        true;
#endif

    /// True if sync_wait is available
    static constexpr bool has_sync_wait =
#if defined(LOOM_EXECUTION_BACKEND_NONE)
        false;
#else
        true;
#endif

    /// True if bulk is available
    static constexpr bool has_bulk =
#if defined(LOOM_EXECUTION_BACKEND_NONE)
        false;
#else
        true;
#endif
};

}  // namespace loom::execution
