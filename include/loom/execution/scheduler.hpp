// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file scheduler.hpp
 * @brief P2300 scheduler for loom completion queue-based execution.
 *
 * This header provides a scheduler type that integrates loom's completion
 * queue polling with the P2300 scheduler concept. The scheduler allows
 * work to be scheduled on a completion queue's execution context.
 *
 * ## Usage
 *
 * ```cpp
 * #include <loom/execution.hpp>
 *
 * // Create a scheduler from a completion queue
 * auto cq_ptr = std::make_shared<loom::completion_queue>(cq);
 * loom::scheduler sched{cq_ptr};
 *
 * // Schedule work on the completion queue's context
 * auto work = loom::execution::schedule(sched)
 *           | loom::execution::then([] { return 42; });
 * ```
 */

#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/execution/concepts.hpp"

namespace loom {

// Forward declaration
class scheduler;

namespace execution {

/**
 * @brief Sender returned by scheduler::schedule().
 *
 * This sender represents work that will execute on the scheduler's
 * execution context (completion queue polling thread).
 */
class schedule_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(), set_error_t(std::error_code), set_stopped_t()>;

    explicit schedule_sender(std::shared_ptr<completion_queue> cq) noexcept : cq_(std::move(cq)) {}

    /**
     * @brief Operation state for schedule sender.
     */
    template <typename Receiver>
    class operation_state {
    public:
        operation_state(std::shared_ptr<completion_queue> cq, Receiver receiver) noexcept
            : cq_(std::move(cq)), receiver_(std::move(receiver)) {}

        operation_state(const operation_state&) = delete;
        auto operator=(const operation_state&) -> operation_state& = delete;
        operation_state(operation_state&&) = delete;
        auto operator=(operation_state&&) -> operation_state& = delete;
        ~operation_state() = default;

        /**
         * @brief Starts the scheduled work.
         *
         * For a schedule sender, this immediately completes with set_value
         * as the work is "scheduled" to run inline.
         */
        void start() & noexcept {
            // In a more complete implementation, this would post work
            // to a thread pool or completion queue polling loop.
            // For now, we complete inline.
            loom::execution::set_value(std::move(receiver_));
        }

    private:
        std::shared_ptr<completion_queue> cq_;
        Receiver receiver_;
    };

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept -> operation_state<Receiver> {
        return operation_state<Receiver>(cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_.get()}; }

    /**
     * @brief Returns the completion scheduler for set_value.
     */
    [[nodiscard]] auto get_completion_scheduler() const noexcept -> loom::scheduler;

private:
    std::shared_ptr<completion_queue> cq_;
};

}  // namespace execution

/**
 * @brief P2300-compatible scheduler for loom completion queues.
 *
 * This scheduler allows work to be scheduled on the execution context
 * associated with a loom completion queue. It satisfies the P2300
 * scheduler concept.
 *
 * ## Usage
 *
 * ```cpp
 * // Create from a shared_ptr to a completion queue
 * auto cq_ptr = std::make_shared<loom::completion_queue>(std::move(cq));
 * loom::scheduler sched{cq_ptr};
 *
 * // Use with P2300 algorithms
 * auto work = loom::execution::starts_on(sched, some_sender);
 * ```
 */
class scheduler {
public:
    /**
     * @brief Default constructor creates an empty scheduler.
     */
    scheduler() noexcept = default;

    /**
     * @brief Constructs a scheduler from a shared completion queue.
     * @param cq Shared pointer to the completion queue.
     */
    explicit scheduler(std::shared_ptr<completion_queue> cq) noexcept : cq_(std::move(cq)) {}

    /**
     * @brief Copy constructor.
     */
    scheduler(const scheduler&) = default;

    /**
     * @brief Copy assignment.
     */
    auto operator=(const scheduler&) -> scheduler& = default;

    /**
     * @brief Move constructor.
     */
    scheduler(scheduler&&) noexcept = default;

    /**
     * @brief Move assignment.
     */
    auto operator=(scheduler&&) noexcept -> scheduler& = default;

    ~scheduler() = default;

    /**
     * @brief Schedules work on this scheduler.
     * @return A sender that completes on this scheduler's execution context.
     */
    [[nodiscard]] auto schedule() const noexcept -> execution::schedule_sender {
        return execution::schedule_sender(cq_);
    }

    /**
     * @brief Equality comparison.
     *
     * Two schedulers are equal if they refer to the same completion queue.
     */
    [[nodiscard]] auto operator==(const scheduler& other) const noexcept -> bool {
        return cq_ == other.cq_;
    }

    /**
     * @brief Inequality comparison.
     */
    [[nodiscard]] auto operator!=(const scheduler& other) const noexcept -> bool {
        return !(*this == other);
    }

    /**
     * @brief Returns a pointer to the underlying completion queue.
     */
    [[nodiscard]] auto get_completion_queue() const noexcept -> completion_queue* {
        return cq_.get();
    }

private:
    std::shared_ptr<completion_queue> cq_;
};

namespace execution {

inline auto schedule_sender::get_completion_scheduler() const noexcept -> loom::scheduler {
    return loom::scheduler{cq_};
}

}  // namespace execution

}  // namespace loom

// Tag invoke customization points for scheduler
namespace loom::execution {

/**
 * @brief Customization of schedule for loom::scheduler.
 */
template <typename Tag>
    requires std::same_as<Tag, decltype(loom::execution::schedule)>
[[nodiscard]] inline auto tag_invoke([[maybe_unused]] Tag tag,
                                     const loom::scheduler& sched) noexcept -> schedule_sender {
    return sched.schedule();
}

/**
 * @brief Customization of get_completion_scheduler for schedule_sender.
 */
template <typename Tag>
    requires std::same_as<Tag, decltype(loom::execution::get_completion_scheduler)>
[[nodiscard]] inline auto tag_invoke([[maybe_unused]] Tag tag,
                                     [[maybe_unused]] set_value_t,
                                     const schedule_sender& sender) noexcept -> loom::scheduler {
    return sender.get_completion_scheduler();
}

}  // namespace loom::execution
