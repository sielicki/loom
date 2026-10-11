// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cuda.hpp"

namespace cuda = loom_examples::cuda;
namespace {
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                           \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)
int allocated_context, caller_context;
struct fixture {
    unsigned int memory_type{CU_MEMORYTYPE_DEVICE};
    unsigned int managed{};
    unsigned int rdma_capable{1};
    int device{7};
    CUcontext context{reinterpret_cast<CUcontext>(&allocated_context)};
    int supported{1};
    int ordering{CU_GPU_DIRECT_RDMA_WRITES_ORDERING_NONE};
    int options{CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_HOST};
    CUcontext current{reinterpret_cast<CUcontext>(&caller_context)};
    CUcontext previous{};
    CUresult query_error{CUDA_SUCCESS};
    CUresult sync_error{CUDA_SUCCESS};
    CUresult push_error{CUDA_SUCCESS};
    CUresult flush_error{CUDA_SUCCESS};
    CUresult pop_error{CUDA_SUCCESS};
    int synchronized{}, pushes{}, flushes{}, pops{};
} state;

cuda::api driver{
    .get_device =
        [](CUdevice* device, int ordinal) {
            CHECK(ordinal == state.device);
            *device = ordinal + 11;
            return CUDA_SUCCESS;
        },
    .device_attribute =
        [](int* value, CUdevice_attribute attribute, CUdevice device) {
            CHECK(device == state.device + 11);
            if (state.query_error != CUDA_SUCCESS)
                return state.query_error;
            switch (attribute) {
                case CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED:
                    *value = state.supported;
                    break;
                case CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WRITES_ORDERING:
                    *value = state.ordering;
                    break;
                case CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_FLUSH_WRITES_OPTIONS:
                    *value = state.options;
                    break;
                default:
                    CHECK(false);
            }
            return CUDA_SUCCESS;
        },
    .pointer_attributes =
        [](unsigned int count,
           CUpointer_attribute* attributes,
           void** values,
           CUdeviceptr pointer) {
            CHECK(pointer == 0x1000);
            if (state.query_error != CUDA_SUCCESS)
                return state.query_error;
            for (unsigned int i = 0; i < count; ++i) {
                switch (attributes[i]) {
                    case CU_POINTER_ATTRIBUTE_MEMORY_TYPE:
                        std::memcpy(values[i], &state.memory_type, sizeof(state.memory_type));
                        break;
                    case CU_POINTER_ATTRIBUTE_IS_MANAGED:
                        std::memcpy(values[i], &state.managed, sizeof(state.managed));
                        break;
                    case CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL:
                        std::memcpy(values[i], &state.device, sizeof(state.device));
                        break;
                    case CU_POINTER_ATTRIBUTE_CONTEXT:
                        std::memcpy(values[i], &state.context, sizeof(state.context));
                        break;
                    case CU_POINTER_ATTRIBUTE_IS_GPU_DIRECT_RDMA_CAPABLE:
                        std::memcpy(values[i], &state.rdma_capable, sizeof(state.rdma_capable));
                        break;
                    default:
                        CHECK(false);
                }
            }
            return CUDA_SUCCESS;
        },
    .set_pointer_attribute =
        [](const void* value, CUpointer_attribute attribute, CUdeviceptr pointer) {
            CHECK(attribute == CU_POINTER_ATTRIBUTE_SYNC_MEMOPS && pointer == 0x1000);
            CHECK(*static_cast<const unsigned int*>(value) == 1);
            ++state.synchronized;
            return state.sync_error;
        },
    .push_context =
        [](CUcontext context) {
            CHECK(context == state.context);
            ++state.pushes;
            if (state.push_error != CUDA_SUCCESS)
                return state.push_error;
            state.previous = state.current;
            state.current = context;
            return CUDA_SUCCESS;
        },
    .pop_context =
        [](CUcontext* context) {
            ++state.pops;
            *context = state.current;
            state.current = state.previous;
            return state.pop_error;
        },
    .flush =
        [](CUflushGPUDirectRDMAWritesTarget target, CUflushGPUDirectRDMAWritesScope scope) {
            CHECK(target == CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TARGET_CURRENT_CTX);
            CHECK(scope == CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TO_ALL_DEVICES);
            CHECK(state.current == state.context);
            ++state.flushes;
            return state.flush_error;
        },
};
template <typename Function>
void fails(Function&& function, CUresult expected) {
    try {
        function();
        CHECK(false);
    } catch (const cuda::error& error) {
        CHECK(error.code == expected);
    }
}
auto inspect() {
    return cuda::inspect(driver, reinterpret_cast<void*>(0x1000));
}
}  // namespace

