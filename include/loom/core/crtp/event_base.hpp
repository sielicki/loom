// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <system_error>

#include "loom/core/crtp/crtp_tags.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class event_base
 * @brief CRTP base class for event types.
 * @tparam Derived The derived event type.
 *
 * Provides common interface methods for checking event success
 * or failure status.
 */
template <typename Derived>
class event_base {
public:
    using crtp_tag = crtp::event_tag;  ///< CRTP tag for type detection

    /**
     * @brief Checks if the event completed successfully.
     * @return True if no error occurred.
     */
    [[nodiscard]] constexpr auto success() const noexcept -> bool {
        return !static_cast<const Derived*>(this)->impl_error();
    }

    /**
     * @brief Checks if the event failed.
     * @return True if an error occurred.
     */
    [[nodiscard]] constexpr auto failed() const noexcept -> bool {
        return static_cast<bool>(static_cast<const Derived*>(this)->impl_error());
    }

    /**
     * @brief Returns the error code from the event.
     * @return The error code, or empty code if successful.
     */
    [[nodiscard]] constexpr auto error() const noexcept -> std::error_code {
        return static_cast<const Derived*>(this)->impl_error();
    }

    /**
     * @brief Default constructor.
     */
    event_base() = default;
    /**
     * @brief Destructor.
     */
    ~event_base() = default;
    /**
     * @brief Copy constructor.
     */
    event_base(const event_base&) = default;
    /**
     * @brief Move constructor.
     */
    event_base(event_base&&) = default;
    /**
     * @brief Copy assignment operator.
     */
    auto operator=(const event_base&) -> event_base& = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(event_base&&) -> event_base& = default;
};

}  // namespace loom
