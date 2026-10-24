// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace loom {

inline constexpr std::uint64_t context_magic = 0xC0FFEE42DEADBEEFULL;
inline constexpr std::uint64_t context_invalid_magic = 0xDEADDEADDEADDEADULL;

#ifdef NDEBUG
inline constexpr bool context_validation_enabled = false;
#else
inline constexpr bool context_validation_enabled = true;
#endif

/// Application metadata must follow, never overlap, provider-owned storage.
/// Keep this object alive and at a stable address until completion is consumed.
struct safe_context {
    ::fi_context2 provider{};
    void* object{};
    std::uint64_t magic{context_invalid_magic};
};

static_assert(std::is_standard_layout_v<safe_context>);
static_assert(offsetof(safe_context, provider) == 0);

template <typename T>
void store_safe_context(safe_context& ctx, T* ptr) noexcept {
    ctx.object = ptr;
    ctx.magic = context_magic;
}

/// ctx must be null or point to the provider member of a live safe_context.
/// Validation cannot detect dangling pointers or arbitrary foreign contexts.
template <typename T>
[[nodiscard]] auto recover_safe_context(void* ctx) noexcept -> T* {
    if (!ctx) {
        return nullptr;
    }
    auto* storage = static_cast<safe_context*>(ctx);
    if constexpr (context_validation_enabled) {
        assert(storage->magic == context_magic && "invalidated operation context");
        if (storage->magic != context_magic) {
            return nullptr;
        }
    }
    return static_cast<T*>(storage->object);
}

inline void invalidate_safe_context(safe_context& ctx) noexcept {
    ctx.object = nullptr;
    ctx.magic = context_invalid_magic;
}

/// ctx must be null or refer to a live safe_context, as for recovery.
[[nodiscard]] inline auto is_context_valid(void* ctx) noexcept -> bool {
    return ctx && static_cast<safe_context*>(ctx)->magic == context_magic;
}

}  // namespace loom
