// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <backend/backend_plugin.h>
#include <dlfcn.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void success(nixl_status_t status) {
    if (status != NIXL_SUCCESS)
        throw std::runtime_error("NIXL status " + std::to_string(status));
}
struct library_deleter {
    void operator()(void* library) const { dlclose(library); }
};
struct metadata_deleter {
    nixlBackendEngine* engine;
    void operator()(nixlBackendMD* metadata) const { (void)engine->unloadMD(metadata); }
};
using metadata_owner = std::unique_ptr<nixlBackendMD, metadata_deleter>;
struct request_owner {
    nixlBackendEngine& engine;
    nixlBackendReqH* handle{};
    ~request_owner() {
        if (handle)
            (void)engine.releaseReqH(handle);
    }
};
}  // namespace

int main(int argc, char** argv) try {
    require(argc == 3, "expected plugin path and provider");
    std::unique_ptr<void, library_deleter> library{dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)};
    require(bool(library), "load Loom plugin");
    const auto initialize =
        reinterpret_cast<decltype(&nixl_plugin_init)>(dlsym(library.get(), "nixl_plugin_init"));
    require(initialize, "find plugin entry point");
    auto* plugin = initialize();
    require(plugin && plugin->api_version == NIXL_PLUGIN_API_VERSION, "plugin ABI mismatch");
    nixl_b_params_t settings{{"provider", argv[2]}, {"domain", "lo"}};
    nixlBackendInitParams params{};
    params.localAgent = "identity-source";
    params.type = "LOOM";
    params.customParams = &settings;
    for (const auto* value : {"2", "-1", "1extra", ""}) {
        settings["cuda"] = value;
        std::unique_ptr<nixlBackendEngine, decltype(plugin->destroy_engine)> rejected_engine{
            plugin->create_engine(&params), plugin->destroy_engine};
        require(!rejected_engine, "accepted invalid CUDA mode");
    }
    settings["cuda"] = "0";
    std::unique_ptr<nixlBackendEngine, decltype(plugin->destroy_engine)> source{
        plugin->create_engine(&params), plugin->destroy_engine};
    params.localAgent = "identity-target";
    std::unique_ptr<nixlBackendEngine, decltype(plugin->destroy_engine)> target{
        plugin->create_engine(&params), plugin->destroy_engine};
    require(source && target, "create backend engines");
    require(source->getSupportedMems() == nixl_mem_list_t{DRAM_SEG},
            "host mode advertised device memory");
    std::array<std::byte, 1024> input{}, output{};
    std::ranges::fill(input, std::byte{0x5a});
    nixlBlobDesc local{reinterpret_cast<std::uintptr_t>(input.data()), input.size(), 7, ""};
    nixlBlobDesc remote{reinterpret_cast<std::uintptr_t>(output.data()), output.size(), 19, ""};
    nixlBackendMD* raw{};
    require(source->registerMem(local, VRAM_SEG, raw) == NIXL_ERR_NOT_SUPPORTED && !raw,
            "host mode accepted a device registration");
    success(source->registerMem(local, DRAM_SEG, raw));
    metadata_owner local_memory{raw, {source.get()}};
    success(target->registerMem(remote, DRAM_SEG, raw));
    metadata_owner target_memory{raw, {target.get()}};
    success(target->getPublicData(target_memory.get(), remote.metaInfo));
    std::string connection;
    success(target->getConnInfo(connection));
    success(source->loadRemoteConnInfo(params.localAgent, connection));

    auto mismatched = remote;
    ++mismatched.devId;
    auto status = source->loadRemoteMD(mismatched, DRAM_SEG, params.localAgent, raw);
    metadata_owner rejected{raw, {source.get()}};
    require(status == NIXL_ERR_MISMATCH && !rejected,
            "remote metadata accepted a different device identity");
    success(source->loadRemoteMD(remote, DRAM_SEG, params.localAgent, raw));
    metadata_owner remote_memory{raw, {source.get()}};

    for (const auto operation : {NIXL_READ, NIXL_WRITE}) {
        nixl_meta_dlist_t local_parts{DRAM_SEG}, remote_parts{DRAM_SEG};
        local_parts.addDesc({local.addr + 11, 64, local.devId, local_memory.get()});
        remote_parts.addDesc({remote.addr + 23, 64, remote.devId, remote_memory.get()});
        for (const bool wrong_local : {true, false}) {
            auto& device = wrong_local ? local_parts[0].devId : remote_parts[0].devId;
            ++device;
            request_owner request{*source};
            status = source->prepXfer(
                operation, local_parts, remote_parts, params.localAgent, request.handle);
            require(status == NIXL_ERR_INVALID_PARAM && !request.handle,
                    "transfer accepted a different device identity");
            --device;
        }
        request_owner request{*source};
        success(source->prepXfer(
            operation, local_parts, remote_parts, params.localAgent, request.handle));
        status = source->postXfer(
            operation, local_parts, remote_parts, params.localAgent, request.handle);
        require(status == NIXL_IN_PROG || status == NIXL_SUCCESS, "post transfer");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while ((status = source->checkXfer(request.handle)) == NIXL_IN_PROG) {
            require(std::chrono::steady_clock::now() < deadline, "transfer deadline");
            std::this_thread::yield();
        }
        success(status);
        // READ copies zeroes into one source subrange; WRITE then uses a new pattern.
        auto& destination = operation == NIXL_READ ? input : output;
        const std::size_t offset = operation == NIXL_READ ? 11 : 23;
        for (std::size_t i = 0; i < destination.size(); ++i) {
            const bool inside = i >= offset && i < offset + 64;
            const auto expected = operation == NIXL_READ ? (inside ? std::byte{} : std::byte{0x5a})
                                                         : (inside ? std::byte{0x3c} : std::byte{});
            require(destination[i] == expected, "data or guard mismatch");
        }
        std::ranges::fill(input, std::byte{0x3c});
    }
    // Leave more notices queued than the receiver's application queue can
    // hold. The visibility control channels must continue independently.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    for (unsigned i = 0; i < 96;) {
        status = source->genNotif(params.localAgent, "queued");
        if (status == NIXL_SUCCESS)
            ++i;
        else
            require(status == NIXL_ERR_NOT_ALLOWED, "queue notification burst");
        require(std::chrono::steady_clock::now() < deadline, "notification submission deadline");
        std::this_thread::yield();
    }
    const auto wait = [&](nixlBackendReqH* request) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        nixl_status_t state;
        while ((state = source->checkXfer(request)) == NIXL_IN_PROG) {
            require(std::chrono::steady_clock::now() < end, "visibility acknowledgement deadline");
            std::this_thread::yield();
        }
        return state;
    };
    nixl_meta_dlist_t many_local{DRAM_SEG}, many_remote{DRAM_SEG};
    for (unsigned i = 0; i < 257; ++i) {
        many_local.addDesc({local.addr + i, 1, local.devId, local_memory.get()});
        many_remote.addDesc({remote.addr + i, 1, remote.devId, remote_memory.get()});
    }
    request_owner batched{*source};
    success(
        source->prepXfer(NIXL_WRITE, many_local, many_remote, params.localAgent, batched.handle));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        std::ranges::fill(output, std::byte{});
        require(source->postXfer(
                    NIXL_WRITE, many_local, many_remote, params.localAgent, batched.handle) ==
                    NIXL_IN_PROG,
                "post multi-packet visibility batch");
        success(wait(batched.handle));
        for (std::size_t i = 0; i < output.size(); ++i)
            require(output[i] == (i < 257 ? std::byte{0x3c} : std::byte{}),
                    "batch data or guard mismatch");
    }
    nixl_opt_b_args_t invalid;
    invalid.hasNotif = true;
    invalid.notifMsg = std::string(8192, 'x');
    require(source->postXfer(
                NIXL_WRITE, many_local, many_remote, params.localAgent, batched.handle, &invalid) ==
                NIXL_ERR_INVALID_PARAM,
            "reject oversized notification");
    success(source->checkXfer(batched.handle));  // A failed repost preserves completed state.

    unsigned received{};
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (received != 96) {
        notif_list_t messages;
        success(target->getNotifs(messages));
        for (const auto& [sender, message] : messages) {
            require(sender == "identity-source" && message == "queued", "notice payload mismatch");
            ++received;
        }
        require(received <= 96 && std::chrono::steady_clock::now() < deadline,
                "notice drain deadline");
        std::this_thread::yield();
    }

    // Keep the native RMA address/key valid but forge the opaque registration
    // generation in the version-3 envelope. The target must reject its flush.
    auto stale = remote;
    require(stale.metaInfo.size() == 64, "expected version-3 memory envelope");
    stale.metaInfo.back() ^= static_cast<char>(0x80);
    success(source->loadRemoteMD(stale, DRAM_SEG, params.localAgent, raw));
    metadata_owner stale_memory{raw, {source.get()}};
    nixl_meta_dlist_t one_local{DRAM_SEG}, stale_remote{DRAM_SEG};
    one_local.addDesc({local.addr, 1, local.devId, local_memory.get()});
    stale_remote.addDesc({remote.addr, 1, remote.devId, stale_memory.get()});
    input[0] = std::byte{0x7d};
    request_owner failed{*source};
    success(
        source->prepXfer(NIXL_WRITE, one_local, stale_remote, params.localAgent, failed.handle));
    nixl_opt_b_args_t notified;
    notified.hasNotif = true;
    notified.notifMsg = "must not be delivered";
    require(source->postXfer(
                NIXL_WRITE, one_local, stale_remote, params.localAgent, failed.handle, &notified) ==
                NIXL_IN_PROG,
            "post stale-generation write");
    require(wait(failed.handle) == NIXL_ERR_BACKEND,
            "stale registration acknowledgement reported success");
    require(output[0] == input[0], "stale-generation test did not complete native RMA");
    notif_list_t unexpected;
    success(target->getNotifs(unexpected));
    require(unexpected.empty(), "failed write sent a success notification");
    std::puts(
        "NIXL: device identities, visibility acknowledgements and notification backpressure pass");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
