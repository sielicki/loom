// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <variant>

#include "loom/core/concepts.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace address_traits
 * @brief Type traits for network address types.
 */
namespace address_traits {

/**
 * @struct properties
 * @brief Primary template for address type properties.
 * @tparam T The address type.
 */
template <typename T>
struct properties;

/**
 * @brief Type trait indicating if a type is a network protocol address.
 * @tparam T The address type to check.
 */
template <typename T>
inline constexpr bool is_network_protocol_v = false;

/**
 * @brief Type trait indicating if a type supports RDMA operations.
 * @tparam T The address type to check.
 */
template <typename T>
inline constexpr bool is_rdma_capable_v = false;

/**
 * @brief Type trait providing the size of an address type.
 * @tparam T The address type.
 */
template <typename T>
inline constexpr std::size_t address_size_v = 0;
}  // namespace address_traits

/**
 * @class ipv4_address
 * @brief Represents an IPv4 network address with optional port.
 */
class ipv4_address {
public:
    static constexpr std::size_t octet_count = 4;  ///< Number of octets in IPv4 address
    using octet_type = std::uint8_t;               ///< Type for individual octets

    /**
     * @brief Default constructor creating a zero address.
     */
    constexpr ipv4_address() noexcept = default;

    /**
     * @brief Constructs an IPv4 address from individual octets.
     * @tparam A Type of first octet.
     * @tparam B Type of second octet.
     * @tparam C Type of third octet.
     * @tparam D Type of fourth octet.
     * @param a First octet.
     * @param b Second octet.
     * @param c Third octet.
     * @param d Fourth octet.
     * @param port Optional port number.
     */
    template <std::integral A, std::integral B, std::integral C, std::integral D>
        requires(sizeof(A) <= sizeof(std::uint8_t)) && (sizeof(B) <= sizeof(std::uint8_t)) &&
                    (sizeof(C) <= sizeof(std::uint8_t)) && (sizeof(D) <= sizeof(std::uint8_t))
    constexpr ipv4_address(A a, B b, C c, D d, std::uint16_t port = 0) noexcept
        : octets_{static_cast<octet_type>(a),
                  static_cast<octet_type>(b),
                  static_cast<octet_type>(c),
                  static_cast<octet_type>(d)},
          port_(port) {}

    /**
     * @brief Constructs an IPv4 address from a 32-bit integer.
     * @param addr The address in network byte order.
     * @param port Optional port number.
     */
    constexpr explicit ipv4_address(std::uint32_t addr, std::uint16_t port = 0) noexcept
        : octets_{static_cast<octet_type>((addr >> 24) & 0xFF),
                  static_cast<octet_type>((addr >> 16) & 0xFF),
                  static_cast<octet_type>((addr >> 8) & 0xFF),
                  static_cast<octet_type>(addr & 0xFF)},
          port_(port) {}

    /**
     * @brief Returns the address octets.
     * @return Span of the four address octets.
     */
    [[nodiscard]] constexpr auto octets() const noexcept
        -> std::span<const octet_type, octet_count> {
        return octets_;
    }

    /**
     * @brief Returns the port number.
     * @return The port number.
     */
    [[nodiscard]] constexpr auto port() const noexcept -> std::uint16_t { return port_; }

    /**
     * @brief Converts the address to a 32-bit integer.
     * @return The address as a 32-bit integer.
     */
    [[nodiscard]] constexpr auto to_uint32() const noexcept -> std::uint32_t {
        return (static_cast<std::uint32_t>(octets_[0]) << 24) |
               (static_cast<std::uint32_t>(octets_[1]) << 16) |
               (static_cast<std::uint32_t>(octets_[2]) << 8) |
               static_cast<std::uint32_t>(octets_[3]);
    }

    /**
     * @brief Checks if this is a loopback address (127.x.x.x).
     * @return True if loopback address.
     */
    [[nodiscard]] constexpr auto is_loopback() const noexcept -> bool { return octets_[0] == 127; }

    /**
     * @brief Checks if this is a private network address.
     * @return True if private address (10.x, 172.16-31.x, 192.168.x).
     */
    [[nodiscard]] constexpr auto is_private() const noexcept -> bool {
        return (octets_[0] == 10) || (octets_[0] == 172 && octets_[1] >= 16 && octets_[1] <= 31) ||
               (octets_[0] == 192 && octets_[1] == 168);
    }

    /**
     * @brief Checks if this is a multicast address (224-239.x.x.x).
     * @return True if multicast address.
     */
    [[nodiscard]] constexpr auto is_multicast() const noexcept -> bool {
        return (octets_[0] >= 224 && octets_[0] <= 239);
    }

