// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>

#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <utility>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class request_context
 * @brief Context structure for tracking asynchronous fabric operations.
 *
 * Wraps a libfabric fi_context2 structure and provides user data storage.
 */
class request_context {
public:
    /**
     * @brief Default constructor.
     */
    request_context() noexcept = default;

    /**
     * @brief Deleted copy constructor.
     */
    request_context(const request_context&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const request_context&) -> request_context& = delete;

    /**
     * @brief Move constructor.
     */
    request_context(request_context&&) noexcept = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(request_context&&) noexcept -> request_context& = default;

    /**
     * @brief Destructor.
     */
    ~request_context() = default;

    /**
     * @brief Gets the raw context pointer for libfabric operations.
     * @return Pointer to the internal fi_context2.
     */
    [[nodiscard]] auto context_ptr() noexcept -> void* { return &fi_ctx_; }

    /**
     * @brief Gets the raw context pointer (const version).
     * @return Const pointer to the internal fi_context2.
     */
    [[nodiscard]] auto context_ptr() const noexcept -> const void* { return &fi_ctx_; }

    /**
     * @brief Sets user-defined data associated with this context.
     * @tparam T The user data type.
     * @param data Pointer to the user data.
     */
    template <typename T>
    void set_user_data(T* data) noexcept {
        user_data_ = data;
    }

    /**
     * @brief Gets the user-defined data.
     * @tparam T The expected user data type.
     * @return Pointer to the user data.
     */
    template <typename T>
    [[nodiscard]] auto user_data() const noexcept -> T* {
        return static_cast<T*>(user_data_);
    }

    /**
     * @brief Recovers the request_context from a libfabric context pointer.
     * @param ctx The fi_context pointer from a completion.
     * @return Pointer to the owning request_context, or null.
     */
    [[nodiscard]] static auto from_fi_context(void* ctx) noexcept -> request_context* {
        if (ctx == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<request_context*>(static_cast<char*>(ctx) -
                                                  offsetof(request_context, fi_ctx_));
    }

    /**
     * @brief Recovers the request_context from a const libfabric context pointer.
     * @param ctx The fi_context pointer from a completion.
     * @return Const pointer to the owning request_context, or null.
     */
    [[nodiscard]] static auto from_fi_context(const void* ctx) noexcept -> const request_context* {
        if (ctx == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<const request_context*>(static_cast<const char*>(ctx) -
                                                        offsetof(request_context, fi_ctx_));
    }

private:
    ::fi_context2 fi_ctx_{};    ///< Libfabric context structure
    void* user_data_{nullptr};  ///< User-defined data pointer
};

/**
 * @class typed_request_context
 * @brief Request context with embedded typed user data.
 * @tparam UserData The type of user data to embed.
 */
template <typename UserData>
class typed_request_context : public request_context {
public:
    using user_data_type = UserData;  ///< The embedded user data type

    /**
     * @brief Default constructor.
     */
    typed_request_context() noexcept = default;

    /**
     * @brief Constructs with initial user data.
     * @param data The user data to embed.
     */
    explicit typed_request_context(UserData data) noexcept(
        std::is_nothrow_move_constructible_v<UserData>)
        : data_(std::move(data)) {
        set_user_data(&data_);
    }

    /**
     * @brief Gets the embedded user data.
     * @return Reference to the user data.
     */
    [[nodiscard]] auto data() noexcept -> UserData& { return data_; }

    /**
     * @brief Gets the embedded user data (const version).
     * @return Const reference to the user data.
     */
    [[nodiscard]] auto data() const noexcept -> const UserData& { return data_; }

private:
    UserData data_;  ///< The embedded user data
};

/**
 * @class request_context_pool
 * @brief Pool for reusing request contexts.
 *
 * Manages a pool of request_context objects to avoid repeated allocations.
 */
class request_context_pool {
public:
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;  ///< Allocator type

    /**
     * @brief Constructs a pool with initial capacity.
     * @param initial_capacity Initial number of contexts to allocate.
     * @param resource Memory resource to use (default if null).
     */
    explicit request_context_pool(std::size_t initial_capacity = 64,
                                  std::pmr::memory_resource* resource = nullptr)
        : contexts_(initial_capacity, resource ? resource : std::pmr::get_default_resource()) {}

    /**
     * @brief Constructs a pool with a specific memory resource.
     * @param resource Memory resource to use.
     */
    explicit request_context_pool(std::pmr::memory_resource* resource) : contexts_(64, resource) {}

    /**
     * @brief Deleted copy constructor.
     */
    request_context_pool(const request_context_pool&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const request_context_pool&) -> request_context_pool& = delete;
    /**
     * @brief Move constructor.
     */
    request_context_pool(request_context_pool&&) = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(request_context_pool&&) -> request_context_pool& = default;
    /**
     * @brief Destructor.
     */
    ~request_context_pool() = default;

    /**
     * @brief Acquires a context from the pool.
     * @return Pointer to an available context.
     */
    [[nodiscard]] auto acquire() -> request_context* {
        for (auto& ctx : contexts_) {
            if (!ctx.in_use) {
                ctx.in_use = true;
                return &ctx.context;
            }
        }

        contexts_.emplace_back();
        contexts_.back().in_use = true;
        return &contexts_.back().context;
    }

    /**
     * @brief Releases a context back to the pool.
     * @param ctx The context to release.
     */
    void release(request_context* ctx) noexcept {
        for (auto& entry : contexts_) {
            if (&entry.context == ctx) {
                entry.in_use = false;
                entry.context = request_context{};
                return;
            }
        }
    }

    /**
     * @brief Gets the total pool size.
     * @return Number of contexts in the pool.
     */
    [[nodiscard]] auto size() const noexcept -> std::size_t { return contexts_.size(); }

    /**
     * @brief Gets the number of contexts currently in use.
     * @return Count of in-use contexts.
     */
    [[nodiscard]] auto in_use_count() const noexcept -> std::size_t {
        std::size_t count = 0;
        for (const auto& entry : contexts_) {
            if (entry.in_use) {
                ++count;
            }
        }
        return count;
    }

    /**
     * @brief Gets the allocator used by the pool.
     * @return The polymorphic allocator.
     */
    [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
        return contexts_.get_allocator();
    }

private:
    /**
     * @struct pool_entry
     * @brief Internal entry tracking context and usage state.
     */
    struct pool_entry {
        request_context context;  ///< The request context
        bool in_use{false};       ///< Whether this context is currently in use
    };

    std::pmr::vector<pool_entry> contexts_;  ///< Pool storage
};

/**
 * @brief Factory function for typed request contexts.
 * @tparam UserData The user data type.
 * @param data The user data to embed.
 * @return A typed request context containing the data.
 */
template <typename UserData>
[[nodiscard]] auto make_typed_request_context(UserData&& data)
    -> typed_request_context<std::decay_t<UserData>> {
    return typed_request_context<std::decay_t<UserData>>{std::forward<UserData>(data)};
}

}  // namespace loom