int main() {
    const auto caller = state.current;
    const auto memory = inspect();
    CHECK(memory.device == 7 && memory.context == state.context);
    CHECK(memory.ordering == cuda::visibility::host_flush && state.synchronized == 1);
    cuda::flush(driver, memory);
    CHECK(state.current == caller && state.pushes == 1 && state.pops == 1 && state.flushes == 1);
    state.flush_error = CUDA_ERROR_UNKNOWN;
    fails([&] { cuda::flush(driver, memory); }, CUDA_ERROR_UNKNOWN);
    CHECK(state.current == caller && state.pushes == 2 && state.pops == 2);
    state.push_error = CUDA_ERROR_INVALID_CONTEXT;
    fails([&] { cuda::flush(driver, memory); }, CUDA_ERROR_INVALID_CONTEXT);
    CHECK(state.current == caller && state.pops == 2 && state.flushes == 2);

    state = {};
    state.ordering = CU_GPU_DIRECT_RDMA_WRITES_ORDERING_ALL_DEVICES;
    state.options = 0;
    auto no_flush_driver = driver;
    no_flush_driver.flush = nullptr;
    const auto ordered = cuda::inspect(no_flush_driver, reinterpret_cast<void*>(0x1000));
    cuda::flush(no_flush_driver, ordered);
    CHECK(state.pushes == 0 && state.flushes == 0 && state.pops == 0);
    state.ordering = CU_GPU_DIRECT_RDMA_WRITES_ORDERING_OWNER;
    fails(inspect,
          CUDA_ERROR_NOT_SUPPORTED);  // Owner ordering is weaker than our advertised scope.

    for (int failure = 0; failure < 8; ++failure) {
        state = {};
        switch (failure) {
            case 0:
                state.memory_type = CU_MEMORYTYPE_HOST;
                break;
            case 1:
                state.managed = 1;
                break;
            case 2:
                state.device = -1;
                break;
            case 3:
                state.context = nullptr;
                break;
            case 4:
                state.rdma_capable = 0;
                break;
            case 5:
                state.supported = 0;
                break;
            case 6:
                state.options = CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_MEMOPS;
                break;
            case 7:
                no_flush_driver = driver;
                no_flush_driver.flush = nullptr;
                break;
        }
        fails(
            [&] {
                (void)cuda::inspect(failure == 7 ? no_flush_driver : driver,
                                    reinterpret_cast<void*>(0x1000));
            },
            CUDA_ERROR_NOT_SUPPORTED);
        CHECK(state.synchronized == 0);
    }
    state = {};
    state.query_error = CUDA_ERROR_INVALID_VALUE;
    fails(inspect, CUDA_ERROR_INVALID_VALUE);
    fails([&] { (void)cuda::device_visibility(driver, state.device); }, CUDA_ERROR_INVALID_VALUE);
    CHECK(state.synchronized == 0);
    state = {};
    state.sync_error = CUDA_ERROR_INVALID_VALUE;
    fails(inspect, CUDA_ERROR_INVALID_VALUE);
    state = {};
    state.pop_error = CUDA_ERROR_INVALID_CONTEXT;
    fails([&] { cuda::flush(driver, memory); }, CUDA_ERROR_INVALID_CONTEXT);
    std::puts("CUDA policy: allocation identity, visibility scope and context restoration pass");
}
