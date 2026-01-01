// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <string>
#include <type_traits>
#include <vector>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @typedef memory_resource
 * @brief Alias for polymorphic memory resource.
 */
using memory_resource = std::pmr::memory_resource;

/**
 * @brief Returns the default memory resource.
 * @return Pointer to the default memory resource.
 */
[[nodiscard]] inline auto get_default_resource() noexcept -> memory_resource* {
    return std::pmr::get_default_resource();
}

/**
 * @brief Sets the default memory resource.
 * @param resource The new default memory resource.
 * @return The previous default memory resource.
 */
inline auto set_default_resource(memory_resource* resource) noexcept -> memory_resource* {
    return std::pmr::set_default_resource(resource);
}

/**
 * @typedef pmr_allocator
 * @brief Polymorphic allocator type alias.
 * @tparam T The allocated type.
 */
template <typename T>
using pmr_allocator = std::pmr::polymorphic_allocator<T>;

/**
 * @namespace pmr
 * @brief Polymorphic memory resource container aliases.
 */
namespace pmr {
/**
 * @typedef vector
 * @brief PMR-aware vector type.
 * @tparam T The element type.
 */
template <typename T>
using vector = std::pmr::vector<T>;

/**
 * @typedef string
 * @brief PMR-aware string type.
 */
using string = std::pmr::string;
}  // namespace pmr

/**
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {

/**
 * @struct pmr_deleter
 * @brief Custom deleter for PMR-allocated objects.
 * @tparam T The managed type.
 */
template <typename T>
struct pmr_deleter {
    memory_resource* resource{nullptr};  ///< Memory resource used for allocation

    /**
     * @brief Default constructor.
     */
    pmr_deleter() noexcept = default;

    /**
     * @brief Constructs with a memory resource.
     * @param res The memory resource.
     */
    explicit pmr_deleter(memory_resource* res) noexcept : resource(res) {}

    /**
     * @brief Deletes the managed object.
     * @param ptr Pointer to the object to delete.
     */
    void operator()(T* ptr) const noexcept {
        if (ptr) {
            ptr->~T();
            if (resource) {
                resource->deallocate(ptr, sizeof(T), alignof(T));
            }
        }
    }
};

/**
 * @typedef pmr_unique_ptr
 * @brief Unique pointer with PMR deleter.
 * @tparam T The managed type.
 */
template <typename T>
using pmr_unique_ptr = std::unique_ptr<T, pmr_deleter<T>>;

/**
 * @brief Creates a PMR-managed unique pointer.
 * @tparam T The type to allocate.
 * @tparam Args Constructor argument types.
 * @param resource The memory resource to use.
 * @param args Constructor arguments.
 * @return A unique pointer managing the allocated object.
 */
template <typename T, typename... Args>
[[nodiscard]] auto make_pmr_unique(memory_resource* resource, Args&&... args) -> pmr_unique_ptr<T> {
    void* mem = resource->allocate(sizeof(T), alignof(T));
    try {
        auto* ptr = ::new (mem) T(std::forward<Args>(args)...);
        return pmr_unique_ptr<T>(ptr, pmr_deleter<T>(resource));
    } catch (...) {
        resource->deallocate(mem, sizeof(T), alignof(T));
        throw;
    }
}

}  // namespace detail

}  // namespace loom
