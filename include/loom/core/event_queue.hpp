// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/event_base.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace loom::detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct event_queue_impl
 * @brief Internal implementation for event_queue.
 */
struct event_queue_impl;
}  // namespace detail

/**
 * @enum event_type
 * @brief Types of fabric events.
 */
enum class event_type : std::uint32_t {
    connected = 0,      ///< Connection established
    connreq = 1,        ///< Connection request received
    shutdown = 2,       ///< Shutdown notification
    join_complete = 3,  ///< Multicast join complete
    mr_complete = 4,    ///< Memory registration complete
    av_complete = 5,    ///< Address vector operation complete
};

/**
 * @struct fabric_event
 * @brief Fabric event structure for event queue notifications.
 *
 * Contains event type, context, and associated data.
 */
struct fabric_event : event_base<fabric_event> {
    event_type type{};          ///< The event type
    void* fid{nullptr};         ///< Associated fabric identifier
    void* context{nullptr};     ///< User context pointer
    std::error_code error;      ///< Error code if event indicates failure
    std::uint64_t data{0};      ///< Event-specific data
    std::span<std::byte> info;  ///< Additional event information
    std::size_t info_len{0};    ///< Length of info data

    /**
     * @brief Returns the error code for this event.
     * @return The error code.
     */
    [[nodiscard]] constexpr auto impl_error() const noexcept -> std::error_code { return error; }
};

/**
 * @struct event_queue_attr
 * @brief Attributes for configuring an event queue.
 */
struct event_queue_attr {
    queue_size size{128UL};             ///< Queue capacity
    std::uint64_t flags{0};             ///< Configuration flags
    bool wait_obj{false};               ///< Enable wait object support
    std::uint64_t signaling_vector{0};  ///< Signaling vector for interrupts
};

/**
 * @class event_queue
 * @brief Event queue for receiving fabric events.
 *
 * Event queues deliver fabric events such as connection requests,
 * connection completions, and shutdown notifications.
 */
class event_queue : public fabric_object_base<event_queue> {
public:
    using impl_ptr =
        detail::pmr_unique_ptr<detail::event_queue_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty event queue.
     */
    event_queue() = default;

    /**
     * @brief Constructs from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit event_queue(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~event_queue();

    /**
     * @brief Deleted copy constructor.
     */
    event_queue(const event_queue&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const event_queue&) -> event_queue& = delete;
    /**
     * @brief Move constructor.
     */
    event_queue(event_queue&&) noexcept;
    auto operator=(event_queue&&) noexcept -> event_queue&;

    [[nodiscard]] static auto create(const fabric& fab,
                                     const event_queue_attr& attr = {},
                                     memory_resource* resource = nullptr) -> result<event_queue>;

    [[nodiscard]] auto poll() -> std::optional<fabric_event>;

    [[nodiscard]] auto wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt)
        -> result<fabric_event>;

    [[nodiscard]] auto read() -> result<fabric_event>;

    auto write_error(const fabric_event& event) -> void_result;

    [[nodiscard]] auto event_to_string(const fabric_event& event) const -> std::string;

    [[nodiscard]] auto capacity() const noexcept -> std::size_t;

    auto ack(const fabric_event& event) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class endpoint;
    friend class passive_endpoint;
};

}  // namespace loom