    /**
     * @brief Three-way comparison operator.
     */
    [[nodiscard]] constexpr auto operator<=>(const ipv4_address&) const noexcept = default;
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const ipv4_address&) const noexcept -> bool = default;

private:
    std::array<octet_type, octet_count> octets_{};
    std::uint16_t port_{};
};

static_assert(trivially_serializable<ipv4_address>);
static_assert(network_address<ipv4_address>);

/**
 * @namespace address_traits
 * @brief Specializations for ipv4_address traits.
 */
namespace address_traits {
/**
 * @brief IPv4 addresses are network protocol addresses.
 */
template <>
inline constexpr bool is_network_protocol_v<ipv4_address> = true;

/**
 * @brief Size of ipv4_address type.
 */
template <>
inline constexpr std::size_t address_size_v<ipv4_address> = sizeof(ipv4_address);
}  // namespace address_traits

/**
 * @class ipv6_address
 * @brief Represents an IPv6 network address with optional port.
 */
class ipv6_address {
public:
    static constexpr std::size_t segment_count = 8;  ///< Number of 16-bit segments
    using segment_type = std::uint16_t;              ///< Type for individual segments

    /**
     * @brief Default constructor creating a zero address.
     */
    constexpr ipv6_address() noexcept = default;

    /**
     * @brief Constructs an IPv6 address from a range of segments.
     * @tparam R A contiguous range of uint16_t segments.
     * @param segments The eight 16-bit segments.
     * @param port Optional port number.
     */
    template <typename R>
        requires std::ranges::contiguous_range<R> &&
                 std::same_as<std::ranges::range_value_t<R>, segment_type>
    constexpr explicit ipv6_address(R&& segments, std::uint16_t port = 0) noexcept : port_(port) {
        std::ranges::copy_n(std::ranges::begin(segments), segment_count, segments_.begin());
    }

    /**
     * @brief Constructs an IPv6 address from a span of segments.
     * @param segments Span of exactly eight 16-bit segments.
     * @param port Optional port number.
     */
    constexpr explicit ipv6_address(std::span<const segment_type, segment_count> segments,
                                    std::uint16_t port = 0) noexcept
        : port_(port) {
        std::ranges::copy(segments, segments_.begin());
    }

    /**
     * @brief Returns the address segments.
     * @return Span of the eight 16-bit segments.
     */
    [[nodiscard]] constexpr auto segments() const noexcept
        -> std::span<const segment_type, segment_count> {
        return segments_;
    }

    /**
     * @brief Returns the port number.
     * @return The port number.
     */
    [[nodiscard]] constexpr auto port() const noexcept -> std::uint16_t { return port_; }

    /**
     * @brief Checks if this is the IPv6 loopback address (::1).
     * @return True if loopback address.
     */
    [[nodiscard]] constexpr auto is_loopback() const noexcept -> bool {
        return std::ranges::all_of(segments_ | std::views::take(7),
                                   [](auto s) { return s == 0; }) &&
               segments_[7] == 1;
    }

    /**
     * @brief Checks if this is a multicast address (ff00::/8).
     * @return True if multicast address.
     */
    [[nodiscard]] constexpr auto is_multicast() const noexcept -> bool {
        return (segments_[0] & 0xFF00) == 0xFF00;
    }

    /**
     * @brief Three-way comparison operator.
     */
    [[nodiscard]] constexpr auto operator<=>(const ipv6_address&) const noexcept = default;
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const ipv6_address&) const noexcept -> bool = default;

private:
    std::array<segment_type, segment_count> segments_{};
    std::uint16_t port_{};
};

static_assert(trivially_serializable<ipv6_address>);
static_assert(network_address<ipv6_address>);

/**
 * @namespace address_traits
 * @brief Specializations for ipv6_address traits.
 */
namespace address_traits {
/**
 * @brief IPv6 addresses are network protocol addresses.
 */
template <>
inline constexpr bool is_network_protocol_v<ipv6_address> = true;

/**
 * @brief Size of ipv6_address type.
 */
template <>
inline constexpr std::size_t address_size_v<ipv6_address> = sizeof(ipv6_address);
}  // namespace address_traits

/**
 * @class ib_address
 * @brief Represents an InfiniBand network address.
 */
class ib_address {
public:
    static constexpr std::size_t gid_size = 16;  ///< Size of GID in bytes
    using gid_type = std::uint8_t;               ///< Type for GID bytes

    /**
     * @brief Default constructor creating a zero address.
     */
    constexpr ib_address() noexcept = default;

