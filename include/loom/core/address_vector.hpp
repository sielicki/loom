// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

#include "loom/core/address.hpp"
#include "loom/core/allocator.hpp"
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
 * @struct address_vector_impl
 * @brief Internal implementation for address_vector.
 */
struct address_vector_impl;
}  // namespace detail

/**
 * @struct av_handle
 * @brief Handle to an address in an address vector.
 */
struct av_handle {
    std::uint64_t value{static_cast<std::uint64_t>(-1)};  ///< The handle value

    /**
     * @brief Checks if the handle is valid.
     * @return True if the handle is valid.
     */
    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return value != static_cast<std::uint64_t>(-1);
    }

    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const av_handle&) const noexcept -> bool = default;
};

/**
 * @enum av_type
 * @brief Address vector organization types.
 */
enum class av_type : std::uint32_t {
    unspecified = 0,  ///< Let provider choose
    map = 1,          ///< Map-based address vector
    table = 2,        ///< Table-based address vector
};

/**
 * @struct address_vector_attr
 * @brief Attributes for configuring an address vector.
 */
struct address_vector_attr {
    av_type type{av_type::map};                               ///< Address vector type
    std::size_t count{128};                                   ///< Expected address count
    std::uint64_t flags{0};                                   ///< Configuration flags
    std::size_t rx_ctx_bits{0};                               ///< Receive context bits
    std::size_t ep_per_node{0};                               ///< Endpoints per node
    const char* name{nullptr};                                ///< Optional name
    address_format addr_format{address_format::unspecified};  ///< Address format hint
};

/**
 * @namespace loom::av_flags
 * @brief Address vector flag constants.
 */
namespace av_flags {
inline constexpr std::uint64_t read_only = 1ULL << 0;  ///< Read-only address vector
inline constexpr std::uint64_t symmetric = 1ULL << 1;  ///< Symmetric addressing
inline constexpr std::uint64_t async = 1ULL << 2;      ///< Async address insertion
}  // namespace av_flags

/**
 * @class address_vector
 * @brief Address vector for managing peer addresses.
 *
 * Address vectors map fabric addresses to local handles for
 * efficient endpoint addressing in connectionless communication.
 */
class address_vector : public fabric_object_base<address_vector> {
public:
    using impl_ptr =
        detail::pmr_unique_ptr<detail::address_vector_impl>;  ///< Implementation pointer type

    /**
     * @brief Default constructor creating an empty address vector.
     */
    address_vector() = default;

    /**
     * @brief Constructs from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit address_vector(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~address_vector();

    /**
     * @brief Deleted copy constructor.
     */
    address_vector(const address_vector&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const address_vector&) -> address_vector& = delete;
    /**
     * @brief Move constructor.
     */
    address_vector(address_vector&&) noexcept;
    auto operator=(address_vector&&) noexcept -> address_vector&;

    [[nodiscard]] static auto create(const domain& dom,
                                     const address_vector_attr& attr = {},
                                     memory_resource* resource = nullptr) -> result<address_vector>;

    [[nodiscard]] auto insert(const address& addr, void* context = nullptr) -> result<av_handle>;

    [[nodiscard]] auto insert_batch(std::span<const address> addresses,
                                    std::span<av_handle> handles,
                                    std::span<void*> contexts = {}) -> result<std::size_t>;

    auto remove(av_handle handle) -> void_result;

    auto remove_batch(std::span<const av_handle> handles) -> void_result;

    [[nodiscard]] auto lookup(av_handle handle) const -> result<address>;

    [[nodiscard]] auto address_to_string(av_handle handle) const -> result<std::string>;

    [[nodiscard]] auto get_type() const -> av_type;

    [[nodiscard]] auto count() const noexcept -> std::size_t;

    auto bind(class endpoint& ep) -> void_result;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class endpoint;
};

}  // namespace loom
