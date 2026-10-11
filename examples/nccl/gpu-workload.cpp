// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nccl_net.h>

#include "cuda.hpp"

namespace cuda = loom_nccl::cuda;
namespace {
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error(std::string{#condition} + " at line " +                       \
                                     std::to_string(__LINE__));                                    \
    } while (false)
#define OK(expression) CHECK((expression) == ncclSuccess)
#define CUDA(expression) cuda::check((expression), #expression)
using clock_type = std::chrono::steady_clock;
void tick(clock_type::time_point start) {
    CHECK(clock_type::now() - start < std::chrono::seconds{30});
    std::this_thread::yield();
}
template <typename Function>
auto symbol(void* library, const char* name) -> Function {
    auto* result = dlsym(library, name);
    CHECK(result);
    return reinterpret_cast<Function>(result);
}
struct control {
    std::uint32_t started{}, ready{}, result{};
};
static_assert(sizeof(control) == 3 * sizeof(std::uint32_t));
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
}  // namespace

int main(int argc, char** argv) {
    try {
        // libnccl-net-loom.so, visibility.ptx, GPU ordinal, provider, domain
        CHECK(argc == 6);
        int ordinal{};
        const std::string_view text{argv[3]};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ordinal);
        CHECK(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && ordinal >= 0);
        CHECK(setenv("NCCL_LOOM_CUDA", "1", 1) == 0);
        CHECK(setenv("NCCL_LOOM_PROVIDER", argv[4], 1) == 0);
        CHECK(setenv("NCCL_LOOM_DOMAIN", argv[5], 1) == 0);
        cuda::driver capabilities;
        void* driver = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        CHECK(driver);
        const auto device_get = symbol<decltype(&cuDeviceGet)>(driver, "cuDeviceGet");
        const auto attribute =
            symbol<decltype(&cuDeviceGetAttribute)>(driver, "cuDeviceGetAttribute");
        const auto retain =
            symbol<decltype(&cuDevicePrimaryCtxRetain)>(driver, "cuDevicePrimaryCtxRetain");
        const auto release =
            symbol<decltype(&cuDevicePrimaryCtxRelease)>(driver, "cuDevicePrimaryCtxRelease_v2");
        const auto push = symbol<decltype(&cuCtxPushCurrent)>(driver, "cuCtxPushCurrent_v2");
        const auto pop = symbol<decltype(&cuCtxPopCurrent)>(driver, "cuCtxPopCurrent_v2");
        const auto allocate = symbol<decltype(&cuMemAlloc)>(driver, "cuMemAlloc_v2");
        const auto free = symbol<decltype(&cuMemFree)>(driver, "cuMemFree_v2");
        const auto copy = symbol<decltype(&cuMemcpyHtoD)>(driver, "cuMemcpyHtoD_v2");
        const auto host_allocate = symbol<decltype(&cuMemHostAlloc)>(driver, "cuMemHostAlloc");
        const auto host_free = symbol<decltype(&cuMemFreeHost)>(driver, "cuMemFreeHost");
        const auto device_pointer =
            symbol<decltype(&cuMemHostGetDevicePointer)>(driver, "cuMemHostGetDevicePointer_v2");
        const auto module_load = symbol<decltype(&cuModuleLoadData)>(driver, "cuModuleLoadData");
        const auto module_unload = symbol<decltype(&cuModuleUnload)>(driver, "cuModuleUnload");
        const auto function = symbol<decltype(&cuModuleGetFunction)>(driver, "cuModuleGetFunction");
        const auto stream_create = symbol<decltype(&cuStreamCreate)>(driver, "cuStreamCreate");
        const auto stream_destroy =
            symbol<decltype(&cuStreamDestroy)>(driver, "cuStreamDestroy_v2");
        const auto synchronize =
            symbol<decltype(&cuStreamSynchronize)>(driver, "cuStreamSynchronize");
        const auto launch = symbol<decltype(&cuLaunchKernel)>(driver, "cuLaunchKernel");
        CUdevice device{};
        CUDA(device_get(&device, ordinal));
        int maps_host{}, major{};
        CUDA(attribute(&maps_host, CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY, device));
        CUDA(attribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device));
        CHECK(maps_host && major >= 7);
        CUcontext context{};
        CUDA(retain(&context, device));
        CUDA(push(context));
        auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        CHECK(library);
        auto* net = static_cast<ncclNet_v12_t*>(dlsym(library, "ncclNetPlugin_v12"));
        CHECK(net);
        ncclNetProperties_v12_t props{};
        OK(net->getProperties(0, &props));
        CHECK((props.ptrSupport & NCCL_PTR_CUDA) && props.forceFlush);
        void* ctx{};
        OK(net->init(&ctx, 1, nullptr, nullptr, nullptr));
        std::array<std::byte, NCCL_NET_HANDLE_MAXSIZE> wire{};
        void *listener{}, *send{}, *receive{};
        OK(net->listen(ctx, 0, wire.data(), &listener));
        auto start = clock_type::now();
        while (!send || !receive) {
            if (!send)
                OK(net->connect(ctx, 0, wire.data(), &send, nullptr));
            if (!receive)
                OK(net->accept(listener, &receive, nullptr));
            tick(start);
        }
        OK(net->closeListen(listener));
        constexpr std::uint32_t words = 256 * 1024;
        constexpr std::size_t bytes = words * sizeof(std::uint32_t);
        constexpr std::size_t guard = 256;
        CUdeviceptr source{}, target{};
        CUDA(allocate(&source, bytes + 2 * guard));
        CUDA(allocate(&target, bytes + 2 * guard));
        std::vector<std::uint32_t> initial(words + 2 * guard / sizeof(std::uint32_t));
        CUDA(copy(target, initial.data(), initial.size() * sizeof(std::uint32_t)));
        for (std::uint32_t i = 0; i < words; ++i)
            initial[i + guard / sizeof(std::uint32_t)] = i ^ 0x5a5a5a5aU;
        CUDA(copy(source, initial.data(), initial.size() * sizeof(std::uint32_t)));
        void *source_mr{}, *target_mr{};
        OK(net->regMr(
            send, reinterpret_cast<void*>(source), bytes + 2 * guard, NCCL_PTR_CUDA, &source_mr));
        OK(net->regMr(receive,
                      reinterpret_cast<void*>(target),
                      bytes + 2 * guard,
                      NCCL_PTR_CUDA,
                      &target_mr));
        control* flags{};
        CUDA(host_allocate(
            reinterpret_cast<void**>(&flags), sizeof(control), CU_MEMHOSTALLOC_DEVICEMAP));
        std::construct_at(flags);
        CUdeviceptr device_flags{};
        CUDA(device_pointer(&device_flags, flags, 0));
        std::ifstream file{argv[2]};
        CHECK(file.good());
        const std::string ptx{std::istreambuf_iterator<char>{file},
                              std::istreambuf_iterator<char>{}};
        CUmodule module{};
        CUDA(module_load(&module, ptx.c_str()));
        CUfunction verify{};
        CUDA(function(&verify, module, "verify_receive"));
        CUstream stream{};
        CUDA(stream_create(&stream, CU_STREAM_NON_BLOCKING));
        auto target_data = target + guard;
        auto count = words;
        std::array<void*, 3> parameters{&target_data, &count, &device_flags};
        CUDA(launch(verify, 1, 1, 1, 1, 1, 1, 0, stream, parameters.data(), nullptr));
        start = clock_type::now();
        while (!std::atomic_ref{flags->started}.load(std::memory_order_acquire))
            tick(start);
        // The verifier is now running. No CUDA copy or synchronization API may
        // order received data before it verifies the bytes after the plugin flush.
        void *send_request{}, *recv_request{};
        void* buffer = reinterpret_cast<void*>(target_data);
        auto size = bytes;
        int tag{42};
        while (!send_request || !recv_request) {
            if (!recv_request)
                OK(net->irecv(
                    receive, 1, &buffer, &size, &tag, &target_mr, nullptr, &recv_request));
            if (!send_request)
                OK(net->isend(send,
                              reinterpret_cast<void*>(source + guard),
                              bytes,
                              tag,
                              source_mr,
                              nullptr,
                              &send_request));
            tick(start);
        }
        const auto test = [&](void*& request) {
            if (!request)
                return;
            int done{}, length{};
            OK(net->test(request, &done, &length));
            if (done) {
                CHECK(length == bytes);
                request = nullptr;
            }
        };
        while (send_request || recv_request) {
            test(send_request);
            test(recv_request);
            tick(start);
        }
        int received = bytes;
        void* flush{};
        OK(net->iflush(receive, 1, &buffer, &received, &target_mr, &flush));
        CHECK(!flush);
        std::atomic_ref{flags->ready}.store(1, std::memory_order_release);
        while (!std::atomic_ref{flags->result}.load(std::memory_order_acquire))
            tick(start);
        CHECK(std::atomic_ref{flags->result}.load(std::memory_order_acquire) == 1);
        CUDA(synchronize(stream));
        OK(net->deregMr(send, source_mr));
        OK(net->deregMr(receive, target_mr));
        OK(net->closeSend(send));
        OK(net->closeRecv(receive));
        OK(net->finalize(ctx));
        CUDA(stream_destroy(stream));
        CUDA(module_unload(module));
        CUDA(host_free(flags));
        CUDA(free(source));
        CUDA(free(target));
        CUcontext popped{};
        CUDA(pop(&popped));
        CHECK(popped == context);
        CUDA(release(device));
        dlclose(library);
        dlclose(driver);
        std::puts("NCCL CUDA: running-kernel receive visibility and device subrange transfer pass");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        // A failed transfer may leave a verifier spinning or native DMA live.
        // Terminate the hardware test instead of freeing buffers underneath it.
        std::_Exit(1);
    }
}
