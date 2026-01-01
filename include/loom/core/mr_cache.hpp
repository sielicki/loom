// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

#include "loom/core/allocator.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace loom::detail
 * @brief Implementation details for loom.
 */
namespace detail {

/**
 * @struct mr_cache_entry_base
 * @brief Base structure for memory region cache entries.
 */
struct mr_cache_entry_base {
    std::uintptr_t base_addr{0};           ///< Base address of registered region
    std::size_t length{0};                 ///< Length of registered region
    mr_access access{};                    ///< Access flags for the region
    std::atomic<std::size_t> refcount{0};  ///< Reference count
    std::optional<memory_region> mr{};     ///< Cached memory region

    /**
     * @brief Default constructor.
     */
    mr_cache_entry_base() = default;

    /**
     * @brief Constructs from address, length, and access flags.
     * @param addr Base address.
     * @param len Region length.
     * @param acc Access flags.
     */
    mr_cache_entry_base(std::uintptr_t addr, std::size_t len, mr_access acc) noexcept
        : base_addr(addr), length(len), access(acc) {}

    /**
     * @brief Checks if this entry contains the given range.
     * @param addr Start address to check.
     * @param len Length to check.
     * @return True if the range is fully contained.
     */
    [[nodiscard]] auto contains(std::uintptr_t addr, std::size_t len) const noexcept -> bool {
        return addr >= base_addr && (addr + len) <= (base_addr + length);
    }

    /**
     * @brief Checks if this entry overlaps the given range.
     * @param addr Start address to check.
     * @param len Length to check.
     * @return True if the ranges overlap.
     */
    [[nodiscard]] auto overlaps(std::uintptr_t addr, std::size_t len) const noexcept -> bool {
        return addr < (base_addr + length) && (addr + len) > base_addr;
    }
};

}  // namespace detail

/**
 * @struct mr_cache_traits
 * @brief Traits for memory region cache alignment.
 * @tparam Provider The provider tag type.
 */
template <provider_tag Provider>
struct mr_cache_traits {
    using traits = provider_traits<Provider>;  ///< Provider traits

    static constexpr std::size_t default_page_size = 4096;       ///< Default page size
    static constexpr std::size_t page_size = default_page_size;  ///< Page size to use

    /**
     * @brief Aligns an address down to page boundary.
     * @param addr Address to align.
     * @return Page-aligned address.
     */
    [[nodiscard]] static constexpr auto align_down(std::uintptr_t addr) noexcept -> std::uintptr_t {
        return addr & ~(page_size - 1);
    }

    /**
     * @brief Aligns an address up to page boundary.
     * @param addr Address to align.
     * @return Page-aligned address.
     */
    [[nodiscard]] static constexpr auto align_up(std::uintptr_t addr) noexcept -> std::uintptr_t {
        return (addr + page_size - 1) & ~(page_size - 1);
    }

    /**
     * @brief Calculates the aligned length for a region.
     * @param addr Start address.
     * @param length Original length.
     * @return Aligned length covering the full page range.
     */
    [[nodiscard]] static constexpr auto aligned_length(std::uintptr_t addr,
                                                       std::size_t length) noexcept -> std::size_t {
        auto aligned_base = align_down(addr);
        auto end = addr + length;
        auto aligned_end = align_up(end);
        return aligned_end - aligned_base;
    }
};

/// Forward declaration of mr_cache
template <provider_tag Provider>
class mr_cache;

/**
 * @class mr_handle
 * @brief RAII handle for a cached memory region.
 *
 * Provides reference-counted access to a cached memory region.
 *
 * @tparam Provider The provider tag type.
 */
template <provider_tag Provider>
class mr_handle {
public:
    /**
     * @brief Default constructor.
     */
    mr_handle() noexcept = default;

