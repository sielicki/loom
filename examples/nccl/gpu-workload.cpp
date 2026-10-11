// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <nccl_net.h>

#include "cuda-workload.hpp"

namespace gpu_test = loom_examples::cuda_test;
#define CHECK(condition) gpu_test::require(bool(condition), #condition)
#define OK(expression) CHECK((expression) == ncclSuccess)

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
        gpu_test::buffers memory{ordinal, argv[2]};
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
        auto start = gpu_test::clock_type::now();
        while (!send || !receive) {
            if (!send)
                OK(net->connect(ctx, 0, wire.data(), &send, nullptr));
            if (!receive)
                OK(net->accept(listener, &receive, nullptr));
            gpu_test::tick(start);
        }
        OK(net->closeListen(listener));
        void *source_mr{}, *target_mr{};
        OK(net->regMr(send, memory.source(), memory.allocation_size, NCCL_PTR_CUDA, &source_mr));
        OK(net->regMr(receive, memory.target(), memory.allocation_size, NCCL_PTR_CUDA, &target_mr));
        memory.start();
        start = gpu_test::clock_type::now();
        void *send_request{}, *recv_request{};
        void* buffer = memory.target_data();
        auto size = memory.bytes;
        int tag{42};
        while (!send_request || !recv_request) {
            if (!recv_request)
                OK(net->irecv(
                    receive, 1, &buffer, &size, &tag, &target_mr, nullptr, &recv_request));
            if (!send_request)
                OK(net->isend(send,
                              memory.source_data(),
                              memory.bytes,
                              tag,
                              source_mr,
                              nullptr,
                              &send_request));
            gpu_test::tick(start);
        }
        const auto test = [&](void*& request) {
            if (!request)
                return;
            int done{}, length{};
            OK(net->test(request, &done, &length));
            if (done) {
                CHECK(length == memory.bytes);
                request = nullptr;
            }
        };
        while (send_request || recv_request) {
            test(send_request);
            test(recv_request);
            gpu_test::tick(start);
        }
        int received = memory.bytes;
        void* flush{};
        OK(net->iflush(receive, 1, &buffer, &received, &target_mr, &flush));
        CHECK(!flush);
        memory.finish();
        OK(net->deregMr(send, source_mr));
        OK(net->deregMr(receive, target_mr));
        OK(net->closeSend(send));
        OK(net->closeRecv(receive));
        OK(net->finalize(ctx));
        memory.close();
        dlclose(library);
        std::puts("NCCL CUDA: running-kernel receive visibility, device subrange and guards pass");
    } catch (const std::exception& error) {
        gpu_test::fail(error.what());
    }
}
