// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cuda.hpp"

namespace loom_examples::cuda_test {
[[noreturn]] inline void fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    // A failed test can leave live DMA or a GPU verifier waiting for data.
    // Exit before stack unwinding could free its storage or synchronize it.
    std::_Exit(1);
}
inline void require(bool condition, const char* message) {
    if (!condition)
        fail(message);
}
inline void check(CUresult result, const char* operation) {
    if (result != CUDA_SUCCESS) {
        std::fprintf(stderr, "%s failed with CUDA error %d\n", operation, static_cast<int>(result));
        std::_Exit(1);
    }
}
using clock_type = std::chrono::steady_clock;
inline void tick(clock_type::time_point start) {
    require(clock_type::now() - start < std::chrono::seconds{30},
            "CUDA workload deadline exceeded");
    std::this_thread::yield();
}

// Test-only resources have explicit close: on failure the process exits with
// the entire allocation/context graph intact. Call close after deregistration.
class buffers {
    cuda::driver capabilities_;
    void* library_{dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL)};
    template <typename Function>
    auto symbol(const char* name) -> Function {
        require(library_, "load CUDA driver");
        auto* result = dlsym(library_, name);
        require(result, name);
        return reinterpret_cast<Function>(result);
    }
    const decltype(&cuDeviceGet) device_get_ = symbol<decltype(&cuDeviceGet)>("cuDeviceGet");
    const decltype(&cuDeviceGetAttribute) attribute_ =
        symbol<decltype(&cuDeviceGetAttribute)>("cuDeviceGetAttribute");
    const decltype(&cuDevicePrimaryCtxRetain) retain_ =
        symbol<decltype(&cuDevicePrimaryCtxRetain)>("cuDevicePrimaryCtxRetain");
    const decltype(&cuDevicePrimaryCtxRelease) release_ =
        symbol<decltype(&cuDevicePrimaryCtxRelease)>("cuDevicePrimaryCtxRelease_v2");
    const decltype(&cuCtxPushCurrent) push_ =
        symbol<decltype(&cuCtxPushCurrent)>("cuCtxPushCurrent_v2");
    const decltype(&cuCtxPopCurrent) pop_ =
        symbol<decltype(&cuCtxPopCurrent)>("cuCtxPopCurrent_v2");
    const decltype(&cuCtxSynchronize) context_sync_ =
        symbol<decltype(&cuCtxSynchronize)>("cuCtxSynchronize");
    const decltype(&cuMemAlloc) allocate_ = symbol<decltype(&cuMemAlloc)>("cuMemAlloc_v2");
    const decltype(&cuMemFree) free_ = symbol<decltype(&cuMemFree)>("cuMemFree_v2");
    const decltype(&cuMemcpyHtoD) copy_ = symbol<decltype(&cuMemcpyHtoD)>("cuMemcpyHtoD_v2");
    const decltype(&cuMemHostAlloc) host_allocate_ =
        symbol<decltype(&cuMemHostAlloc)>("cuMemHostAlloc");
    const decltype(&cuMemFreeHost) host_free_ = symbol<decltype(&cuMemFreeHost)>("cuMemFreeHost");
    const decltype(&cuMemHostGetDevicePointer) device_pointer_ =
        symbol<decltype(&cuMemHostGetDevicePointer)>("cuMemHostGetDevicePointer_v2");
    const decltype(&cuModuleLoadData) module_load_ =
        symbol<decltype(&cuModuleLoadData)>("cuModuleLoadData");
    const decltype(&cuModuleUnload) module_unload_ =
        symbol<decltype(&cuModuleUnload)>("cuModuleUnload");
    const decltype(&cuModuleGetFunction) function_ =
        symbol<decltype(&cuModuleGetFunction)>("cuModuleGetFunction");
    const decltype(&cuStreamCreate) stream_create_ =
        symbol<decltype(&cuStreamCreate)>("cuStreamCreate");
    const decltype(&cuStreamDestroy) stream_destroy_ =
        symbol<decltype(&cuStreamDestroy)>("cuStreamDestroy_v2");
    const decltype(&cuStreamSynchronize) synchronize_ =
        symbol<decltype(&cuStreamSynchronize)>("cuStreamSynchronize");
    const decltype(&cuLaunchKernel) launch_ = symbol<decltype(&cuLaunchKernel)>("cuLaunchKernel");

    struct control {
        std::uint32_t started{}, ready{}, result{};
    };
    static_assert(sizeof(control) == 3 * sizeof(std::uint32_t));
    static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
    CUdevice device_{};
    CUcontext context_{};
    CUdeviceptr source_{}, target_{}, device_flags_{};
    control* flags_{};
    CUmodule module_{};
    CUfunction verify_{};
    CUstream stream_{};
    bool running_{};

    struct current_context {
        buffers& owner;
        explicit current_context(buffers& memory) : owner(memory) {
            check(owner.push_(owner.context_), "activate workload CUDA context");
        }
        ~current_context() {
            CUcontext popped{};
            check(owner.pop_(&popped), "restore workload CUDA context");
            require(popped == owner.context_, "workload CUDA context stack mismatch");
        }
    };

