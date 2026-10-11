// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <cstdint>

#ifdef LOOM_MONARCH_CUDA
#    include <new>
#    include <utility>

#    include "../hmem/cuda.hpp"

namespace {
template <typename Function>
auto guarded(Function&& function) noexcept -> int {
    try {
        std::forward<Function>(function)();
        return CUDA_SUCCESS;
    } catch (const loom_examples::cuda::error& error) {
        return error.code;
    } catch (const std::bad_alloc&) {
        return CUDA_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return CUDA_ERROR_UNKNOWN;
    }
}
}  // namespace
#endif

// Rust's repr(C) representation contains only C ABI fields. The context is
// borrowed from the allocation owner; this bridge never retains a primary
// context or substitutes device zero for an unknown allocation.
extern "C" {
struct loom_cuda_allocation {
    void* context;
    std::int32_t device;
    bool host_flush;
};

#ifdef LOOM_MONARCH_CUDA
auto loom_cuda_open(void** output) noexcept -> int {
    if (!output)
        return CUDA_ERROR_INVALID_VALUE;
    *output = nullptr;
    return guarded([&] { *output = new loom_examples::cuda::driver{}; });
}

void loom_cuda_close(void* driver) noexcept {
    delete static_cast<loom_examples::cuda::driver*>(driver);
}

auto loom_cuda_inspect(const void* driver,
                       std::uintptr_t address,
                       loom_cuda_allocation* output) noexcept -> int {
    if (!output)
        return CUDA_ERROR_INVALID_VALUE;
    *output = {};
    if (!driver || !address)
        return CUDA_ERROR_INVALID_VALUE;
    return guarded([&] {
        const auto& api = static_cast<const loom_examples::cuda::driver*>(driver)->functions;
        const auto memory = loom_examples::cuda::inspect(api, reinterpret_cast<void*>(address));
        *output = {.context = memory.context,
                   .device = memory.device,
                   .host_flush = memory.ordering == loom_examples::cuda::visibility::host_flush};
    });
}

auto loom_cuda_flush(const void* driver, const loom_cuda_allocation* memory) noexcept -> int {
    if (!driver || !memory || !memory->context || memory->device < 0)
        return CUDA_ERROR_INVALID_VALUE;
    return guarded([&] {
        const auto& api = static_cast<const loom_examples::cuda::driver*>(driver)->functions;
        loom_examples::cuda::flush(
            api,
            {.device = memory->device,
             .context = static_cast<CUcontext>(memory->context),
             .ordering = memory->host_flush ? loom_examples::cuda::visibility::host_flush
                                            : loom_examples::cuda::visibility::native_ordering});
    });
}
#else
// A ROCm consumer can still use the host Loom profile. This CUDA-specific
// policy must reject activation when it was built without the CUDA SDK.
auto loom_cuda_open(void** output) noexcept -> int {
    if (output)
        *output = nullptr;
    return -1;
}
void loom_cuda_close(void*) noexcept {}
auto loom_cuda_inspect(const void*, std::uintptr_t, loom_cuda_allocation* output) noexcept -> int {
    if (output)
        *output = {};
    return -1;
}
auto loom_cuda_flush(const void*, const loom_cuda_allocation*) noexcept -> int {
    return -1;
}
#endif
}