    /**
     * @brief Constructs an InfiniBand address.
     * @param gid The 128-bit Global Identifier.
     * @param qpn The Queue Pair Number.
     * @param lid The Local Identifier.
     */
    constexpr explicit ib_address(std::span<const gid_type, gid_size> gid,
                                  std::uint32_t qpn = 0,
                                  std::uint16_t lid = 0) noexcept
        : qpn_(qpn), lid_(lid) {
        std::ranges::copy(gid, gid_.begin());
    }

    /**
     * @brief Returns the Global Identifier.
     * @return Span of the 16-byte GID.
     */
    [[nodiscard]] constexpr auto gid() const noexcept -> std::span<const gid_type, gid_size> {
        return gid_;
    }

    /**
     * @brief Returns the Queue Pair Number.
     * @return The QPN.
     */
    [[nodiscard]] constexpr auto qpn() const noexcept -> std::uint32_t { return qpn_; }

    /**
     * @brief Returns the Local Identifier.
     * @return The LID.
     */
    [[nodiscard]] constexpr auto lid() const noexcept -> std::uint16_t { return lid_; }

    /**
     * @brief Three-way comparison operator.
     */
    [[nodiscard]] constexpr auto operator<=>(const ib_address&) const noexcept = default;
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const ib_address&) const noexcept -> bool = default;

private:
    std::array<gid_type, gid_size> gid_{};
    std::uint32_t qpn_{};
    std::uint16_t lid_{};
};

static_assert(trivially_serializable<ib_address>);
static_assert(gid_address<ib_address>);

/**
 * @namespace address_traits
 * @brief Specializations for ib_address traits.
 */
namespace address_traits {
/**
 * @brief InfiniBand addresses support RDMA operations.
 */
template <>
inline constexpr bool is_rdma_capable_v<ib_address> = true;

/**
 * @brief Size of ib_address type.
 */
template <>
inline constexpr std::size_t address_size_v<ib_address> = sizeof(ib_address);
}  // namespace address_traits

/**
 * @class ethernet_address
 * @brief Represents an Ethernet MAC address.
 */
class ethernet_address {
public:
    static constexpr std::size_t mac_size = 6;  ///< Size of MAC address in bytes
    using octet_type = std::uint8_t;            ///< Type for MAC octets

    /**
     * @brief Default constructor creating a zero address.
     */
    constexpr ethernet_address() noexcept = default;

    /**
     * @brief Constructs an Ethernet address from a MAC.
     * @param mac The 6-byte MAC address.
     */
    constexpr explicit ethernet_address(std::span<const octet_type, mac_size> mac) noexcept {
        std::ranges::copy(mac, mac_.begin());
    }

    /**
     * @brief Returns the MAC address bytes.
     * @return Span of the 6-byte MAC address.
     */
    [[nodiscard]] constexpr auto mac() const noexcept -> std::span<const octet_type, mac_size> {
        return mac_;
    }

    /**
     * @brief Checks if this is a unicast address.
     * @return True if unicast (individual) address.
     */
    [[nodiscard]] constexpr auto is_unicast() const noexcept -> bool {
        return (mac_[0] & 0x01) == 0;
    }

    /**
     * @brief Checks if this is a multicast address.
     * @return True if multicast (group) address.
     */
    [[nodiscard]] constexpr auto is_multicast() const noexcept -> bool {
        return (mac_[0] & 0x01) == 1;
    }

    /**
     * @brief Checks if this is the broadcast address.
     * @return True if broadcast (ff:ff:ff:ff:ff:ff).
     */
    [[nodiscard]] constexpr auto is_broadcast() const noexcept -> bool {
        return std::ranges::all_of(mac_, [](auto b) { return b == 0xFF; });
    }

    /**
     * @brief Checks if this is a locally administered address.
     * @return True if locally administered.
     */
    [[nodiscard]] constexpr auto is_locally_administered() const noexcept -> bool {
        return (mac_[0] & 0x02) == 0x02;
    }

    /**
     * @brief Three-way comparison operator.
     */
    [[nodiscard]] constexpr auto operator<=>(const ethernet_address&) const noexcept = default;
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const ethernet_address&) const noexcept
        -> bool = default;

private:
    std::array<octet_type, mac_size> mac_{};
};

static_assert(trivially_serializable<ethernet_address>);
static_assert(mac_address<ethernet_address>);

/**
 * @namespace address_traits
 * @brief Specializations for ethernet_address traits.
 */
namespace address_traits {
/**
 * @brief Size of ethernet_address type.
 */
template <>
inline constexpr std::size_t address_size_v<ethernet_address> = sizeof(ethernet_address);
}  // namespace address_traits

