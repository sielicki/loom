// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file safe_context.hpp
 * @brief Safe context pointer storage and recovery for async operations.
 *
 * This header provides infrastructure for safely storing and recovering
 * object pointers through libfabric's fi_context2 structure. In debug builds,
 * it validates that recovered pointers have not been invalidated (e.g., by
 * object destruction), helping detect use-after-free bugs.
 *
 * ## Usage
 *
 * ```cpp
 * class my_operation {
 *     ::fi_context2 fi_ctx_{};
 *
 *     my_operation() {
 *         loom::store_safe_context(fi_ctx_, this);
 *     }
 *
 *     ~my_operation() {
 *         loom::invalidate_safe_context(fi_ctx_);
 *     }
 *
 *     static auto from_context(void* ctx) -> my_operation* {
 *         return loom::recover_safe_context<my_operation>(ctx);
 *     }
 * };
 * ```
 */

#include <rdma/fabric.h>

#include <bit>
#include <cassert>
#include <cstdint>

namespace loom {

/// Magic value used to validate context pointers in debug builds
inline constexpr std::uint64_t context_magic = 0xC0FFEE42DEADBEEFULL;

/// Magic value indicating an invalidated context
inline constexpr std::uint64_t context_invalid_magic = 0xDEADDEADDEADDEADULL;

#ifdef NDEBUG
/// Whether context validation is enabled (debug builds only)
inline constexpr bool context_validation_enabled = false;
#else
/// Whether context validation is enabled (debug builds only)
inline constexpr bool context_validation_enabled = true;
#endif

/**
 * @brief Stores a pointer in fi_context2 with optional validation magic.
 *
 * In debug builds, this stores a magic value in internal[1] that can be
 * checked during recovery to detect use-after-free.
 *
 * @tparam T The type of object being stored.
 * @param fi_ctx The fi_context2 structure to store in.
 * @param ptr The pointer to store.
 */
template <typename T>
void store_safe_context(::fi_context2& fi_ctx, T* ptr) noexcept {
    fi_ctx.internal[0] = static_cast<void*>(ptr);
    if constexpr (context_validation_enabled) {
        // Store magic in internal[1] for validation
        fi_ctx.internal[1] = std::bit_cast<void*>(context_magic);
    }
}

/**
 * @brief Recovers a pointer from fi_context2 with optional validation.
 *
 * In debug builds, this validates the magic value before returning the
 * pointer. If validation fails (e.g., the context was invalidated), this
 * returns nullptr.
 *
 * @tparam T The expected type of the stored object.
 * @param ctx The context pointer from a completion event (points to fi_context2).
 * @return The recovered pointer, or nullptr if ctx is null or validation fails.
 */
template <typename T>
[[nodiscard]] auto recover_safe_context(void* ctx) noexcept -> T* {
    if (ctx == nullptr) {
        return nullptr;
    }

    auto* fi_ctx = static_cast<::fi_context2*>(ctx);

    if constexpr (context_validation_enabled) {
        auto magic = std::bit_cast<std::uint64_t>(fi_ctx->internal[1]);
        if (magic != context_magic) {
            // Context has been invalidated or corrupted
            assert(false && "recover_safe_context: invalid or corrupted context - possible use-after-free");
            return nullptr;
        }
    }

    return static_cast<T*>(fi_ctx->internal[0]);
}

/**
 * @brief Invalidates a context to detect use-after-free in debug builds.
 *
 * Call this in destructors to ensure that any later attempt to recover
 * the context will fail validation.
 *
 * @param fi_ctx The fi_context2 structure to invalidate.
 */
inline void invalidate_safe_context(::fi_context2& fi_ctx) noexcept {
    if constexpr (context_validation_enabled) {
        fi_ctx.internal[0] = nullptr;
        fi_ctx.internal[1] = std::bit_cast<void*>(context_invalid_magic);
    }
}

/**
 * @brief Checks if a context appears valid (debug builds only).
 *
 * @param ctx The context pointer to check.
 * @return True if the context appears valid, false otherwise.
 */
[[nodiscard]] inline auto is_context_valid(void* ctx) noexcept -> bool {
    if (ctx == nullptr) {
        return false;
    }

    if constexpr (context_validation_enabled) {
        auto* fi_ctx = static_cast<::fi_context2*>(ctx);
        auto magic = std::bit_cast<std::uint64_t>(fi_ctx->internal[1]);
        return magic == context_magic;
    }

    return true;  // In release builds, assume valid
}

}  // namespace loom
