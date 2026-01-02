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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
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
 */
/**
 * @brief Configuration options for the fabric service.
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
     */
    void register_cq(completion_queue& cq, const options& opts = options{});

    /**
     * @brief Unregisters a completion queue.
     * @param cq The completion queue to unregister.
     */
    void deregister_cq(completion_queue& cq);

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
     * @return The number of completions processed.
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
    void process_completions(completion_queue& cq, std::size_t max_count);

    struct cq_entry {
        completion_queue* cq;
        options opts;
    };

    ::asio::io_context& io_context_;
    std::unique_ptr<::asio::steady_timer> poll_timer_;
    std::vector<cq_entry> completion_queues_;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
};

// Implementation

inline fabric_service::fabric_service(::asio::execution_context& ctx)
    : ::asio::execution_context::service(ctx), io_context_(static_cast<::asio::io_context&>(ctx)),
      poll_timer_(std::make_unique<::asio::steady_timer>(io_context_)) {}

inline fabric_service::~fabric_service() {
    shutdown();
}

inline void fabric_service::shutdown() {
    shutdown_.store(true, std::memory_order_release);
    stop();
}

inline void fabric_service::notify_fork(::asio::execution_context::fork_event event) {
    switch (event) {
        case ::asio::execution_context::fork_prepare:
            // Stop polling before fork to avoid issues with shared state
            stop();
            break;

        case ::asio::execution_context::fork_parent:
            // Parent can resume polling after fork
            if (!shutdown_.load(std::memory_order_acquire)) {
                start();
            }
            break;

        case ::asio::execution_context::fork_child:
            // Child process: clear completion queues as they're inherited but
            // the underlying fabric resources may not be valid across fork.
            // Applications should re-register CQs after fork in the child.
            {
                std::lock_guard lock(mutex_);
                completion_queues_.clear();
            }
            // Recreate the timer for the child process
            poll_timer_ = std::make_unique<::asio::steady_timer>(io_context_);
            break;
    }
}

inline void fabric_service::register_cq(completion_queue& cq, const options& opts) {
    std::lock_guard lock(mutex_);
    completion_queues_.push_back({&cq, opts});

    if (running_.load(std::memory_order_acquire) && completion_queues_.size() == 1) {
        schedule_poll();
    }
}

inline void fabric_service::deregister_cq(completion_queue& cq) {
    std::lock_guard lock(mutex_);
    std::erase_if(completion_queues_, [&cq](const cq_entry& entry) { return entry.cq == &cq; });
}

inline void fabric_service::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    schedule_poll();
}

inline void fabric_service::stop() {
    running_.store(false, std::memory_order_release);
    if (poll_timer_) {
        poll_timer_->cancel();
    }
}

inline auto fabric_service::is_running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
}

inline auto fabric_service::poll_once() -> std::size_t {
    std::size_t total = 0;

    std::vector<cq_entry> cqs_copy;
    {
        std::lock_guard lock(mutex_);
        cqs_copy = completion_queues_;
    }

    for (const auto& entry : cqs_copy) {
        process_completions(*entry.cq, entry.opts.max_completions_per_poll);
    }

    return total;
}

inline void fabric_service::schedule_poll() {
    if (!running_.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire)) {
        return;
    }

    std::chrono::microseconds interval{100};
    {
        std::lock_guard lock(mutex_);
        if (!completion_queues_.empty()) {
            interval = completion_queues_.front().opts.poll_interval;
        }
    }

    poll_timer_->expires_after(interval);
    poll_timer_->async_wait([this](const std::error_code& ec) {
        if (!ec && running_.load(std::memory_order_acquire)) {
            do_poll();
            schedule_poll();
        }
    });
}

inline void fabric_service::do_poll() {
    std::vector<cq_entry> cqs_copy;
    {
        std::lock_guard lock(mutex_);
        cqs_copy = completion_queues_;
    }

    for (const auto& entry : cqs_copy) {
        process_completions(*entry.cq, entry.opts.max_completions_per_poll);
    }
}

inline void fabric_service::process_completions(completion_queue& cq, std::size_t max_count) {
    for (std::size_t i = 0; i < max_count; ++i) {
        auto event_opt = cq.poll();
        if (!event_opt) {
            break;
        }

        const auto& event = *event_opt;
        dispatch_asio_completion(event.context.get(), event);
    }
}

inline auto fabric_service::use(::asio::io_context& ioc) -> fabric_service& {
    if (!::asio::has_service<fabric_service>(ioc)) {
        ::asio::make_service<fabric_service>(ioc);
    }
    return ::asio::use_service<fabric_service>(ioc);
}

inline auto fabric_service::has_service(::asio::io_context& ioc) -> bool {
    return ::asio::has_service<fabric_service>(ioc);
}

}  // namespace loom::asio
