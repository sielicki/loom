// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <charconv>
#include <cstdio>
#include <string_view>

#include <nixl.h>

#include "cuda-workload.hpp"

namespace gpu_test = loom_examples::cuda_test;
namespace {
void success(nixl_status_t status, const char* operation) {
    if (status != NIXL_SUCCESS) {
        std::fprintf(
            stderr, "%s failed with NIXL status %d\n", operation, static_cast<int>(status));
        std::_Exit(1);
    }
}
auto ordinal(std::string_view text) -> int {
    int result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    gpu_test::require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                          result >= 0,
                      "invalid CUDA device ordinal");
    return result;
}
auto registration(void* buffer, int device) -> nixl_reg_dlist_t {
    nixl_reg_dlist_t result{VRAM_SEG};
    result.addDesc({reinterpret_cast<std::uintptr_t>(buffer),
                    gpu_test::buffers::allocation_size,
                    static_cast<std::uint64_t>(device),
                    ""});
    return result;
}
auto segments(void* buffer, int device) -> nixl_xfer_dlist_t {
    nixl_xfer_dlist_t result{VRAM_SEG};
    constexpr unsigned count = 257;
    constexpr auto segment_words = gpu_test::buffers::words / count;
    for (unsigned i = 0; i < count; ++i) {
        const auto offset = i * segment_words;
        const auto length = i == count - 1 ? gpu_test::buffers::words - offset : segment_words;
        result.addDesc({reinterpret_cast<std::uintptr_t>(buffer) + offset * sizeof(std::uint32_t),
                        length * sizeof(std::uint32_t),
                        static_cast<std::uint64_t>(device)});
    }
    return result;
}
void wait(nixlAgent& agent, nixlXferReqH* request) {
    const auto start = gpu_test::clock_type::now();
    for (;;) {
        const auto status = agent.getXferStatus(request);
        if (status != NIXL_IN_PROG) {
            success(status, "complete CUDA transfer");
            return;
        }
        gpu_test::tick(start);
    }
}
void wait_notification(nixlAgent& agent) {
    const auto start = gpu_test::clock_type::now();
    nixl_notifs_t messages;
    while (messages.empty()) {
        success(agent.getNotifs(messages), "receive CUDA transfer notification");
        gpu_test::tick(start);
    }
    gpu_test::require(messages.size() == 1 && messages.contains("gpu-producer"),
                      "CUDA notification sender mismatch");
    const auto& payloads = messages.at("gpu-producer");
    gpu_test::require(payloads.size() == 1 && payloads.front() == "gpu-visible",
                      "CUDA notification payload mismatch");
}
}  // namespace

int main(int argc, char** argv) try {
    // visibility.ptx, source GPU ordinal, destination GPU ordinal, provider, domain
    gpu_test::require(argc == 6,
                      "expected PTX path, source GPU, destination GPU, provider and domain");
    const auto source_device = ordinal(argv[2]);
    const auto target_device = ordinal(argv[3]);
    gpu_test::buffers source_memory{source_device, argv[1]};
    gpu_test::buffers target_memory{target_device, argv[1]};
    {
        nixlAgent producer{"gpu-producer", nixlAgentConfig{}};
        nixlAgent consumer{"gpu-consumer", nixlAgentConfig{}};
        // Catch errors while agents and GPU allocations remain alive. The test
        // must not unwind into teardown with live DMA or a waiting verifier.
        try {
            const nixl_b_params_t parameters{
                {"provider", argv[4]}, {"domain", argv[5]}, {"cuda", "1"}};
            nixlBackendH *source_backend{}, *target_backend{};
            success(producer.createBackend("LOOM", parameters, source_backend),
                    "create CUDA producer backend");
            success(consumer.createBackend("LOOM", parameters, target_backend),
                    "create CUDA consumer backend");
            nixl_opt_args_t source_options, target_options;
            source_options.backends = {source_backend};
            target_options.backends = {target_backend};
            const auto source_regions = registration(source_memory.source(), source_device);
            const auto target_regions = registration(target_memory.target(), target_device);
            success(producer.registerMem(source_regions, &source_options), "register CUDA source");
            success(consumer.registerMem(target_regions, &target_options),
                    "register CUDA destination");
            std::string source_metadata, target_metadata, peer;
            success(producer.getLocalMD(source_metadata), "export CUDA source");
            success(consumer.getLocalMD(target_metadata), "export CUDA destination");
            success(producer.loadRemoteMD(target_metadata, peer), "import CUDA destination");
            success(consumer.loadRemoteMD(source_metadata, peer), "import CUDA source");
            const auto source_parts = segments(source_memory.source_data(), source_device);
            const auto target_parts = segments(target_memory.target_data(), target_device);
            nixlXferReqH *write{}, *read{};
            success(
                producer.createXferReq(
                    NIXL_WRITE, source_parts, target_parts, "gpu-consumer", write, &source_options),
                "prepare CUDA write");
            success(
                consumer.createXferReq(
                    NIXL_READ, target_parts, source_parts, "gpu-producer", read, &target_options),
                "prepare CUDA read");
            for (const bool notification : {false, true}) {
                target_memory.reset_destination();
                target_memory.start();
                auto options = source_options;
                if (notification)
                    options.notif = "gpu-visible";
                const auto posted = producer.postXferReq(write, &options);
                gpu_test::require(posted == NIXL_IN_PROG || posted == NIXL_SUCCESS,
                                  "post CUDA write");
                if (notification)
                    wait_notification(consumer);
                else
                    wait(producer, write);
                target_memory.finish();
                wait(producer, write);
            }
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                target_memory.reset_destination();
                target_memory.start();
                const auto posted = consumer.postXferReq(read, &target_options);
                gpu_test::require(posted == NIXL_IN_PROG || posted == NIXL_SUCCESS,
                                  "post CUDA read");
                wait(consumer, read);
                target_memory.finish();
            }
            success(producer.releaseXferReq(write), "release CUDA write");
            success(consumer.releaseXferReq(read), "release CUDA read");
            success(producer.invalidateRemoteMD("gpu-consumer"),
                    "invalidate CUDA destination metadata");
            success(consumer.invalidateRemoteMD("gpu-producer"), "invalidate CUDA source metadata");
            success(producer.deregisterMem(source_regions, &source_options),
                    "deregister CUDA source");
            success(consumer.deregisterMem(target_regions, &target_options),
                    "deregister CUDA destination");
        } catch (const std::exception& error) {
            gpu_test::fail(error.what());
        }
    }
    target_memory.close();
    source_memory.close();
    std::puts("NIXL CUDA: read/write visibility, notification ordering, device identities, "
              "subranges and guards pass");
} catch (const std::exception& error) {
    gpu_test::fail(error.what());
}
