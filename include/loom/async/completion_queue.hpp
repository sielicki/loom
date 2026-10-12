// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/event_base.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/progress_policy.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct completion_queue_impl
 * @brief Internal implementation for completion_queue.
 */
struct completion_queue_impl;

/// Stable, non-owning endpoint binding, detached when either resource dies.
/// Queue and endpoint wrappers may move without relocating their implementations.
struct completion_queue_binding {
    completion_queue_binding() noexcept = default;
    ~completion_queue_binding();
    completion_queue_binding(const completion_queue_binding&) = delete;
    auto operator=(const completion_queue_binding&) -> completion_queue_binding& = delete;
    completion_queue_binding(completion_queue_binding&&) = delete;
    auto operator=(completion_queue_binding&&) -> completion_queue_binding& = delete;

    void bind(completion_queue_impl& queue) noexcept;
    void reset() noexcept;
    completion_queue_impl* owner{};
    completion_queue_binding* previous{};
    completion_queue_binding* next{};
};
}  // namespace detail

/**
 * @struct cq_error_info
 * @brief Extended error information from a completion queue entry.
 */
struct cq_error_info {
    int prov_errno{0};  ///< Provider-specific error code
    /// Owned immutable diagnostic bytes, shared by copies of the event.
    /// The queue's memory resource must outlive all retaining events.
    std::shared_ptr<const std::byte[]> err_data{};
    std::size_t err_data_size{0};  ///< Size of error data in bytes
    std::size_t overflow_length{0};  ///< Received bytes discarded by truncation (native olen)
    /// True when a native CQ error entry supplied these fields. Queue read
    /// failures have no entry, even when their generic error code is nonzero.
    bool available{false};

    /**
     * @brief Checks if a provider-specific error occurred.
     * @return True if there is a provider error.
     */
    [[nodiscard]] constexpr auto has_provider_error() const noexcept -> bool {
        return prov_errno != 0;
    }
};

/**
 * @struct completion_event
 * @brief Represents a completion notification from the fabric.
 *
 * Completion events are returned when data transfer operations complete,
 * either successfully or with an error. They contain information about
 * the completed operation including bytes transferred and any errors.
 */
struct completion_event : event_base<completion_event> {
    context_ptr<void> context{};       ///< User-provided operation context
    std::error_code error;             ///< Error code if operation failed
    std::size_t bytes_transferred{0};  ///< Number of bytes transferred
    std::uint64_t flags{0};            ///< Completion flags
    std::uint64_t tag{0};              ///< Message tag for tagged operations
    std::size_t len{0};                ///< Length of the data
    std::uint64_t data{0};             ///< Immediate data if present
    cq_error_info error_info{};        ///< Extended error information
    void* buffer{nullptr};             ///< Received message location, including multi-receive
    fabric_addr source{
        fabric_addrs::unspecified};  ///< Receive source, when requested and available

    /**
     * @brief Returns the error code from this completion.
     * @return The error code.
     */
    [[nodiscard]] constexpr auto impl_error() const noexcept -> std::error_code { return error; }

    /**
     * @brief Checks if the completion has an error.
     * @return True if an error occurred.
     */
    [[nodiscard]] constexpr auto has_error() const noexcept -> bool {
        return static_cast<bool>(error);
    }

    /**
     * @brief Checks if immediate data is present.
     * @return True if immediate data was received.
     */
    [[nodiscard]] constexpr auto has_immediate_data() const noexcept -> bool {
        return (flags & FI_REMOTE_CQ_DATA) != 0;
    }
};

/// Completion layouts. Fields absent from the selected layout are zeroed.
enum class cq_format {
    unspecified,  ///< Use Loom's tagged default so the entry layout is known
    context,      ///< Context pointer only
    msg,          ///< Context, completion flags, and received message length
    data,         ///< Message fields, receive buffer, and immediate data
    tagged,       ///< Data fields and the received message tag
};

/**
 * @struct completion_queue_attr
 * @brief Attributes for creating a completion queue.
 */
struct completion_queue_attr {
    queue_size size{1024UL};  ///< Number of entries in the queue
    std::uint64_t flags{0};   ///< Creation flags
    bool wait_obj{false};     ///< Enable blocking wait support
    cq_format format{cq_format::tagged};  ///< Native completion entry layout
    /// Use native source-address reads. Bound endpoints must negotiate FI_SOURCE
    /// to report peer addresses; unavailable addresses remain unspecified.
    bool source_addresses{false};
};

class completion_queue;
class endpoint;

/// Registers one completion without interpreting a native context as an object.
/// The registration, context storage and callback object must remain alive until
/// dispatch or explicit reset. Reset only detaches the callback; it does not
/// cancel native work. Do not race reset/destruction with dispatch of this entry.
/// Separate entries may be registered and dispatched concurrently, subject to
/// the provider's threading contract. Registration and dispatch do not allocate.
/// Context lookup has amortized logarithmic cost in the outstanding registrations.
class completion_registration {
public:
    using callback = void (*)(void*, const completion_event&);

    completion_registration() noexcept = default;
    ~completion_registration();
    completion_registration(const completion_registration&) = delete;
    auto operator=(const completion_registration&) -> completion_registration& = delete;
    completion_registration(completion_registration&&) = delete;
    auto operator=(completion_registration&&) -> completion_registration& = delete;

