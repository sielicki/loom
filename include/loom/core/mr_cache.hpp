// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <variant>

#include "loom/core/allocator.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
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

struct host_cache_key {
    auto operator==(const host_cache_key&) const -> bool = default;
};

struct hmem_cache_key {
    hmem_iface iface;
    std::int64_t device;
    void* reserved;
    auto operator==(const hmem_cache_key&) const -> bool = default;
};

struct dmabuf_cache_key {
    std::uintptr_t allocation;
    int fd;
    std::uint64_t offset;
};

using mr_cache_identity = std::variant<host_cache_key, hmem_cache_key, dmabuf_cache_key>;

/**
 * @struct mr_cache_entry_base
 * @brief Base structure for memory region cache entries.
 */
struct mr_cache_entry_base {
    std::uintptr_t base_addr{0};           ///< Base address of registered region
    std::size_t length{0};                 ///< Length of registered region
    mr_access access{};                    ///< Access flags for the region
    mr_cache_identity identity{};          ///< Registration kind and allocation identity
    bool invalidated{false};               ///< Retained only for existing handles
    std::atomic<std::size_t> refcount{0};  ///< Reference count
    std::optional<memory_region> mr{};     ///< Cached memory region
    mr_cache_entry_base* index_parent{};
    mr_cache_entry_base* index_left{};
    mr_cache_entry_base* index_right{};
    std::uintptr_t index_end{};  ///< Greatest exclusive end in this subtree.
    int index_height{1};

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
        return addr >= base_addr && addr - base_addr <= length &&
               len <= length - (addr - base_addr);
    }

    /**
     * @brief Checks if this entry overlaps the given range.
     * @param addr Start address to check.
     * @param len Length to check.
     * @return True if the ranges overlap.
     */
    [[nodiscard]] auto overlaps(std::uintptr_t addr, std::size_t len) const noexcept -> bool {
        return length != 0 && len != 0 &&
               (addr < base_addr ? base_addr - addr < len : addr - base_addr < length);
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
     * @return The scalar key, or an error for invalid or raw-key regions.
     */
    [[nodiscard]] auto key() const noexcept -> result<std::uint64_t> {
        return entry_ && entry_->mr ? entry_->mr->key()
                                    : make_error_result<std::uint64_t>(errc::invalid_argument);
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
        return entry_ ? std::bit_cast<void*>(entry_->base_addr) : nullptr;
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
    [[nodiscard]] auto to_remote_memory() const noexcept -> result<remote_memory> {
        if (!entry_ || !entry_->mr) {
            return make_error_result<remote_memory>(errc::invalid_argument);
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
            cache_->release_entry(entry_);
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
 * The domain and optional endpoint must outlive this cache, which must outlive
 * every returned handle. Keep those wrappers unmoved and their native handles
 * unchanged until the cache is destroyed. Endpoint-bound registrations are
 * closed before their endpoint; retire all operations before clearing the cache.
 * Cached allocations and DMA-BUF descriptors must remain valid until their
 * registrations are evicted. Before freeing or remapping memory or reusing a
 * descriptor, invalidate overlapping entries and retire every affected handle.
 * Retention is explicit: clear() and evict_unreferenced() release idle entries.
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
        : domain_(&dom), domain_identity_(dom.internal_ptr()),
          resource_(resource ? resource : get_default_resource()) {}

    /// Scope registrations to one endpoint. FI_MR_ENDPOINT domains require
    /// this overload; each new host, HMEM, or DMA-BUF MR is bound and enabled
    /// before publication. External endpoint use still follows the negotiated
    /// provider threading rules; this cache serializes only its own accesses.
    mr_cache(domain& dom, endpoint& ep, memory_resource* resource = nullptr) noexcept
        : mr_cache(dom, resource) {
        endpoint_ = &ep;
        endpoint_identity_ = ep.internal_ptr();
    }

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
        auto uaddr = std::bit_cast<std::uintptr_t>(addr);
        constexpr auto last_aligned =
            traits::align_down(std::numeric_limits<std::uintptr_t>::max());
        if (addr == nullptr || length == 0 || uaddr > last_aligned ||
            length > last_aligned - uaddr) {
            return make_error_result<handle_type>(errc::invalid_argument);
        }

        {
            const std::shared_lock lock{mutex_};
            if (auto valid = validate_context(); !valid)
                return std::unexpected(valid.error());
            if (auto* entry = find_existing_entry(uaddr, length, access)) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return handle_type{this, entry};
            }
        }
        std::unique_lock lock(mutex_);
        if (auto valid = validate_context(); !valid)
            return std::unexpected(valid.error());

        if (auto* entry = find_existing_entry(uaddr, length, access)) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return handle_type{this, entry};
        }

        ++stats_.misses;

        auto aligned_base = traits::align_down(uaddr);
        auto aligned_length = traits::aligned_length(uaddr, length);

        auto entry =
            std::make_unique<detail::mr_cache_entry_base>(aligned_base, aligned_length, access);

        auto mr_result = memory_region::register_memory(
            *domain_, std::bit_cast<void*>(aligned_base), aligned_length, access, resource_);

        if (!mr_result) {
            return std::unexpected(mr_result.error());
        }

        if (auto ready = prepare_registration(*mr_result); !ready)
            return std::unexpected(ready.error());

        entry->mr = std::move(*mr_result);

        auto* entry_ptr = entry.get();
        entries_.emplace(aligned_base, std::move(entry));
        index_insert(*entry_ptr);

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
     * @param addr Allocation base address, or null for an unmapped DMA buffer.
     * @param length Buffer length.
     * @param access Required access permissions.
     * @param fd DMA buffer file descriptor.
     * @param offset Offset within the DMA buffer.
     * @return Handle to the cached region or error.
     * Reuses containing file ranges only for the same borrowed descriptor and
     * allocation base. The descriptor must not be closed while an MR is cached.
     */
    [[nodiscard]] auto lookup_dmabuf(void* addr,
                                     std::size_t length,
                                     mr_access access,
                                     int fd,
                                     std::uint64_t offset = 0) -> result<handle_type> {
        auto uaddr = std::bit_cast<std::uintptr_t>(addr);
        constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
        if (length == 0 || fd < 0 || length > std::numeric_limits<std::uint64_t>::max() - offset ||
            (addr && (offset > maximum - uaddr || length > maximum - (uaddr + offset)))) {
            return make_error_result<handle_type>(errc::invalid_argument);
        }
        const auto local = addr ? uaddr + static_cast<std::uintptr_t>(offset) : 0;
        const detail::dmabuf_cache_key identity{uaddr, fd, offset};

        {
            const std::shared_lock lock{mutex_};
            if (auto valid = validate_context(); !valid)
                return std::unexpected(valid.error());
            if (auto* entry = find_existing_entry(local, length, access, identity)) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return handle_type{this, entry};
            }
        }
        std::unique_lock lock(mutex_);
        if (auto valid = validate_context(); !valid)
            return std::unexpected(valid.error());

        if (auto* entry = find_existing_entry(local, length, access, identity)) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return handle_type{this, entry};
        }
        ++stats_.misses;

        auto entry = std::make_unique<detail::mr_cache_entry_base>(local, length, access);
        entry->identity = identity;

        auto mr_result = memory_region::register_dmabuf(
            *domain_, std::bit_cast<void*>(uaddr), length, access, fd, offset, resource_);

        if (!mr_result) {
            return std::unexpected(mr_result.error());
        }

        entry->base_addr = reinterpret_cast<std::uintptr_t>(mr_result->address());
        if (auto ready = prepare_registration(*mr_result); !ready)
            return std::unexpected(ready.error());

        entry->mr = std::move(*mr_result);

        auto* entry_ptr = entry.get();
        entries_.emplace(entry_ptr->base_addr, std::move(entry));
        index_insert(*entry_ptr);

        ++stats_.registrations;
        ++stats_.current_entries;
        stats_.total_registered_bytes += length;

        return handle_type{this, entry_ptr};
    }

    /**
     * @brief Looks up or registers heterogeneous memory.
     * @param addr Memory address.
     * @param length Buffer length.
     * @param access Required access permissions.
     * @param device The heterogeneous memory device.
     * @return Handle to the cached region or error.
     * Reuses containing ranges only for the same memory interface and device.
     * Device allocations are never rounded to host page boundaries.
     */
    [[nodiscard]] auto
    lookup_hmem(void* addr, std::size_t length, mr_access access, hmem_device device)
        -> result<handle_type> {
        auto uaddr = std::bit_cast<std::uintptr_t>(addr);
        if (length == 0 || length > std::numeric_limits<std::uintptr_t>::max() - uaddr) {
            return make_error_result<handle_type>(errc::invalid_argument);
        }

        const detail::hmem_cache_key identity{device.iface, device.device_id, device.hmem_data};
        {
            const std::shared_lock lock{mutex_};
            if (auto valid = validate_context(); !valid)
                return std::unexpected(valid.error());
            if (auto* entry = find_existing_entry(uaddr, length, access, identity)) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return handle_type{this, entry};
            }
        }
        std::unique_lock lock(mutex_);
        if (auto valid = validate_context(); !valid)
            return std::unexpected(valid.error());

        if (auto* entry = find_existing_entry(uaddr, length, access, identity)) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return handle_type{this, entry};
        }
        ++stats_.misses;

        auto entry = std::make_unique<detail::mr_cache_entry_base>(uaddr, length, access);
        entry->identity = identity;

        auto mr_result = memory_region::register_hmem(
            *domain_, std::bit_cast<void*>(uaddr), length, access, device, resource_);

        if (!mr_result) {
            return std::unexpected(mr_result.error());
        }

        if (auto ready = prepare_registration(*mr_result); !ready)
            return std::unexpected(ready.error());

        entry->mr = std::move(*mr_result);

        auto* entry_ptr = entry.get();
        entries_.emplace(uaddr, std::move(entry));
        index_insert(*entry_ptr);

        ++stats_.registrations;
        ++stats_.current_entries;
        stats_.total_registered_bytes += length;

        return handle_type{this, entry_ptr};
    }

    /**
     * @brief Invalidates cache entries overlapping a memory range.
     * Further lookups cannot reuse these entries. Existing handles retain their
     * registrations until the last handle is released; the backing memory must
     * remain valid until then. Statistics include these retained registrations.
     * @param addr Start address.
     * @param length Length of the range.
     */
    void invalidate(void* addr, std::size_t length) {
        auto uaddr = std::bit_cast<std::uintptr_t>(addr);

        std::unique_lock lock(mutex_);

        auto it = entries_.begin();
        while (it != entries_.end()) {
            auto& entry = *it->second;
            if (entry.overlaps(uaddr, length)) {
                if (!entry.invalidated)
                    index_remove(entry);
                entry.invalidated = true;
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
    void clear() { evict_unreferenced(); }

    /**
     * @brief Evicts all unreferenced entries.
     */
    void evict_unreferenced() {
        std::unique_lock lock(mutex_);

        auto it = entries_.begin();
        while (it != entries_.end()) {
            if (it->second->refcount.load(std::memory_order_acquire) == 0) {
                if (!it->second->invalidated)
                    index_remove(*it->second);
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
        auto snapshot = stats_;
        snapshot.hits = hits_.load(std::memory_order_relaxed);
        return snapshot;
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
        const auto hits = hits_.load(std::memory_order_relaxed);
        const auto total = hits + stats_.misses;
        return total > 0 ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
    }

private:
    friend class mr_handle<Provider>;
    using entry_type = detail::mr_cache_entry_base;

    static auto index_height(const entry_type* entry) noexcept -> int {
        return entry ? entry->index_height : 0;
    }

    static void index_update(entry_type& entry) noexcept {
        entry.index_height =
            1 + std::max(index_height(entry.index_left), index_height(entry.index_right));
        entry.index_end = entry.base_addr + entry.length;
        if (entry.index_left)
            entry.index_end = std::max(entry.index_end, entry.index_left->index_end);
        if (entry.index_right)
            entry.index_end = std::max(entry.index_end, entry.index_right->index_end);
    }

    void index_replace(entry_type& entry, entry_type* replacement) noexcept {
        if (!entry.index_parent)
            index_root_ = replacement;
        else if (entry.index_parent->index_left == &entry)
            entry.index_parent->index_left = replacement;
        else
            entry.index_parent->index_right = replacement;
        if (replacement)
            replacement->index_parent = entry.index_parent;
    }

    auto index_rotate_left(entry_type& entry) noexcept -> entry_type* {
        auto* next = entry.index_right;
        index_replace(entry, next);
        entry.index_right = next->index_left;
        if (entry.index_right)
            entry.index_right->index_parent = &entry;
        next->index_left = &entry;
        entry.index_parent = next;
        index_update(entry);
        index_update(*next);
        return next;
    }

    auto index_rotate_right(entry_type& entry) noexcept -> entry_type* {
        auto* next = entry.index_left;
        index_replace(entry, next);
        entry.index_left = next->index_right;
        if (entry.index_left)
            entry.index_left->index_parent = &entry;
        next->index_right = &entry;
        entry.index_parent = next;
        index_update(entry);
        index_update(*next);
        return next;
    }

    void index_rebalance(entry_type* entry) noexcept {
        while (entry) {
            index_update(*entry);
            const auto balance = index_height(entry->index_left) - index_height(entry->index_right);
            if (balance > 1) {
                if (index_height(entry->index_left->index_left) <
                    index_height(entry->index_left->index_right))
                    index_rotate_left(*entry->index_left);
                entry = index_rotate_right(*entry);
            } else if (balance < -1) {
                if (index_height(entry->index_right->index_right) <
                    index_height(entry->index_right->index_left))
                    index_rotate_right(*entry->index_right);
                entry = index_rotate_left(*entry);
            }
            entry = entry->index_parent;
        }
    }

    void index_insert(entry_type& entry) noexcept {
        auto** link = &index_root_;
        entry_type* parent{};
        while (*link) {
            parent = *link;
            const bool before =
                entry.base_addr < parent->base_addr ||
                (entry.base_addr == parent->base_addr && std::less<entry_type*>{}(&entry, parent));
            link = before ? &parent->index_left : &parent->index_right;
        }
        *link = &entry;
        entry.index_parent = parent;
        entry.index_end = entry.base_addr + entry.length;
        index_rebalance(parent);
    }

    void index_remove(entry_type& entry) noexcept {
        auto* parent = entry.index_parent;
        if (!entry.index_left || !entry.index_right) {
            index_replace(entry, entry.index_left ? entry.index_left : entry.index_right);
        } else {
            auto* next = entry.index_right;
            while (next->index_left)
                next = next->index_left;
            if (next->index_parent == &entry) {
                parent = next;
            } else {
                parent = next->index_parent;
                index_replace(*next, next->index_right);
                next->index_right = entry.index_right;
                next->index_right->index_parent = next;
            }
            index_replace(entry, next);
            next->index_left = entry.index_left;
            next->index_left->index_parent = next;
        }
        entry.index_parent = entry.index_left = entry.index_right = nullptr;
        index_rebalance(parent);
    }

    [[nodiscard]] auto validate_context() const noexcept -> void_result {
        if (!*domain_ || domain_->internal_ptr() != domain_identity_)
            return make_error_result<void>(errc::invalid_argument);
        if (endpoint_ &&
            (!endpoint_->belongs_to(*domain_) || endpoint_->internal_ptr() != endpoint_identity_))
            return make_error_result<void>(errc::invalid_argument);
        if (!endpoint_ && domain_->get_memory_region_mode().has(mr_mode_flags::endpoint))
            return make_error_result<void>(errc::invalid_argument);
        return make_success();
    }

    [[nodiscard]] auto prepare_registration(memory_region& region) -> void_result {
        if (!domain_->get_memory_region_mode().has(mr_mode_flags::endpoint))
            return make_success();
        if (auto bound = region.bind(*endpoint_); !bound)
            return bound;
        return region.enable();
    }

    /**
     * @brief Finds an existing entry containing the requested range.
     * @param addr Start address.
     * @param length Length of the range.
     * @param access Required access permissions.
     * @return Pointer to matching entry, or nullptr.
     */
    template <typename Key = detail::host_cache_key>
    [[nodiscard]] auto find_existing_entry(std::uintptr_t addr,
                                           std::size_t length,
                                           mr_access access,
                                           const Key& identity = {}) const noexcept
        -> detail::mr_cache_entry_base* {
        const auto matches = [&](const entry_type& entry) {
            if (!entry.contains(addr, length) || !entry.access.has(access))
                return false;
            const auto* cached = std::get_if<Key>(&entry.identity);
            if (!cached)
                return false;
            if constexpr (std::same_as<Key, detail::dmabuf_cache_key>) {
                // File offsets distinguish unmapped buffers, whose local addresses are null.
                if (cached->allocation != identity.allocation || cached->fd != identity.fd ||
                    identity.offset < cached->offset ||
                    identity.offset - cached->offset > entry.length ||
                    length > entry.length - (identity.offset - cached->offset))
                    return false;
            } else if (*cached != identity) {
                return false;
            }
            return true;
        };
        // The AVL index excludes invalidated registrations. Its subtree end
        // bounds prune earlier ranges that cannot contain this request.
        const auto find = [&](this auto& self, entry_type* entry) -> entry_type* {
            if (!entry || entry->index_end < addr + length)
                return nullptr;
            if (entry->base_addr > addr)
                return self(entry->index_left);
            if (auto* found = self(entry->index_right))
                return found;
            if (matches(*entry))
                return entry;
            return self(entry->index_left);
        };
        return find(index_root_);
    }

    /**
     * @brief Releases a handle and retires an invalidated entry's last reference.
     * @param entry The released entry.
     */
    void release_entry(detail::mr_cache_entry_base* entry) noexcept {
        {
            // Readers protect the entry until the decrement is finished. A
            // reusable registration stays cached even after its last handle.
            const std::shared_lock lock{mutex_};
            if (!entry->invalidated) {
                entry->refcount.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
        }
        // Retain our reference while switching locks so eviction cannot destroy
        // an invalidated entry before its final release acquires exclusive access.
        std::unique_lock lock(mutex_);
        if (entry->refcount.fetch_sub(1, std::memory_order_acq_rel) != 1 || !entry->invalidated)
            return;

        const auto [first, last] = entries_.equal_range(entry->base_addr);
        for (auto it = first; it != last; ++it) {
            if (it->second.get() == entry) {
                stats_.total_registered_bytes -= entry->length;
                --stats_.current_entries;
                ++stats_.evictions;
                entries_.erase(it);
                return;
            }
        }
    }

    domain* domain_;                   ///< Fabric domain
    void* domain_identity_;            ///< Native identity fixed at construction
    endpoint* endpoint_{};             ///< Optional endpoint registration scope
    void* endpoint_identity_{};
    memory_resource* resource_;        ///< Memory resource
    mutable std::shared_mutex mutex_;  ///< Lock for thread safety
    std::multimap<std::uintptr_t, std::unique_ptr<detail::mr_cache_entry_base>>
        entries_;           ///< Cache entries
    entry_type* index_root_{};  ///< Nonowning AVL index of reusable ranges.
    std::atomic<std::size_t> hits_{};
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

/// Create a cache scoped to one endpoint in the supplied domain.
template <provider_tag Provider>
[[nodiscard]] auto make_mr_cache(domain& dom, endpoint& ep, memory_resource* resource = nullptr)
    -> mr_cache<Provider> {
    return mr_cache<Provider>(dom, ep, resource);
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
