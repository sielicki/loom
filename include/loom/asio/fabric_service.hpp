// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file fabric_service.hpp
 * @brief Asio execution_context service for managing fabric completion queue polling.
 *
 * The fabric_service integrates loom completion queues with Asio's io_context,
 * enabling automatic polling and dispatch of fabric completions within the
 * io_context run loop.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "loom/asio/asio_context.hpp"
#include "loom/async/completion_queue.hpp"
#include <asio/any_io_executor.hpp>
#include <asio/execution_context.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

namespace loom::asio {

/**
 * @brief Service that manages fabric completion queue polling within an io_context.
 *
 * This service provides integration between loom's completion-based async model
 * and Asio's reactor pattern. It supports two modes of operation:
 *
 * 1. Timer-based polling: Periodically polls completion queues using a steady_timer
 * 2. FD-based integration: Uses the CQ's file descriptor with Asio's reactor (if supported)
 *
 * The service is registered per io_context and can manage multiple completion queues.
 * Service calls and completion dispatch are serialized. Inline callbacks may reenter
 * the service, but must not wait for another thread to call this same service.
 * External submissions and progress must still obey the provider's threading rules.
 */
/**
 * @brief Configuration options for the fabric service.
 * The shared polling timer uses the shortest registered queue interval.
 */
struct fabric_service_options {
    std::chrono::microseconds poll_interval{100};  ///< Timer-based poll interval
    std::size_t max_completions_per_poll{64};      ///< Max completions per poll cycle
    bool use_fd_polling{true};                     ///< Try to use FD-based polling if available
};

class fabric_service : public ::asio::execution_context::service {
public:
    using options = fabric_service_options;

    /// Required by Asio's service model
    static inline ::asio::execution_context::id id;

    /**
     * @brief Constructs the fabric service.
     * @param ctx The execution context (io_context).
     */
    explicit fabric_service(::asio::execution_context& ctx);

    /**
     * @brief Destroys the fabric service.
     */
    ~fabric_service() override;

    fabric_service(const fabric_service&) = delete;
    auto operator=(const fabric_service&) -> fabric_service& = delete;
    fabric_service(fabric_service&&) = delete;
    auto operator=(fabric_service&&) -> fabric_service& = delete;

    /**
     * @brief Registers a completion queue for polling.
     * @param cq The completion queue to register.
     * @param opts Configuration options.
     * The queue must remain at the same address until deregistration completes.
     * Registering an already registered queue leaves its existing options intact.
     */
    void register_cq(completion_queue& cq, const options& opts = options{});

    /**
     * @brief Unregisters a completion queue.
     * @param cq The completion queue to unregister.
     * Waits for polling in other threads to finish. An inline callback may
     * deregister its own queue; the service will not read that queue again.
     * @return Any retained unclaimed event, transferred to the caller.
     */
    [[nodiscard]] auto deregister_cq(completion_queue& cq) -> std::optional<completion_event>;

    /**
     * @brief Takes an event with no registered completion owner.
     * The service preserves its metadata without interpreting its context and
     * pauses reads from that queue until this call consumes the event. Other
     * queues continue progressing. The caller must handle the returned event.
     */
    [[nodiscard]] auto take_unhandled(completion_queue& cq) -> std::optional<completion_event>;

    /**
     * @brief Starts polling for the registered completion queues.
     *
     * This begins the async polling loop that integrates with io_context::run().
     */
    void start();

    /**
     * @brief Stops polling.
     */
    void stop();

    /**
     * @brief Checks if the service is currently polling.
     * @return True if polling is active.
     */
    [[nodiscard]] auto is_running() const noexcept -> bool;

    /**
     * @brief Manually polls all registered completion queues once.
     *
     * This is useful for manual progress when not using the timer-based loop.
     * @return The number of registered completions dispatched. Unclaimed events
     * remain available through take_unhandled and do not contribute to the count.
     */
    auto poll_once() -> std::size_t;

