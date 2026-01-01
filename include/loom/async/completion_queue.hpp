// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

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
}  // namespace detail

/**
 * @struct cq_error_info
 * @brief Extended error information from a completion queue entry.
 */
struct cq_error_info {
    int prov_errno{0};             ///< Provider-specific error code
    void* err_data{nullptr};       ///< Provider-specific error data
    std::size_t err_data_size{0};  ///< Size of error data in bytes

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
        return (flags & (1ULL << 4)) != 0;
    }
};

/**
 * @struct completion_queue_attr
 * @brief Attributes for creating a completion queue.
 */
struct completion_queue_attr {
    queue_size size{1024UL};  ///< Number of entries in the queue
    std::uint64_t flags{0};   ///< Creation flags
    bool wait_obj{false};     ///< Enable blocking wait support
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
    completion_queue() = default;

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

    [[nodiscard]] static auto create(const domain& dom,
                                     const completion_queue_attr& attr = {},
                                     memory_resource* resource = nullptr)
        -> result<completion_queue>;

    [[nodiscard]] auto poll() -> std::optional<completion_event>;

    [[nodiscard]] auto wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt)
        -> result<completion_event>;

    [[nodiscard]] auto poll_batch(std::span<completion_event> events) -> std::size_t;

    [[nodiscard]] auto read() -> result<completion_event>;

    [[nodiscard]] auto capacity() const noexcept -> std::size_t;

    [[nodiscard]] auto pending() const noexcept -> std::size_t;

    auto ack(const completion_event& event) -> void_result;

    [[nodiscard]] auto get_progress_policy() const noexcept -> runtime_progress_policy;

    [[nodiscard]] auto supports_blocking_wait() const noexcept -> bool;

    [[nodiscard]] auto requires_manual_progress() const noexcept -> bool;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class endpoint;
};

/**
 * @enum cq_format
 * @brief Completion queue entry format types.
 */
enum class cq_format {
    unspecified,  ///< Provider determines format
    context,      ///< Context pointer only
    msg,          ///< Message format with size
    data,         ///< Data format with immediate data
    tagged,       ///< Tagged message format
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
