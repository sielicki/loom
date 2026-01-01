// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include "loom/async/completion_queue.hpp"
#include "loom/async/rma.hpp"
#include "loom/async/run_loop.hpp"
#include "loom/async/scheduler.hpp"
#include "loom/async/send_recv.hpp"

namespace loom::async_ops = loom::async;

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @brief Synchronously waits for a sender to complete.
 * @tparam Sender The sender type.
 * @param sender The sender to wait on.
 * @return The result of the sender.
 */
template <stdexec::sender Sender>
auto sync_wait_async(Sender&& sender) {
    return stdexec::sync_wait(std::forward<Sender>(sender));
}

/**
 * @brief Starts a sender and detaches it.
 * @tparam Sender The sender type.
 * @param sender The sender to start.
 */
template <stdexec::sender Sender>
void start_detached_async(Sender&& sender) {
    stdexec::start_detached(std::forward<Sender>(sender));
}

/**
 * @brief Runs a sender on a scheduler.
 * @tparam Sender The sender type.
 * @param sched The scheduler to run on.
 * @param sender The sender to run.
 * @return A new sender that runs on the scheduler.
 */
template <stdexec::sender Sender>
auto on_async(scheduler sched, Sender&& sender) {
    return stdexec::on(sched, std::forward<Sender>(sender));
}

/**
 * @brief Transfers completion to another scheduler.
 * @tparam Sender The sender type.
 * @param sender The sender to transfer.
 * @param to_sched The target scheduler.
 * @return A new sender that transfers to the scheduler.
 */
template <stdexec::sender Sender>
auto transfer_async(Sender&& sender, scheduler to_sched) {
    return std::forward<Sender>(sender) | stdexec::transfer(to_sched);
}

}  // namespace loom