    /**
     * @brief Gets the fabric_service from an io_context, creating it if needed.
     * @param ioc The io_context.
     * @return Reference to the fabric_service.
     */
    static auto use(::asio::io_context& ioc) -> fabric_service&;

    /**
     * @brief Checks if an io_context has a fabric_service.
     * @param ioc The io_context to check.
     * @return True if the service exists.
     */
    static auto has_service(::asio::io_context& ioc) -> bool;

private:
    void shutdown() override;
    void notify_fork(::asio::execution_context::fork_event event) override;

    void schedule_poll();
    void do_poll();
    struct cq_entry {
        completion_queue* cq;
        options opts;
        std::optional<completion_event> unhandled;
    };
    struct poll_scope {
        fabric_service& owner;
        explicit poll_scope(fabric_service& service) noexcept : owner(service) {
            ++owner.poll_depth_;
        }
        ~poll_scope() {
            if (--owner.poll_depth_ == 0 && owner.removed_queues_) {
                std::erase_if(owner.completion_queues_,
                              [](const auto& entry) { return entry->cq == nullptr; });
                owner.removed_queues_ = false;
            }
        }
        poll_scope(const poll_scope&) = delete;
        auto operator=(const poll_scope&) -> poll_scope& = delete;
    };
    auto process_completions(cq_entry& entry) -> std::size_t;

    ::asio::io_context& io_context_;
    std::unique_ptr<::asio::steady_timer> poll_timer_;
    std::vector<std::unique_ptr<cq_entry>> completion_queues_;
    // Native reads and dispatch share this lock with deregistration. Recursion
    // permits inline handlers to mutate registration. Keep entry positions
    // stable until every nested poll returns, including during exception unwinding.
    std::recursive_mutex mutex_;
    std::size_t poll_depth_{};
    bool removed_queues_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
    std::uint64_t poll_generation_{};
    bool resume_after_fork_{};
};

// Implementation

inline fabric_service::fabric_service(::asio::execution_context& ctx)
    : ::asio::execution_context::service(ctx), io_context_(static_cast<::asio::io_context&>(ctx)),
      poll_timer_(std::make_unique<::asio::steady_timer>(io_context_)) {}

inline fabric_service::~fabric_service() {
    shutdown();
}

inline void fabric_service::shutdown() {
    const std::lock_guard lock{mutex_};
    shutdown_.store(true, std::memory_order_release);
    stop();
}

inline void fabric_service::notify_fork(::asio::execution_context::fork_event event) {
    const std::lock_guard lock{mutex_};
    switch (event) {
        case ::asio::execution_context::fork_prepare:
            // Stop polling before fork to avoid issues with shared state
            resume_after_fork_ = is_running();
            stop();
            break;

        case ::asio::execution_context::fork_parent:
            // Parent can resume polling after fork
            if (std::exchange(resume_after_fork_, false) &&
                !shutdown_.load(std::memory_order_acquire)) {
                start();
            }
            break;

        case ::asio::execution_context::fork_child:
            resume_after_fork_ = false;
            // Child process: clear completion queues as they're inherited but
            // the underlying fabric resources may not be valid across fork.
            // Applications should re-register CQs after fork in the child.
            {
                std::lock_guard lock(mutex_);
                if (poll_depth_) {
                    for (auto& entry : completion_queues_)
                        entry->cq = nullptr;
                    removed_queues_ = true;
                } else {
                    completion_queues_.clear();
                }
            }
            // Recreate the timer for the child process
            poll_timer_ = std::make_unique<::asio::steady_timer>(io_context_);
            break;
    }
}

inline void fabric_service::register_cq(completion_queue& cq, const options& opts) {
    const std::lock_guard lock{mutex_};
    if (std::ranges::any_of(completion_queues_,
                            [&cq](const auto& entry) { return entry->cq == &cq; })) {
        return;
    }
    const bool faster = std::ranges::none_of(completion_queues_, [&](const auto& entry) {
        return entry->cq && entry->opts.poll_interval <= opts.poll_interval;
    });
    completion_queues_.push_back(std::make_unique<cq_entry>(&cq, opts, std::nullopt));
    if (running_.load(std::memory_order_acquire) && faster) {
        schedule_poll();
    }
}

inline auto fabric_service::deregister_cq(completion_queue& cq) -> std::optional<completion_event> {
    const std::lock_guard lock{mutex_};
    auto found = std::ranges::find_if(completion_queues_,
                                      [&cq](const auto& entry) { return entry->cq == &cq; });
    if (found == completion_queues_.end()) {
        return std::nullopt;
    }
    (*found)->cq = nullptr;
    auto unhandled = std::exchange((*found)->unhandled, std::nullopt);
    if (poll_depth_) {
        removed_queues_ = true;
    } else {
        completion_queues_.erase(found);
    }
    return unhandled;
}

inline auto fabric_service::take_unhandled(completion_queue& cq)
    -> std::optional<completion_event> {
    const std::lock_guard lock{mutex_};
    auto found = std::ranges::find_if(completion_queues_,
                                      [&cq](const auto& entry) { return entry->cq == &cq; });
    return found == completion_queues_.end() ? std::nullopt
                                             : std::exchange((*found)->unhandled, std::nullopt);
}

inline void fabric_service::start() {
    const std::lock_guard lock{mutex_};
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    schedule_poll();
}

inline void fabric_service::stop() {
    const std::lock_guard lock{mutex_};
    running_.store(false, std::memory_order_release);
    ++poll_generation_;
    if (poll_timer_) {
        poll_timer_->cancel();
    }
}

inline auto fabric_service::is_running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
}