public:
    static constexpr std::uint32_t words = 256 * 1024;
    static constexpr std::uint32_t guard_words = 64;
    static constexpr std::size_t bytes = words * sizeof(std::uint32_t);
    static constexpr std::size_t guard = guard_words * sizeof(std::uint32_t);
    static constexpr std::size_t allocation_size = bytes + 2 * guard;
    static constexpr std::uint32_t guard_value = 0xa55aa55a;

    buffers(int ordinal, const char* ptx_path) {
        check(device_get_(&device_, ordinal), "resolve workload CUDA device");
        int maps_host{}, major{};
        check(attribute_(&maps_host, CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY, device_),
              "query mapped host memory");
        check(attribute_(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device_),
              "query compute capability");
        require(maps_host && major >= 7,
                "GPU verifier requires mapped host memory and compute capability 7.0");
        check(retain_(&context_, device_), "retain workload CUDA context");
        const current_context current{*this};
        check(allocate_(&source_, allocation_size), "allocate GPU source");
        check(allocate_(&target_, allocation_size), "allocate GPU destination");
        std::vector<std::uint32_t> initial(words + 2 * guard_words, guard_value);
        for (std::uint32_t i = 0; i < words; ++i)
            initial[i + guard_words] = i ^ 0x5a5a5a5aU;
        check(copy_(source_, initial.data(), allocation_size), "initialize GPU source");
        check(host_allocate_(
                  reinterpret_cast<void**>(&flags_), sizeof(control), CU_MEMHOSTALLOC_DEVICEMAP),
              "allocate GPU verifier flags");
        std::construct_at(flags_);
        check(device_pointer_(&device_flags_, flags_, 0), "map GPU verifier flags");
        std::ifstream file{ptx_path};
        require(file.good(), "open GPU verifier PTX");
        const std::string ptx{std::istreambuf_iterator<char>{file},
                              std::istreambuf_iterator<char>{}};
        check(module_load_(&module_, ptx.c_str()), "load GPU verifier");
        check(function_(&verify_, module_, "verify_receive"), "resolve GPU verifier");
        check(stream_create_(&stream_, CU_STREAM_NON_BLOCKING), "create GPU verifier stream");
        // Initialization precedes every verifier and every network operation.
        check(context_sync_(), "finish GPU source initialization");
        reset_destination();
    }
    buffers(const buffers&) = delete;
    auto operator=(const buffers&) -> buffers& = delete;

    auto source() const -> void* { return reinterpret_cast<void*>(source_); }
    auto target() const -> void* { return reinterpret_cast<void*>(target_); }
    auto source_data() const -> void* { return reinterpret_cast<void*>(source_ + guard); }
    auto target_data() const -> void* { return reinterpret_cast<void*>(target_ + guard); }

    void reset_destination() {
        require(!running_, "cannot reset a running GPU verifier");
        const current_context current{*this};
        std::vector<std::uint32_t> initial(words + 2 * guard_words, guard_value);
        std::fill_n(initial.begin() + guard_words, words, 0);
        check(copy_(target_, initial.data(), allocation_size), "reset GPU destination");
        check(context_sync_(), "finish GPU destination initialization");
    }
    void start() {
        require(!running_, "GPU verifier already running");
        const current_context current{*this};
        *flags_ = {};
        auto count = words;
        auto guard_count = guard_words;
        std::array<void*, 4> parameters{&target_, &count, &guard_count, &device_flags_};
        check(launch_(verify_, 1, 1, 1, 1, 1, 1, 0, stream_, parameters.data(), nullptr),
              "start GPU verifier");
        running_ = true;
        const auto start = clock_type::now();
        while (!std::atomic_ref{flags_->started}.load(std::memory_order_acquire))
            tick(start);
    }
    void finish() {
        require(running_, "GPU verifier was not started");
        // No CUDA API before the verifier reports its result: another work
        // submission or synchronization could hide a missing transport flush.
        std::atomic_ref{flags_->ready}.store(1, std::memory_order_release);
        const auto start = clock_type::now();
        while (!std::atomic_ref{flags_->result}.load(std::memory_order_acquire))
            tick(start);
        require(std::atomic_ref{flags_->result}.load(std::memory_order_acquire) == 1,
                "GPU payload or guard mismatch");
        const current_context current{*this};
        check(synchronize_(stream_), "retire GPU verifier");
        running_ = false;
    }
    void close() {
        require(!running_ && library_, "cannot close an active or closed GPU workload");
        {
            const current_context current{*this};
            check(stream_destroy_(stream_), "destroy GPU verifier stream");
            check(module_unload_(module_), "unload GPU verifier");
            check(host_free_(flags_), "free GPU verifier flags");
            check(free_(source_), "free GPU source");
            check(free_(target_), "free GPU destination");
        }
        check(release_(device_), "release workload CUDA context");
        dlclose(std::exchange(library_, nullptr));
    }
};
}  // namespace loom_examples::cuda_test
