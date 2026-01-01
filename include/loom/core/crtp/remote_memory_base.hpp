// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include "loom/core/crtp/crtp_tags.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class remote_memory_base
 * @brief CRTP base class for remote memory descriptors.
 * @tparam Derived The derived class type.
 *
 * Provides common interface methods for accessing remote memory
 * region properties and performing address calculations.
 */
template <typename Derived>
class remote_memory_base {
public:
    using crtp_tag = crtp::remote_memory_tag;  ///< CRTP tag for type detection

    /**
     * @brief Returns the remote address.
     * @return The remote virtual address.
     */
    [[nodiscard]] constexpr auto get_addr() const noexcept -> std::uint64_t {
        return static_cast<const Derived*>(this)->addr;
    }

    /**
     * @brief Returns the remote memory key.
     * @return The memory region key.
     */
    [[nodiscard]] constexpr auto get_key() const noexcept -> std::uint64_t {
        return static_cast<const Derived*>(this)->key;
    }

    /**
     * @brief Returns the region length.
     * @return The length in bytes.
     */
    [[nodiscard]] constexpr auto get_length() const noexcept -> std::size_t {
        return static_cast<const Derived*>(this)->length;
    }

    /**
     * @brief Checks if an offset and length are within bounds.
     * @param offset The byte offset from start.
     * @param len The length to access.
     * @return True if the range is valid.
     */
    [[nodiscard]] constexpr auto contains(std::uint64_t offset, std::size_t len) const noexcept
        -> bool {
        auto* self = static_cast<const Derived*>(this);
        return offset < self->length && len <= self->length - offset;
    }

    /**
     * @brief Creates a new descriptor offset from this one.
     * @param offset The byte offset.
     * @return A new descriptor at the offset.
     */
    [[nodiscard]] constexpr auto offset_by(std::uint64_t offset) const noexcept -> Derived {
        auto* self = static_cast<const Derived*>(this);
        return Derived{
            self->addr + offset, self->key, self->length - static_cast<std::size_t>(offset)};
    }

    /**
     * @brief Creates a subregion descriptor.
     * @param offset The byte offset from start.
     * @param len The subregion length.
     * @return A new descriptor for the subregion.
     */
    [[nodiscard]] constexpr auto subregion(std::uint64_t offset, std::size_t len) const noexcept
        -> Derived {
        auto* self = static_cast<const Derived*>(this);
        return Derived{self->addr + offset, self->key, len};
    }

    /**
     * @brief Equality comparison operator.
     * @param other The other remote memory to compare.
     * @return True if equal.
     */
    [[nodiscard]] constexpr auto operator==(const remote_memory_base& other) const noexcept
        -> bool {
        auto* self = static_cast<const Derived*>(this);
        auto* other_derived = static_cast<const Derived*>(&other);
        return self->addr == other_derived->addr && self->key == other_derived->key &&
               self->length == other_derived->length;
    }

    /**
     * @brief Default constructor.
     */
    remote_memory_base() = default;
    /**
     * @brief Destructor.
     */
    ~remote_memory_base() = default;
    /**
     * @brief Copy constructor.
     */
    remote_memory_base(const remote_memory_base&) = default;
    /**
     * @brief Move constructor.
     */
    remote_memory_base(remote_memory_base&&) = default;
    /**
     * @brief Copy assignment operator.
     */
    auto operator=(const remote_memory_base&) -> remote_memory_base& = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(remote_memory_base&&) -> remote_memory_base& = default;
};

/**
 * @class provider_aware_remote_memory_base
 * @brief CRTP base for provider-specific remote memory descriptors.
 * @tparam Derived The derived class type.
 * @tparam ProviderTraits The provider traits type.
 */
template <typename Derived, typename ProviderTraits>
class provider_aware_remote_memory_base : public remote_memory_base<Derived> {
public:
    using provider_traits_type = ProviderTraits;  ///< Provider traits type

    /**
     * @brief Computes effective address for provider.
     * @param base The base address.
     * @param offset The offset.
     * @return The effective remote address.
     */
    [[nodiscard]] static constexpr auto compute_effective_addr(std::uint64_t base,
                                                               std::uint64_t offset) noexcept
        -> std::uint64_t {
        return ProviderTraits::compute_remote_addr(base, offset);
    }

    /**
     * @brief Computes effective address at offset.
     * @param offset The byte offset.
     * @return The effective remote address.
     */
    [[nodiscard]] constexpr auto effective_addr_at(std::uint64_t offset) const noexcept
        -> std::uint64_t {
        return compute_effective_addr(this->get_addr(), offset);
    }

    /**
     * @brief Checks if provider requires local key.
     * @return True if local key is required.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool {
        return ProviderTraits::requires_local_key();
    }

    /**
     * @brief Returns the provider name.
     * @return The provider name string.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* {
        return ProviderTraits::provider_name();
    }
};

}  // namespace loom
