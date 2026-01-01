// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <string_view>
#include <system_error>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @enum errc
 * @brief Error codes for loom operations mapped from libfabric errors.
 */
enum class errc {
    success = 0,                  ///< Operation completed successfully
    no_data = -61,                ///< No data available
    message_too_long = -90,       ///< Message too long for buffer
    no_space = -28,               ///< No space left on device
    again = -11,                  ///< Resource temporarily unavailable
    invalid_argument = -22,       ///< Invalid argument
    io_error = -5,                ///< I/O error
    not_supported = -95,          ///< Operation not supported
    busy = -16,                   ///< Device or resource busy
    canceled = -125,              ///< Operation canceled
    no_memory = -12,              ///< Out of memory
    already = -114,               ///< Connection already in progress
    bad_flags = -1001,            ///< Invalid flags specified
    no_entry = -2,                ///< No such entry
    not_connected = -107,         ///< Transport endpoint not connected
    address_in_use = -98,         ///< Address already in use
    connection_refused = -111,    ///< Connection refused
    address_not_available = -99,  ///< Address not available
    timeout = -110,               ///< Connection timed out
};

/**
 * @class error_category
 * @brief Custom error category for loom errors.
 *
 * Provides error message translation and integration with
 * the standard error handling facilities.
 */
class error_category : public std::error_category {
public:
    /**
     * @brief Default constructor.
     */
    constexpr error_category() noexcept = default;

    /**
     * @brief Returns the name of the error category.
     * @return The string "loom".
     */
    auto name() const noexcept -> const char* override { return "loom"; }

    /**
     * @brief Returns a descriptive message for an error code.
     * @param ev The error value.
     * @return A descriptive error message.
     */
    auto message(int ev) const -> std::string override;

    /**
     * @brief Returns the default error condition for an error value.
     * @param ev The error value.
     * @return The error condition.
     */
    auto default_error_condition(int ev) const noexcept -> std::error_condition override {
        return std::error_condition(ev, *this);
    }
};

/**
 * @brief Returns the singleton loom error category.
 * @return Reference to the loom error category.
 */
inline auto loom_category() noexcept -> const error_category& {
    static const error_category category{};
    return category;
}

/**
 * @brief Creates an error_code from a loom errc value.
 * @param e The error code enumeration value.
 * @return A std::error_code in the loom category.
 */
inline auto make_error_code(errc e) noexcept -> std::error_code {
    return {static_cast<int>(e), loom_category()};
}

/**
 * @brief Creates an error_code from a libfabric errno value.
 * @param fi_errno The libfabric error number.
 * @return A std::error_code in the loom category.
 */
inline auto make_error_code_from_fi_errno(int fi_errno) noexcept -> std::error_code {
    return {fi_errno, loom_category()};
}

}  // namespace loom

/**
 * @namespace std
 * @brief Standard library namespace extension for error code traits.
 */
namespace std {
/**
 * @struct is_error_code_enum<loom::errc>
 * @brief Specialization enabling loom::errc as an error code enum.
 */
template <>
struct is_error_code_enum<loom::errc> : true_type {};
}  // namespace std
