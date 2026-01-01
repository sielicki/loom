// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <variant>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @struct msg_endpoint_tag
 * @brief Tag type for connection-oriented message endpoints.
 */
struct msg_endpoint_tag {
    static constexpr const char* name = "message";  ///< Endpoint type name
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const msg_endpoint_tag&) const noexcept
        -> bool = default;
};

/**
 * @struct rdm_endpoint_tag
 * @brief Tag type for reliable datagram endpoints.
 */
struct rdm_endpoint_tag {
    static constexpr const char* name = "reliable_datagram";  ///< Endpoint type name
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const rdm_endpoint_tag&) const noexcept
        -> bool = default;
};

/**
 * @struct dgram_endpoint_tag
 * @brief Tag type for unreliable datagram endpoints.
 */
struct dgram_endpoint_tag {
    static constexpr const char* name = "datagram";  ///< Endpoint type name
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const dgram_endpoint_tag&) const noexcept
        -> bool = default;
};

using endpoint_type = std::variant<msg_endpoint_tag, rdm_endpoint_tag, dgram_endpoint_tag>;

/**
 * @namespace loom::endpoint_types
 * @brief Pre-defined endpoint type constants.
 */
namespace endpoint_types {
inline constexpr endpoint_type msg{msg_endpoint_tag{}};      ///< Message endpoint type
inline constexpr endpoint_type rdm{rdm_endpoint_tag{}};      ///< Reliable datagram type
inline constexpr endpoint_type dgram{dgram_endpoint_tag{}};  ///< Datagram endpoint type
}  // namespace endpoint_types

/**
 * @concept endpoint_tag
 * @brief Concept for endpoint tag types.
 * @tparam T The type to check.
 */
template <typename T>
concept endpoint_tag = std::same_as<T, msg_endpoint_tag> || std::same_as<T, rdm_endpoint_tag> ||
                       std::same_as<T, dgram_endpoint_tag>;

/**
 * @struct endpoint_properties
 * @brief Properties for an endpoint type.
 * @tparam T The endpoint tag type.
 */
template <endpoint_tag T>
struct endpoint_properties;

/**
 * @struct endpoint_properties<msg_endpoint_tag>
 * @brief Properties for message endpoints.
 */
template <>
struct endpoint_properties<msg_endpoint_tag> {
    static constexpr bool is_reliable = true;             ///< Message endpoints are reliable
    static constexpr bool is_connection_oriented = true;  ///< Requires connection
    static constexpr bool supports_rma = true;            ///< Supports RMA
};

/**
 * @struct endpoint_properties<rdm_endpoint_tag>
 * @brief Properties for reliable datagram endpoints.
 */
template <>
struct endpoint_properties<rdm_endpoint_tag> {
    static constexpr bool is_reliable = true;              ///< RDM endpoints are reliable
    static constexpr bool is_connection_oriented = false;  ///< Connectionless
    static constexpr bool supports_rma = true;             ///< Supports RMA
};

/**
 * @struct endpoint_properties<dgram_endpoint_tag>
 * @brief Properties for datagram endpoints.
 */
template <>
struct endpoint_properties<dgram_endpoint_tag> {
    static constexpr bool is_reliable = false;             ///< Datagrams may be lost
    static constexpr bool is_connection_oriented = false;  ///< Connectionless
    static constexpr bool supports_rma = false;            ///< No RMA support
};

/**
 * @brief Checks if an endpoint type is reliable.
 * @param type The endpoint type variant.
 * @return True if the endpoint type is reliable.
 */
[[nodiscard]] constexpr auto is_reliable(const endpoint_type& type) noexcept -> bool {
    return std::visit([](auto tag) { return endpoint_properties<decltype(tag)>::is_reliable; },
                      type);
}

/**
 * @brief Checks if an endpoint type is connection-oriented.
 * @param type The endpoint type variant.
 * @return True if connection-oriented.
 */
[[nodiscard]] constexpr auto is_connection_oriented(const endpoint_type& type) noexcept -> bool {
    return std::visit(
        [](auto tag) { return endpoint_properties<decltype(tag)>::is_connection_oriented; }, type);
}

/**
 * @brief Checks if an endpoint type supports RMA.
 * @param type The endpoint type variant.
 * @return True if RMA is supported.
 */
[[nodiscard]] constexpr auto supports_rma(const endpoint_type& type) noexcept -> bool {
    return std::visit([](auto tag) { return endpoint_properties<decltype(tag)>::supports_rma; },
                      type);
}

/**
 * @brief Gets the name of an endpoint type.
 * @param type The endpoint type variant.
 * @return The type name string.
 */
[[nodiscard]] constexpr auto type_name(const endpoint_type& type) noexcept -> const char* {
    return std::visit([](auto tag) { return decltype(tag)::name; }, type);
}

}  // namespace loom
