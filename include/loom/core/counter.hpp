// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/result.hpp"

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
 * @struct counter_impl
 * @brief Internal implementation for counter.
 */
struct counter_impl;
}  // namespace detail

/**
 * @struct counter_attr
 * @brief Attributes for configuring a counter.
 */
struct counter_attr {
    std::uint64_t flags{0};      ///< Configuration flags
    std::uint64_t initial{0};    ///< Initial counter value
    std::uint64_t threshold{1};  ///< Threshold for wait operations
    bool wait_obj{false};        ///< Enable wait object support
};

/**
 * @enum counter_event
 * @brief Types of counter events.
 */
enum class counter_event : std::uint32_t {
    completion = 0,  ///< Completion event
    threshold = 1,   ///< Threshold reached event
};

/**
 * @class counter
 * @brief Hardware counter for tracking operation completions.
 *
 * Counters provide lightweight completion tracking without the
 * overhead of full completion queue events.
 */
class counter : public fabric_object_base<counter> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::counter_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty counter.
     */
    counter() = default;

    /**
     * @brief Constructs from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit counter(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~counter();

    /**
     * @brief Deleted copy constructor.
     */
    counter(const counter&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const counter&) -> counter& = delete;
    /**
     * @brief Move constructor.
     */
    counter(counter&&) noexcept;
    auto operator=(counter&&) noexcept -> counter&;

    [[nodiscard]] static auto create(const domain& dom,
                                     const counter_attr& attr = {},
                                     memory_resource* resource = nullptr) -> result<counter>;

    [[nodiscard]] auto read() const -> result<std::uint64_t>;

    auto set(std::uint64_t value) -> void_result;

    auto add(std::uint64_t value) -> void_result;

    [[nodiscard]] auto wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt)
        -> void_result;

    [[nodiscard]] auto check_threshold() const -> bool;

    [[nodiscard]] auto get_error() const -> std::error_code;

    auto ack(std::uint64_t count = 1) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class endpoint;
};

}  // namespace loom
