// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file scheduler.hpp
 * @brief P2300 inline scheduler retaining a loom completion queue.
 *
 * This scheduler completes inline on the thread calling start. It retains
 * a completion queue but does not drive progress or dispatch work to a
 * separate CQ polling thread.
 *
 * ## Usage
 *
 * ```cpp
 * #include <loom/execution.hpp>
 *
 * // Create a scheduler from a completion queue
 * auto cq_ptr = std::make_shared<loom::completion_queue>(std::move(cq));
 * loom::scheduler sched{cq_ptr};
 *
 * // Run work inline when started
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
 * This sender completes inline on the thread calling start.
 */
class schedule_sender {
public:
    using sender_concept = sender_tag;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(), set_error_t(std::error_code), set_stopped_t()>;

    [[nodiscard]] static consteval auto get_completion_signatures() noexcept
        -> completion_signatures {
        return {};
    }

    explicit schedule_sender(std::shared_ptr<completion_queue> cq) noexcept : cq_(std::move(cq)) {}

    /**
     * @brief Operation state for schedule sender.
     */
    template <typename Receiver>
    class operation_state {
    public:
        using operation_state_concept = execution::operation_state_tag;
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

    struct environment {
        std::shared_ptr<completion_queue> cq;

        [[nodiscard]] auto
        query(decltype(loom::execution::get_completion_scheduler<set_value_t>)) const noexcept
            -> loom::scheduler;
    };

    [[nodiscard]] auto get_env() const noexcept -> environment { return {cq_}; }

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
 * This scheduler retains a completion queue and satisfies the P2300
 * scheduler concept. Scheduled work runs inline on the initiating thread.
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
    using scheduler_concept = execution::scheduler_tag;
    /**
     * @brief Default constructor creates an empty scheduler.
     */
    scheduler() noexcept = default;

    [[nodiscard]] static constexpr auto
    query(decltype(execution::get_forward_progress_guarantee)) noexcept
        -> execution::forward_progress_guarantee {
        return execution::forward_progress_guarantee::weakly_parallel;
    }

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
     * @return A sender that completes inline when started.
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

inline auto schedule_sender::environment::query(
    decltype(loom::execution::get_completion_scheduler<set_value_t>)) const noexcept
    -> loom::scheduler {
    return loom::scheduler{cq};
}

inline auto schedule_sender::get_completion_scheduler() const noexcept -> loom::scheduler {
    return loom::scheduler{cq_};
}

}  // namespace execution

}  // namespace loom