    [[nodiscard]] auto bind(completion_queue& queue,
                            context_ptr<void> context,
                            void* object,
                            callback complete) noexcept -> void_result;
    /// Register through the endpoint's last successful native CQ binding.
    /// Returns an error for a missing, destroyed or unbound resource. This does
    /// not retain either resource; they must outlive accepted native work.
    [[nodiscard]] auto bind(endpoint& ep,
                            completion_direction direction,
                            context_ptr<void> context,
                            void* object,
                            callback complete) noexcept -> void_result;
    void reset() noexcept;

private:
    [[nodiscard]] auto bind_to(detail::completion_queue_impl& queue,
                               context_ptr<void> context,
                               void* object,
                               callback complete) noexcept -> void_result;
    void unlink() noexcept;
    detail::completion_queue_impl* owner_{};
    completion_registration* left_{};
    completion_registration* right_{};
    void* context_{};
    void* object_{};
    callback complete_{};
    friend class completion_queue;
    friend struct detail::completion_queue_impl;
};

/**
 * @class completion_queue
 * @brief Manages operation completion notifications.
 *
 * A completion queue receives notifications when data transfer operations
 * complete. Applications poll or wait on the queue to retrieve completion
 * events and determine operation status.
 */
class completion_queue : public fabric_object_base<completion_queue> {
public:
    using impl_ptr =
        detail::pmr_unique_ptr<detail::completion_queue_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty completion queue.
     */
    completion_queue();

    /**
     * @brief Constructs a completion_queue from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit completion_queue(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~completion_queue();

    /**
     * @brief Deleted copy constructor.
     */
    completion_queue(const completion_queue&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const completion_queue&) -> completion_queue& = delete;
    /**
     * @brief Move constructor.
     */
    completion_queue(completion_queue&&) noexcept;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(completion_queue&&) noexcept -> completion_queue&;

    [[nodiscard("completion queue creation result must be checked for errors")]]
    static auto create(const domain& dom,
                       const completion_queue_attr& attr = {},
                       memory_resource* resource = nullptr) -> result<completion_queue>;

    /// Deliver a previously read event to its registered operation. A false
    /// result leaves the event with the caller, including foreign contexts and
    /// context-free notifications/errors. Unlink before invoking the callback
    /// so it may destroy its registration or submit and dispatch other work.
    /// Native reads never invoke callbacks implicitly. Route each event once;
    /// do not replay an old event after its context storage has been reused.
    /// Callback exceptions propagate after the registration has been detached.
    [[nodiscard]] auto dispatch(const completion_event& event) const -> bool;

    [[nodiscard]] auto poll() const -> std::optional<completion_event>;

    /// Requires completion_queue_attr::wait_obj. Missing or negative timeouts
    /// wait indefinitely; finite timeouts must fit in int milliseconds.
    /// Read failures return an error; asynchronous operation failures remain
    /// completion events carrying their operation context and error.
    [[nodiscard("wait result must be checked for errors or timeout")]]
    auto wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt) const
        -> result<completion_event>;

    [[nodiscard]] auto poll_batch(std::span<completion_event> events) const -> std::size_t;

    [[nodiscard("read result must be checked for errors")]]
    auto read() const -> result<completion_event>;

    /// Read at most one native entry for this context, preserving other events
    /// for subsequent reads. Returns again if another context was preserved,
    /// the queue is empty, or another reader owns the native queue. Does not
    /// dispatch callbacks. A null context is invalid. Metadata allocation may
    /// throw before consuming the next native entry.
    [[nodiscard]] auto read_for(context_ptr<void> context) const -> result<completion_event>;

    /// Native entry layout; unspecified for an empty queue.
    [[nodiscard]] auto format() const noexcept -> cq_format;

    /// Minimum capacity requested at creation; zero if unknown or provider-selected.
    [[nodiscard]] auto capacity() const noexcept -> std::size_t;

    /// Drive native progress without consuming completions. Libfabric provides
    /// no portable queue-depth query. FI_EAVAIL leaves an error queued for a read.
    [[nodiscard]] auto progress() const noexcept -> void_result;

    auto ack(const completion_event& event) -> void_result;

    [[nodiscard]] auto get_progress_policy() const noexcept -> runtime_progress_policy;

    [[nodiscard]] auto supports_blocking_wait() const noexcept -> bool;

    [[nodiscard]] auto requires_manual_progress() const noexcept -> bool;

    /**
     * @brief Returns the wait object file descriptor for async I/O integration.
     *
     * This method returns the file descriptor that can be used with epoll, kqueue,
     * io_uring, or similar mechanisms to wait for completions without busy-polling.
     *
     * @return The file descriptor, or an error if not supported.
     * @note The completion queue must have been created with wait_obj=true.
     */
    [[nodiscard]] auto get_wait_fd() const -> result<int>;

    /// Prepare a native wait using the fabric recorded during queue creation.
    /// False means poll again; call this before every potentially blocking wait.
    /// The native fabric must outlive the queue. Unknown parents are unsupported.
    [[nodiscard]] auto try_wait() const -> result<bool>;

    /// Supply the parent for an imported queue without recorded provenance.
    /// A different fabric is rejected when the queue's parent is known.
    [[nodiscard]] auto try_wait(const fabric& parent) const -> result<bool>;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    [[nodiscard]] auto read_native() const -> result<completion_event>;
    [[nodiscard]] auto read_error(int status) const -> result<completion_event>;
    impl_ptr impl_;
    friend class endpoint;
    friend class completion_registration;
};

/**
 * @enum wait_condition
 * @brief Wait conditions for completion queue operations.
 */
enum class wait_condition {
    none,       ///< No wait condition
    threshold,  ///< Wait until threshold reached
    timeout,    ///< Wait with timeout
};

}  // namespace loom
