// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>

#include <cuda.h>
#include <dlfcn.h>

namespace loom_nccl::cuda {
class error final : public std::runtime_error {
public:
    CUresult code;
    error(CUresult result, const char* operation)
        : std::runtime_error(
              std::format("{} failed with CUDA error {}", operation, static_cast<int>(result))),
          code(result) {}
};
inline void check(CUresult result, const char* operation) {
    if (result != CUDA_SUCCESS)
        throw error(result, operation);
}

// Function signatures come from the pinned CUDA SDK. The injectable table
// tests policy and context restoration without pretending to execute a GPU.
struct api {
    decltype(&cuDeviceGet) get_device{};
    decltype(&cuDeviceGetAttribute) device_attribute{};
    decltype(&cuPointerGetAttributes) pointer_attributes{};
    decltype(&cuPointerSetAttribute) set_pointer_attribute{};
    decltype(&cuCtxPushCurrent) push_context{};
    decltype(&cuCtxPopCurrent) pop_context{};
    decltype(&cuFlushGPUDirectRDMAWrites) flush{};
};
enum class visibility { native_ordering, host_flush };
struct allocation {
    int device{};
    CUcontext context{};  // Borrowed, like the registered CUDA allocation itself.
    visibility ordering{};
};

inline auto device_visibility(const api& driver, int device) -> visibility {
    CUdevice handle{};
    check(driver.get_device(&handle, device), "resolve CUDA device ordinal");
    int supported{}, ordering{}, options{};
    check(
        driver.device_attribute(&supported, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED, handle),
        "query GPUDirect support");
    check(driver.device_attribute(
              &ordering, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WRITES_ORDERING, handle),
          "query GPUDirect ordering");
    if (!supported)
        throw error(CUDA_ERROR_NOT_SUPPORTED, "GPUDirect memory registration");
    if (ordering >= CU_GPU_DIRECT_RDMA_WRITES_ORDERING_ALL_DEVICES)
        return visibility::native_ordering;
    check(driver.device_attribute(
              &options, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_FLUSH_WRITES_OPTIONS, handle),
          "query GPUDirect flush support");
    if (!(options & CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_HOST) || !driver.flush)
        throw error(CUDA_ERROR_NOT_SUPPORTED, "GPUDirect receive visibility");
    return visibility::host_flush;
}

inline auto inspect(const api& driver, void* address) -> allocation {
    unsigned int memory_type{}, managed{}, rdma_capable{};
    allocation result;
    std::array attributes{
        CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
        CU_POINTER_ATTRIBUTE_IS_MANAGED,
        CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL,
        CU_POINTER_ATTRIBUTE_CONTEXT,
        CU_POINTER_ATTRIBUTE_IS_GPU_DIRECT_RDMA_CAPABLE,
    };
    std::array<void*, attributes.size()> values{
        &memory_type,
        &managed,
        &result.device,
        &result.context,
        &rdma_capable,
    };
    check(driver.pointer_attributes(attributes.size(),
                                    attributes.data(),
                                    values.data(),
                                    reinterpret_cast<CUdeviceptr>(address)),
          "classify CUDA allocation");
    // This direct-transfer profile cannot safely service managed memory or
    // allocations whose owning context is unknown. Never guess device zero.
    if (memory_type != CU_MEMORYTYPE_DEVICE || managed || !rdma_capable || !result.context ||
        result.device < 0)
        throw error(CUDA_ERROR_NOT_SUPPORTED, "CUDA allocation contract");
    result.ordering = device_visibility(driver, result.device);
    const unsigned int synchronous = 1;
    check(driver.set_pointer_attribute(&synchronous,
                                       CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                       reinterpret_cast<CUdeviceptr>(address)),
          "enable synchronous CUDA memory operations");
    return result;
}

inline void flush(const api& driver, const allocation& memory) {
    if (memory.ordering == visibility::native_ordering)
        return;
    check(driver.push_context(memory.context), "activate CUDA allocation context");
    const auto flushed = driver.flush(CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TARGET_CURRENT_CTX,
                                      CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TO_ALL_DEVICES);
    CUcontext popped{};
    const auto restored = driver.pop_context(&popped);
    // Restore even when flushing fails. Never synchronize the whole device:
    // NCCL may have a running kernel waiting for the receive to become visible.
    check(restored, "restore CUDA caller context");
    if (popped != memory.context)
        throw error(CUDA_ERROR_INVALID_CONTEXT, "restore CUDA context stack");
    check(flushed, "flush GPUDirect receive writes");
}

class driver {
    struct close_library {
        void operator()(void* library) const noexcept {
            if (library)
                dlclose(library);
        }
    };
    std::unique_ptr<void, close_library> library_;

    template <typename Function>
    auto symbol(const char* name) const -> Function {
        auto* pointer = dlsym(library_.get(), name);
        if (!pointer)
            throw error(CUDA_ERROR_NOT_SUPPORTED, name);
        return reinterpret_cast<Function>(pointer);
    }

public:
    api functions;

    driver() : library_(dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL)) {
        if (!library_)
            throw error(CUDA_ERROR_NOT_INITIALIZED, "load CUDA driver");
        const auto init = symbol<decltype(&cuInit)>("cuInit");
        const auto count_devices = symbol<decltype(&cuDeviceGetCount)>("cuDeviceGetCount");
        functions = {
            .get_device = symbol<decltype(&cuDeviceGet)>("cuDeviceGet"),
            .device_attribute = symbol<decltype(&cuDeviceGetAttribute)>("cuDeviceGetAttribute"),
            .pointer_attributes =
                symbol<decltype(&cuPointerGetAttributes)>("cuPointerGetAttributes"),
            .set_pointer_attribute =
                symbol<decltype(&cuPointerSetAttribute)>("cuPointerSetAttribute"),
            .push_context = symbol<decltype(&cuCtxPushCurrent)>("cuCtxPushCurrent_v2"),
            .pop_context = symbol<decltype(&cuCtxPopCurrent)>("cuCtxPopCurrent_v2"),
            // Native ordering does not require the optional flush entry point.
            .flush = reinterpret_cast<decltype(&cuFlushGPUDirectRDMAWrites)>(
                dlsym(library_.get(), "cuFlushGPUDirectRDMAWrites")),
        };
        check(init(0), "initialize CUDA driver");
        int count{};
        check(count_devices(&count), "enumerate CUDA devices");
        if (count <= 0)
            throw error(CUDA_ERROR_NO_DEVICE, "enumerate CUDA devices");
        // NCCL properties describe the network adapter, not a particular GPU.
        // The advertised CUDA contract must therefore hold for every visible GPU.
        for (int device = 0; device < count; ++device)
            (void)device_visibility(functions, device);
    }
};
}  // namespace loom_nccl::cuda
