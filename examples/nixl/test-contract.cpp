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
    std::unique_ptr<nixlBackendEngine, decltype(plugin->destroy_engine)> source{
        plugin->create_engine(&params), plugin->destroy_engine};
    params.localAgent = "identity-target";
    std::unique_ptr<nixlBackendEngine, decltype(plugin->destroy_engine)> target{
        plugin->create_engine(&params), plugin->destroy_engine};
    require(source && target, "create backend engines");
    std::array<std::byte, 256> input{}, output{};
    std::ranges::fill(input, std::byte{0x5a});
    nixlBlobDesc local{reinterpret_cast<std::uintptr_t>(input.data()), input.size(), 7, ""};
    nixlBlobDesc remote{reinterpret_cast<std::uintptr_t>(output.data()), output.size(), 19, ""};
    nixlBackendMD* raw{};
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
    std::puts("NIXL: registration and transfer device identities preserved");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
