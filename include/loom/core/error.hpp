// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_errno.h>

#include <string_view>
#include <system_error>
#include <utility>

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
    success = 0,                                ///< Operation completed successfully
    no_data = -FI_ENODATA,                      ///< No data available
    no_message = -FI_ENOMSG,                    ///< No matching message for a tagged probe
    message_too_long = -FI_EMSGSIZE,            ///< Message too long for buffer
    no_space = -FI_ENOSPC,                      ///< No space left on device
    again = -FI_EAGAIN,                         ///< Resource temporarily unavailable
    invalid_argument = -FI_EINVAL,              ///< Invalid argument
    io_error = -FI_EIO,                         ///< I/O error
    not_supported = -FI_EOPNOTSUPP,             ///< Operation not supported
    busy = -FI_EBUSY,                           ///< Device or resource busy
    canceled = -FI_ECANCELED,                   ///< Operation canceled
    no_memory = -FI_ENOMEM,                     ///< Out of memory
    already = -FI_EALREADY,                     ///< Connection already in progress
    bad_flags = -FI_EBADFLAGS,                  ///< Invalid flags specified
    no_entry = -FI_ENOENT,                      ///< No such entry
    not_connected = -FI_ENOTCONN,               ///< Transport endpoint not connected
    address_in_use = -FI_EADDRINUSE,            ///< Address already in use
    connection_refused = -FI_ECONNREFUSED,      ///< Connection refused
    address_not_available = -FI_EADDRNOTAVAIL,  ///< Address not available
    timeout = -FI_ETIMEDOUT,                    ///< Connection timed out
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
    return {std::to_underlying(e), loom_category()};
}

/**
 * @brief Creates an error_code from a libfabric errno value.
 * @param fi_errno The libfabric error number.
 * @return A std::error_code in the loom category.
 */
inline auto make_error_code_from_fi_errno(int fi_errno) noexcept -> std::error_code {
    return {fi_errno > 0 ? -fi_errno : fi_errno, loom_category()};
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
