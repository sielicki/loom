// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>

#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept progress_policy
 * @brief Concept for progress policy types.
 * @tparam T The policy type to check.
 */
template <typename T>
concept progress_policy = requires(const T& policy) {
    { policy.control_progress() } noexcept -> std::same_as<progress_mode>;
    { policy.data_progress() } noexcept -> std::same_as<progress_mode>;
    { policy.requires_manual_data_progress() } noexcept -> std::same_as<bool>;
    { policy.requires_manual_control_progress() } noexcept -> std::same_as<bool>;
    { policy.supports_blocking_wait() } noexcept -> std::same_as<bool>;
};

/**
 * @struct runtime_progress_policy
 * @brief Runtime-configurable progress policy.
 *
 * Allows progress mode configuration at runtime based on
 * domain and provider capabilities.
 */
struct runtime_progress_policy {
    progress_mode control{progress_mode::manual};  ///< Control progress mode
    progress_mode data{progress_mode::manual};     ///< Data progress mode

    /**
     * @brief Default constructor with manual progress.
     */
    constexpr runtime_progress_policy() noexcept = default;

    /**
     * @brief Constructs with specified progress modes.
     * @param ctrl Control progress mode.
     * @param dat Data progress mode.
     */
    constexpr runtime_progress_policy(progress_mode ctrl, progress_mode dat) noexcept
        : control(ctrl), data(dat) {}

    /**
     * @brief Returns the control progress mode.
     * @return The control progress mode.
     */
    [[nodiscard]] constexpr auto control_progress() const noexcept -> progress_mode {
        return control;
    }

    /**
     * @brief Returns the data progress mode.
     * @return The data progress mode.
     */
    [[nodiscard]] constexpr auto data_progress() const noexcept -> progress_mode { return data; }

    /**
     * @brief Checks if manual data progress is required.
     * @return True if manual progress needed.
     */
    [[nodiscard]] constexpr auto requires_manual_data_progress() const noexcept -> bool {
        return data == progress_mode::manual || data == progress_mode::manual_all;
    }

    /**
     * @brief Checks if manual control progress is required.
     * @return True if manual progress needed.
     */
    [[nodiscard]] constexpr auto requires_manual_control_progress() const noexcept -> bool {
        return control == progress_mode::manual || control == progress_mode::manual_all;
    }

    /**
     * @brief Checks if blocking wait is supported.
     * @return True if blocking wait works.
     */
    [[nodiscard]] constexpr auto supports_blocking_wait() const noexcept -> bool {
        return data == progress_mode::auto_progress;
    }
};

/**
 * @struct static_progress_policy
 * @brief Compile-time progress policy.
 * @tparam ControlProgress The control progress mode.
 * @tparam DataProgress The data progress mode.
 *
 * Provides compile-time constant progress policy for zero-overhead
 * progress mode checking.
 */
template <progress_mode ControlProgress, progress_mode DataProgress>
struct static_progress_policy {
    static constexpr progress_mode control_progress_value = ControlProgress;  ///< Control mode
    static constexpr progress_mode data_progress_value = DataProgress;        ///< Data mode

    /**
     * @brief Returns the control progress mode.
     * @return The control progress mode.
     */
    [[nodiscard]] static constexpr auto control_progress() noexcept -> progress_mode {
        return ControlProgress;
    }

    /**
     * @brief Returns the data progress mode.
     * @return The data progress mode.
     */
    [[nodiscard]] static constexpr auto data_progress() noexcept -> progress_mode {
        return DataProgress;
    }

    /**
     * @brief Checks if manual data progress is required.
     * @return True if manual progress needed.
     */
    [[nodiscard]] static constexpr auto requires_manual_data_progress() noexcept -> bool {
        return DataProgress == progress_mode::manual || DataProgress == progress_mode::manual_all;
    }

    /**
     * @brief Checks if manual control progress is required.
     * @return True if manual progress needed.
     */
    [[nodiscard]] static constexpr auto requires_manual_control_progress() noexcept -> bool {
        return ControlProgress == progress_mode::manual ||
               ControlProgress == progress_mode::manual_all;
    }

    /**
     * @brief Checks if blocking wait is supported.
     * @return True if blocking wait works.
     */
    [[nodiscard]] static constexpr auto supports_blocking_wait() noexcept -> bool {
        return DataProgress == progress_mode::auto_progress;
    }
};

using auto_progress_policy =
    static_progress_policy<progress_mode::auto_progress, progress_mode::auto_progress>;

using manual_progress_policy = static_progress_policy<progress_mode::manual, progress_mode::manual>;

static_assert(progress_policy<runtime_progress_policy>);
static_assert(progress_policy<auto_progress_policy>);
static_assert(progress_policy<manual_progress_policy>);

static_assert(auto_progress_policy::supports_blocking_wait());
static_assert(!manual_progress_policy::supports_blocking_wait());
static_assert(manual_progress_policy::requires_manual_data_progress());
static_assert(!auto_progress_policy::requires_manual_data_progress());

}  // namespace loom