    /**
     * @brief Copy constructor (increments reference count).
     */
    mr_handle(const mr_handle& other) noexcept : cache_(other.cache_), entry_(other.entry_) {
        if (entry_) {
            entry_->refcount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Copy assignment operator.
     */
    auto operator=(const mr_handle& other) noexcept -> mr_handle& {
        if (this != &other) {
            release();
            cache_ = other.cache_;
            entry_ = other.entry_;
            if (entry_) {
                entry_->refcount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return *this;
    }

    /**
     * @brief Move constructor.
     */
    mr_handle(mr_handle&& other) noexcept : cache_(other.cache_), entry_(other.entry_) {
        other.cache_ = nullptr;
        other.entry_ = nullptr;
    }

    /**
     * @brief Move assignment operator.
     */
    auto operator=(mr_handle&& other) noexcept -> mr_handle& {
        if (this != &other) {
            release();
            cache_ = other.cache_;
            entry_ = other.entry_;
            other.cache_ = nullptr;
            other.entry_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Destructor (decrements reference count).
     */
    ~mr_handle() { release(); }

    /**
     * @brief Checks if this handle references a valid memory region.
     * @return True if valid.
     */
    [[nodiscard]] auto valid() const noexcept -> bool {
        return entry_ != nullptr && entry_->mr.has_value();
    }

    /**
     * @brief Boolean conversion operator.
     * @return True if valid.
     */
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    /**
     * @brief Gets the underlying memory region.
     * @return Pointer to the memory region, or nullptr.
     */
    [[nodiscard]] auto mr() const noexcept -> const memory_region* {
        return entry_ && entry_->mr ? &*entry_->mr : nullptr;
    }

    /**
     * @brief Gets the memory region key.
     * @return The key, or 0 if invalid.
     */
    [[nodiscard]] auto key() const noexcept -> std::uint64_t {
        return entry_ && entry_->mr ? entry_->mr->key() : 0;
    }

    /**
     * @brief Gets the memory region descriptor.
     * @return The descriptor.
     */
    [[nodiscard]] auto descriptor() const noexcept -> mr_descriptor {
        return entry_ && entry_->mr ? entry_->mr->descriptor() : mr_descriptor{};
    }

    /**
     * @brief Gets the base address of the cached region.
     * @return The base address, or nullptr.
     */
    [[nodiscard]] auto base_address() const noexcept -> void* {
        return entry_ ? reinterpret_cast<void*>(entry_->base_addr) : nullptr;
    }

    /**
     * @brief Gets the length of the cached region.
     * @return The length in bytes.
     */
    [[nodiscard]] auto length() const noexcept -> std::size_t {
        return entry_ ? entry_->length : 0;
    }

    /**
     * @brief Gets the current reference count.
     * @return The reference count.
     */
    [[nodiscard]] auto refcount() const noexcept -> std::size_t {
        return entry_ ? entry_->refcount.load(std::memory_order_relaxed) : 0;
    }

    /**
     * @brief Converts to a remote memory descriptor.
     * @return The remote memory descriptor.
     */
    [[nodiscard]] auto to_remote_memory() const noexcept -> remote_memory {
        if (!entry_ || !entry_->mr) {
            return remote_memory{};
        }
        return remote_memory::from_mr(*entry_->mr);
    }

private:
    friend class mr_cache<Provider>;

    /**
     * @brief Private constructor for cache use.
     * @param cache The owning cache.
     * @param entry The cache entry.
     */
    mr_handle(mr_cache<Provider>* cache, detail::mr_cache_entry_base* entry) noexcept
        : cache_(cache), entry_(entry) {
        if (entry_) {
            entry_->refcount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Releases the handle reference.
     */
    void release() noexcept {
        if (entry_) {
            auto prev = entry_->refcount.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1 && cache_) {
                cache_->notify_entry_released(entry_);
            }
        }
        cache_ = nullptr;
        entry_ = nullptr;
    }

    mr_cache<Provider>* cache_{nullptr};           ///< Owning cache
    detail::mr_cache_entry_base* entry_{nullptr};  ///< Cache entry
};

/**
 * @struct mr_cache_stats
 * @brief Statistics for memory region cache performance.
 */
struct mr_cache_stats {
    std::size_t hits{0};                    ///< Number of cache hits
    std::size_t misses{0};                  ///< Number of cache misses
    std::size_t registrations{0};           ///< Total memory registrations
    std::size_t evictions{0};               ///< Number of evicted entries
    std::size_t current_entries{0};         ///< Current number of entries
    std::size_t total_registered_bytes{0};  ///< Total bytes currently registered
};

/**
 * @class mr_cache
 * @brief Thread-safe cache for memory region registrations.
 *
 * Provides caching of memory region registrations to avoid repeated
 * registration overhead. Uses page-aligned regions for efficient sharing.
 *
 * @tparam Provider The provider tag type.
 */
template <provider_tag Provider>
class mr_cache {
public:
    using handle_type = mr_handle<Provider>;   ///< Handle type for cache entries
    using traits = mr_cache_traits<Provider>;  ///< Traits for alignment

    /**
     * @brief Constructs a cache for a domain.
     * @param dom The fabric domain.
     * @param resource Optional memory resource for allocations.
     */
    explicit mr_cache(domain& dom, memory_resource* resource = nullptr) noexcept
        : domain_(&dom), resource_(resource ? resource : get_default_resource()) {}

    /// Deleted copy constructor
    mr_cache(const mr_cache&) = delete;
    /// Deleted copy assignment
    auto operator=(const mr_cache&) -> mr_cache& = delete;
    /// Deleted move constructor
    mr_cache(mr_cache&&) = delete;
    /// Deleted move assignment
    auto operator=(mr_cache&&) -> mr_cache& = delete;

    /**
     * @brief Destructor (clears all entries).
     */
    ~mr_cache() { clear(); }

    /**
     * @brief Looks up or registers a memory region.
     * @param addr Start address of the buffer.
     * @param length Length of the buffer.
     * @param access Required access permissions.
     * @return Handle to the cached region or error.
     */
    [[nodiscard]] auto lookup(void* addr, std::size_t length, mr_access access)
        -> result<handle_type> {
        auto uaddr = reinterpret_cast<std::uintptr_t>(addr);

        {
            std::shared_lock lock(mutex_);
            if (auto* entry = find_existing_entry(uaddr, length, access)) {
                ++stats_.hits;
                return handle_type{this, entry};
            }
        }

        std::unique_lock lock(mutex_);

        if (auto* entry = find_existing_entry(uaddr, length, access)) {
            ++stats_.hits;
            return handle_type{this, entry};
        }

        ++stats_.misses;

        auto aligned_base = traits::align_down(uaddr);
        auto aligned_length = traits::aligned_length(uaddr, length);

        auto entry =
            std::make_unique<detail::mr_cache_entry_base>(aligned_base, aligned_length, access);

        auto mr_result = memory_region::register_memory(
            *domain_, reinterpret_cast<void*>(aligned_base), aligned_length, access, resource_);

        if (!mr_result) {
            return std::unexpected(mr_result.error());
        }

        entry->mr = std::move(*mr_result);

        auto* entry_ptr = entry.get();
        entries_.emplace(aligned_base, std::move(entry));

        ++stats_.registrations;
        ++stats_.current_entries;
        stats_.total_registered_bytes += aligned_length;

        return handle_type{this, entry_ptr};
    }

    /**
     * @brief Looks up or registers a memory region from a span.
     * @param buffer The buffer span.
     * @param access Required access permissions.
     * @return Handle to the cached region or error.
     */
    [[nodiscard]] auto lookup(std::span<std::byte> buffer, mr_access access)
        -> result<handle_type> {
        return lookup(buffer.data(), buffer.size(), access);
    }

    /**
     * @brief Looks up or registers a DMA buffer.
     * @param addr Virtual address.
     * @param length Buffer length.
     * @param access Required access permissions.
     * @param fd DMA buffer file descriptor.
     * @param offset Offset within the DMA buffer.
     * @return Handle to the cached region or error.
     */
    [[nodiscard]] auto lookup_dmabuf(void* addr,
                                     std::size_t length,
                                     mr_access access,
                                     int fd,
                                     std::uint64_t offset = 0) -> result<handle_type> {
        auto uaddr = reinterpret_cast<std::uintptr_t>(addr);

        std::unique_lock lock(mutex_);

        auto aligned_base = traits::align_down(uaddr);
        auto aligned_length = traits::aligned_length(uaddr, length);

        auto entry =
            std::make_unique<detail::mr_cache_entry_base>(aligned_base, aligned_length, access);

        auto mr_result = memory_region::register_dmabuf(*domain_,
                                                        reinterpret_cast<void*>(aligned_base),
                                                        aligned_length,
                                                        access,
                                                        fd,
                                                        offset,
                                                        resource_);

        if (!mr_result) {
            return std::unexpected(mr_result.error());
        }

        entry->mr = std::move(*mr_result);

        auto* entry_ptr = entry.get();
        entries_.emplace(aligned_base, std::move(entry));

        ++stats_.registrations;
        ++stats_.current_entries;
        stats_.total_registered_bytes += aligned_length;

        return handle_type{this, entry_ptr};
    }

    /**
     * @brief Looks up or registers heterogeneous memory.
     * @param addr Memory address.
     * @param length Buffer length.
     * @param access Required access permissions.
     * @param device The heterogeneous memory device.
     * @return Handle to the cached region or error.
     */
    [[nodiscard]] auto
    lookup_hmem(void* addr, std::size_t length, mr_access access, hmem_device device)
        -> result<handle_type> {
        auto uaddr = reinterpret_cast<std::uintptr_t>(addr);

        std::unique_lock lock(mutex_);

        auto aligned_base = traits::align_down(uaddr);
        auto aligned_length = traits::aligned_length(uaddr, length);

        auto entry =
            std::make_unique<detail::mr_cache_entry_base>(aligned_base, aligned_length, access);

        auto mr_result = memory_region::register_hmem(*domain_,
                                                      reinterpret_cast<void*>(aligned_base),
                                                      aligned_length,
                                                      access,
                                                      device,
                                                      resource_);

        if (!mr_result) {
            return std::unexpected(mr_result.error());
        }

        entry->mr = std::move(*mr_result);

        auto* entry_ptr = entry.get();
        entries_.emplace(aligned_base, std::move(entry));

        ++stats_.registrations;
        ++stats_.current_entries;
        stats_.total_registered_bytes += aligned_length;

        return handle_type{this, entry_ptr};
    }

    /**
     * @brief Invalidates cache entries overlapping a memory range.
     * @param addr Start address.
     * @param length Length of the range.
     */
    void invalidate(void* addr, std::size_t length) {
        auto uaddr = reinterpret_cast<std::uintptr_t>(addr);

        std::unique_lock lock(mutex_);

        auto it = entries_.begin();
        while (it != entries_.end()) {
            auto& entry = *it->second;
            if (entry.overlaps(uaddr, length)) {
                if (entry.refcount.load(std::memory_order_acquire) == 0) {
                    stats_.total_registered_bytes -= entry.length;
                    --stats_.current_entries;
                    ++stats_.evictions;
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Clears all unreferenced cache entries.
     */
    void clear() {
        std::unique_lock lock(mutex_);

        for (auto& [addr, entry] : entries_) {
            if (entry->refcount.load(std::memory_order_acquire) == 0) {
                stats_.total_registered_bytes -= entry->length;
                ++stats_.evictions;
            }
        }

        entries_.clear();
        stats_.current_entries = 0;
    }

    /**
     * @brief Evicts all unreferenced entries.
     */
    void evict_unreferenced() {
        std::unique_lock lock(mutex_);

        auto it = entries_.begin();
        while (it != entries_.end()) {
            if (it->second->refcount.load(std::memory_order_acquire) == 0) {
                stats_.total_registered_bytes -= it->second->length;
                --stats_.current_entries;
                ++stats_.evictions;
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Gets cache statistics.
     * @return Current cache statistics.
     */
    [[nodiscard]] auto stats() const noexcept -> mr_cache_stats {
        std::shared_lock lock(mutex_);
        return stats_;
    }

    /**
     * @brief Gets the number of cache entries.
     * @return Number of entries.
     */
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        std::shared_lock lock(mutex_);
        return entries_.size();
    }

    /**
     * @brief Gets the cache hit rate.
     * @return Hit rate as a ratio (0.0 to 1.0).
     */
    [[nodiscard]] auto hit_rate() const noexcept -> double {
        std::shared_lock lock(mutex_);
        auto total = stats_.hits + stats_.misses;
        return total > 0 ? static_cast<double>(stats_.hits) / static_cast<double>(total) : 0.0;
    }

private:
    friend class mr_handle<Provider>;

    /**
     * @brief Finds an existing entry containing the requested range.
     * @param addr Start address.
     * @param length Length of the range.
     * @param access Required access permissions.
     * @return Pointer to matching entry, or nullptr.
     */
    [[nodiscard]] auto
    find_existing_entry(std::uintptr_t addr, std::size_t length, mr_access access) const noexcept
        -> detail::mr_cache_entry_base* {
        auto aligned_base = traits::align_down(addr);

        auto it = entries_.upper_bound(aligned_base);
        if (it != entries_.begin()) {
            --it;
        }

        while (it != entries_.end() && it->first <= addr) {
            auto& entry = *it->second;
            if (entry.contains(addr, length) && entry.access.has(access)) {
                return it->second.get();
            }
            ++it;
        }

        return nullptr;
    }

    /**
     * @brief Callback when an entry's reference count reaches zero.
     * @param entry The released entry.
     */
    void notify_entry_released([[maybe_unused]] detail::mr_cache_entry_base* entry) noexcept {}

    domain* domain_;                   ///< Fabric domain
    memory_resource* resource_;        ///< Memory resource
    mutable std::shared_mutex mutex_;  ///< Lock for thread safety
    std::map<std::uintptr_t, std::unique_ptr<detail::mr_cache_entry_base>>
        entries_;           ///< Cache entries
    mr_cache_stats stats_;  ///< Cache statistics
};

/**
 * @brief Creates a memory region cache for a domain.
 * @tparam Provider The provider tag type.
 * @param dom The fabric domain.
 * @param resource Optional memory resource.
 * @return A new mr_cache instance.
 */
template <provider_tag Provider>
[[nodiscard]] auto make_mr_cache(domain& dom, memory_resource* resource = nullptr)
    -> mr_cache<Provider> {
    return mr_cache<Provider>(dom, resource);
}

/**
 * @concept mr_cacheable
 * @brief Concept for types that satisfy the mr_cache interface.
 */
template <typename T>
concept mr_cacheable = requires(T& cache, void* addr, std::size_t len, mr_access acc) {
    { cache.lookup(addr, len, acc) } -> std::same_as<result<typename T::handle_type>>;
    { cache.invalidate(addr, len) };
    { cache.clear() };
    { cache.stats() } -> std::same_as<mr_cache_stats>;
};

static_assert(mr_cacheable<mr_cache<provider::verbs_tag>>);
static_assert(mr_cacheable<mr_cache<provider::efa_tag>>);

}  // namespace loom
