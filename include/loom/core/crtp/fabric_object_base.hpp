// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include "loom/core/crtp/crtp_tags.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class fabric_object_base
 * @brief CRTP base class for fabric objects.
 * @tparam Derived The derived class type.
 *
 * Provides common interface methods for fabric objects including
 * validity checking and access to internal handles.
 */
template <typename Derived>
class fabric_object_base {
public:
    using crtp_tag = crtp::fabric_object_tag;  ///< CRTP tag for type detection

    /**
     * @brief Returns the internal libfabric handle.
     * @return Void pointer to the internal handle.
     */
    [[nodiscard]] auto internal_ptr() const noexcept -> void* {
        return static_cast<const Derived*>(this)->impl_internal_ptr();
    }

    /**
     * @brief Boolean conversion to check if object is valid.
     * @return True if the object holds a valid handle.
     */
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<const Derived*>(this)->impl_valid();
    }

protected:
    /**
     * @brief Default constructor.
     */
    fabric_object_base() = default;
    /**
     * @brief Destructor.
     */
    ~fabric_object_base() = default;
    /**
     * @brief Copy constructor.
     */
    fabric_object_base(const fabric_object_base&) = default;
    /**
     * @brief Move constructor.
     */
    fabric_object_base(fabric_object_base&&) = default;
    /**
     * @brief Copy assignment operator.
     */
    auto operator=(const fabric_object_base&) -> fabric_object_base& = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(fabric_object_base&&) -> fabric_object_base& = default;
};

}  // namespace loom