inline auto fabric_service::poll_once() -> std::size_t {
    const std::lock_guard lock{mutex_};
    const poll_scope polling{*this};
    // New registrations wait for a subsequent poll. Callbacks may grow the
    // vector, but existing entry objects and their positions remain stable.
    const auto count = completion_queues_.size();
    std::size_t total{};
    for (std::size_t i = 0; i < count; ++i) {
        total += process_completions(*completion_queues_[i]);
    }
    return total;
}

inline void fabric_service::schedule_poll() {
    const std::lock_guard lock{mutex_};
    if (!running_.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire)) {
        return;
    }

    std::optional<std::chrono::microseconds> interval;
    for (const auto& entry : completion_queues_) {
        if (entry->cq && (!interval || entry->opts.poll_interval < *interval))
            interval = entry->opts.poll_interval;
    }

    const auto generation = ++poll_generation_;
    try {
        poll_timer_->expires_after(interval.value_or(std::chrono::microseconds{100}));
        poll_timer_->async_wait([this, generation](const std::error_code& ec) {
            const std::lock_guard lock{mutex_};
            if (ec || !running_.load(std::memory_order_acquire) || generation != poll_generation_) {
                return;
            }
            // Keep progress scheduled if a registered callback throws. Stop or
            // restart from that callback can invalidate this pending wait.
            schedule_poll();
            do_poll();
        });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }
}

inline void fabric_service::do_poll() {
    poll_once();
}

inline auto fabric_service::process_completions(cq_entry& entry) -> std::size_t {
    if (entry.unhandled) {
        return 0;
    }
    std::size_t count{};
    while (entry.cq && !entry.unhandled && count < entry.opts.max_completions_per_poll) {
        auto event = entry.cq->poll();
        if (!event) {
            break;
        }
        if (!entry.cq->dispatch(*event)) {
            entry.unhandled = std::move(event);
            break;
        }
        ++count;
    }
    return count;
}

inline auto fabric_service::use(::asio::io_context& ioc) -> fabric_service& {
    return ::asio::use_service<fabric_service>(ioc);
}

inline auto fabric_service::has_service(::asio::io_context& ioc) -> bool {
    return ::asio::has_service<fabric_service>(ioc);
}

}  // namespace loom::asio