/**
 * @class unspecified_address
 * @brief Represents an unspecified or unknown address type.
 */
class unspecified_address {
public:
    /**
     * @brief Default constructor.
     */
    constexpr unspecified_address() noexcept = default;
    /**
     * @brief Three-way comparison operator.
     */
    [[nodiscard]] constexpr auto operator<=>(const unspecified_address&) const noexcept = default;
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const unspecified_address&) const noexcept
        -> bool = default;
};

static_assert(trivially_serializable<unspecified_address>);
static_assert(address_type<unspecified_address>);

/**
 * @typedef address
 * @brief Variant type holding any supported address type.
 */
using address =
    std::variant<unspecified_address, ipv4_address, ipv6_address, ib_address, ethernet_address>;

/**
 * @namespace address_ops
 * @brief Operations on address types.
 */
namespace address_ops {

/**
 * @brief Returns the size of an address type at compile time.
 * @tparam T The address type.
 * @return The size in bytes.
 */
template <address_type T>
[[nodiscard]] constexpr auto size() noexcept -> std::size_t {
    return address_traits::address_size_v<T>;
}

/**
 * @brief Returns the size of an address at runtime.
 * @param addr The address variant.
 * @return The size in bytes.
 */
[[nodiscard]] constexpr auto size(const address& addr) noexcept -> std::size_t {
    return std::visit(
        [](const auto& a) -> std::size_t {
            using T = std::decay_t<decltype(a)>;
            return sizeof(T);
        },
        addr);
}

/**
 * @brief Checks if an address type supports RDMA at compile time.
 * @tparam T The address type.
 * @return True if RDMA capable.
 */
template <address_type T>
[[nodiscard]] constexpr auto is_rdma_capable() noexcept -> bool {
    return address_traits::is_rdma_capable_v<T>;
}

/**
 * @brief Checks if an address supports RDMA at runtime.
 * @param addr The address variant.
 * @return True if RDMA capable.
 */
[[nodiscard]] constexpr auto is_rdma_capable(const address& addr) noexcept -> bool {
    return std::visit(
        [](const auto& a) -> bool {
            using T = std::decay_t<decltype(a)>;
            return address_traits::is_rdma_capable_v<T>;
        },
        addr);
}

/**
 * @brief Checks if an address type is a network protocol at compile time.
 * @tparam T The address type.
 * @return True if network protocol.
 */
template <address_type T>
[[nodiscard]] constexpr auto is_network_protocol() noexcept -> bool {
    return address_traits::is_network_protocol_v<T>;
}

/**
 * @brief Checks if an address is a network protocol at runtime.
 * @param addr The address variant.
 * @return True if network protocol.
 */
[[nodiscard]] constexpr auto is_network_protocol(const address& addr) noexcept -> bool {
    return std::visit(
        [](const auto& a) -> bool {
            using T = std::decay_t<decltype(a)>;
            return address_traits::is_network_protocol_v<T>;
        },
        addr);
}
}  // namespace address_ops

/**
 * @namespace addresses
 * @brief Factory functions and constants for creating addresses.
 */
namespace addresses {

/**
 * @brief Creates an IPv4 address from octets.
 * @tparam A Type of first octet.
 * @tparam B Type of second octet.
 * @tparam C Type of third octet.
 * @tparam D Type of fourth octet.
 * @param a First octet.
 * @param b Second octet.
 * @param c Third octet.
 * @param d Fourth octet.
 * @param port Optional port number.
 * @return An address variant containing the IPv4 address.
 */
template <std::integral A, std::integral B, std::integral C, std::integral D>
    requires(sizeof(A) <= 1 && sizeof(B) <= 1 && sizeof(C) <= 1 && sizeof(D) <= 1)
[[nodiscard]] constexpr auto ipv4(A a, B b, C c, D d, std::uint16_t port = 0) noexcept -> address {
    return ipv4_address{a, b, c, d, port};
}

/**
 * @brief Creates an IPv6 address from a range of segments.
 * @tparam R A contiguous range of uint16_t.
 * @param segments The eight 16-bit segments.
 * @param port Optional port number.
 * @return An address variant containing the IPv6 address.
 */
template <std::ranges::contiguous_range R>
    requires std::same_as<std::ranges::range_value_t<R>, std::uint16_t>
[[nodiscard]] constexpr auto ipv6(R&& segments, std::uint16_t port = 0) noexcept -> address {
    return ipv6_address{std::forward<R>(segments), port};
}

/**
 * @brief Creates an IPv6 address from a span of segments.
 * @param segments Span of exactly eight 16-bit segments.
 * @param port Optional port number.
 * @return An address variant containing the IPv6 address.
 */
[[nodiscard]] constexpr auto ipv6(std::span<const std::uint16_t, 8> segments,
                                  std::uint16_t port = 0) noexcept -> address {
    return ipv6_address{segments, port};
}

/**
 * @brief Creates an InfiniBand address.
 * @param gid The 128-bit Global Identifier.
 * @param qpn The Queue Pair Number.
 * @param lid The Local Identifier.
 * @return An address variant containing the IB address.
 */
[[nodiscard]] constexpr auto infiniband(std::span<const std::uint8_t, 16> gid,
                                        std::uint32_t qpn = 0,
                                        std::uint16_t lid = 0) noexcept -> address {
    return ib_address{gid, qpn, lid};
}

/**
 * @brief Creates an Ethernet address from a MAC.
 * @param mac The 6-byte MAC address.
 * @return An address variant containing the Ethernet address.
 */
[[nodiscard]] constexpr auto ethernet(std::span<const std::uint8_t, 6> mac) noexcept -> address {
    return ethernet_address{mac};
}

/**
 * @brief Creates an unspecified address.
 * @return An address variant containing an unspecified address.
 */
[[nodiscard]] constexpr auto unspecified() noexcept -> address {
    return unspecified_address{};
}

/**
 * @brief Compile-time IPv4 address literal.
 * @tparam A First octet.
 * @tparam B Second octet.
 * @tparam C Third octet.
 * @tparam D Fourth octet.
 * @tparam Port Port number.
 */
template <std::uint8_t A, std::uint8_t B, std::uint8_t C, std::uint8_t D, std::uint16_t Port = 0>
inline constexpr address ipv4_literal = ipv4_address{A, B, C, D, Port};

inline constexpr address localhost_v4 = ipv4_address{
    std::uint8_t{127}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, 0};  ///< IPv4 loopback
inline constexpr address any_v4 = ipv4_address{
    std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, 0};  ///< IPv4 any address
inline constexpr address broadcast_v4 = ipv4_address{std::uint8_t{255},
                                                     std::uint8_t{255},
                                                     std::uint8_t{255},
                                                     std::uint8_t{255},
                                                     0};  ///< IPv4 broadcast
}  // namespace addresses

/**
 * @struct overload
 * @brief Helper for creating overload sets from lambdas for std::visit.
 * @tparam Ts Lambda types.
 */
template <typename... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};

/**
 * @brief Deduction guide for overload.
 * @tparam Ts Lambda types.
 */
template <typename... Ts>
overload(Ts...) -> overload<Ts...>;

/**
 * @brief Visits an address variant with a visitor.
 * @tparam Visitor The visitor type.
 * @param vis The visitor callable.
 * @param addr The address to visit.
 * @return The result of the visitor.
 */
template <typename Visitor>
[[nodiscard]] constexpr auto visit(Visitor&& vis, const address& addr)
    -> decltype(std::visit(std::forward<Visitor>(vis), addr)) {
    return std::visit(std::forward<Visitor>(vis), addr);
}

/**
 * @brief Gets the address format of an address variant.
 * @param addr The address to query.
 * @return The address format enumeration value.
 */
[[nodiscard]] constexpr auto get_address_format(const address& addr) noexcept -> address_format {
    return std::visit(
        overload{[](const ipv4_address&) { return address_format::inet; },
                 [](const ipv6_address&) { return address_format::inet6; },
                 [](const ib_address&) { return address_format::ib; },
                 [](const ethernet_address&) { return address_format::ethernet; },
                 [](const unspecified_address&) { return address_format::unspecified; }},
        addr);
}

/**
 * @brief Gets the port number from an address if applicable.
 * @param addr The address to query.
 * @return The port number, or 0 if not a network address.
 */
[[nodiscard]] constexpr auto get_port(const address& addr) noexcept -> std::uint16_t {
    return std::visit(overload{[](const network_address auto& a) { return a.port(); },
                               [](const auto&) { return std::uint16_t{0}; }},
                      addr);
}

/**
 * @brief Gets a pointer to the raw address data.
 * @param addr The address to query.
 * @return Pointer to the address data.
 */
[[nodiscard]] inline auto get_address_data(const address& addr) noexcept -> const void* {
    return std::visit([](const auto& a) -> const void* { return &a; }, addr);
}

/**
 * @brief Creates an address from raw data.
 * @param data Pointer to the raw address data.
 * @param len Length of the data.
 * @param format The address format.
 * @return The constructed address variant.
 */
[[nodiscard]] auto
address_from_raw(const void* data, std::size_t len, address_format format) noexcept -> address;

}  // namespace loom
