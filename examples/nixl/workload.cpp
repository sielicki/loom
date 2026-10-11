// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <nixl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void success(nixl_status_t status, const char* message) {
    if (status != NIXL_SUCCESS)
        throw std::runtime_error(std::string{message} + ": " + std::to_string(status));
}

struct request_owner {
    nixlAgent& agent;
    nixlXferReqH* handle{};
    ~request_owner() {
        if (handle)
            (void)agent.releaseXferReq(handle);
    }
    void release() {
        success(agent.releaseXferReq(handle), "release transfer");
        handle = nullptr;
    }
};

auto registration(std::span<std::byte> memory) -> nixl_reg_dlist_t {
    nixl_reg_dlist_t result{DRAM_SEG};
    nixlBlobDesc descriptor;
    descriptor.addr = reinterpret_cast<std::uintptr_t>(memory.data());
    descriptor.len = memory.size();
    descriptor.devId = 0;
    result.addDesc(descriptor);
    return result;
}

auto slice(std::span<std::byte> memory, std::size_t offset, std::size_t length) -> nixlBasicDesc {
    return {reinterpret_cast<std::uintptr_t>(memory.data() + offset), length, 0};
}

void wait(nixlAgent& agent, nixlXferReqH* handle) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        const auto status = agent.getXferStatus(handle);
        if (status != NIXL_IN_PROG) {
            success(status, "complete transfer");
            return;
        }
        require(std::chrono::steady_clock::now() < deadline, "transfer deadline exceeded");
        std::this_thread::sleep_for(1ms);
    }
}

void notification(nixlAgent& agent, const std::string& sender, const std::string& expected) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    nixl_notifs_t messages;
    while (messages.empty()) {
        success(agent.getNotifs(messages), "receive notification");
        require(std::chrono::steady_clock::now() < deadline, "notification deadline exceeded");
        std::this_thread::sleep_for(1ms);
    }
    require(messages.size() == 1 && messages.contains(sender), "notification sender mismatch");
    const auto& payloads = messages.at(sender);
    require(payloads.size() == 1 && payloads.front() == expected, "notification payload mismatch");
}

void notification_burst(nixlAgent& sender, nixlAgent& receiver, const nixl_opt_args_t& options) {
    std::array<bool, 40> received{};
    for (std::size_t i = 0; i < received.size(); ++i)
        success(sender.genNotif("loom-first", std::string(1, static_cast<char>(i)), &options),
                "send notification burst");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    std::size_t count{};
    while (count != received.size()) {
        nixl_notifs_t messages;
        success(receiver.getNotifs(messages), "receive notification burst");
        for (const auto& [sender_name, payloads] : messages) {
            require(sender_name == "loom-source", "burst sender mismatch");
            for (const auto& payload : payloads) {
                require(payload.size() == 1, "burst payload size mismatch");
                const auto index = static_cast<unsigned char>(payload.front());
                require(index < received.size() && !received[index],
                        "duplicate or invalid notification");
                received[index] = true;
                ++count;
            }
        }
        require(std::chrono::steady_clock::now() < deadline,
                "notification burst deadline exceeded");
        std::this_thread::sleep_for(1ms);
    }
}

struct channel {
    int fd{-1};
    ~channel() { close(); }
    void close() {
        if (fd >= 0)
            ::close(std::exchange(fd, -1));
    }
    void put(std::span<const std::byte> bytes) const {
        while (!bytes.empty()) {
            const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "metadata channel send failed");
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
    }
    void get(std::span<std::byte> bytes) const {
        while (!bytes.empty()) {
            const auto count = ::recv(fd, bytes.data(), bytes.size(), 0);
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "metadata channel receive failed");
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
    }
};

struct child_process {
    pid_t pid;
    ~child_process() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status{};
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    void join() {
        int status{};
        pid_t result;
        do {
            result = ::waitpid(pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        require(result == pid, "wait for target process failed");
        pid = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "target process failed");
    }
};

void cross_process(const std::string& provider) {
    int sockets[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create metadata channel failed");
    channel parent{sockets[0]}, child{sockets[1]};
    // No agent, provider, or progress thread exists before fork.
    const auto pid = ::fork();
    require(pid >= 0, "fork target process failed");
    if (pid == 0) {
        parent.close();
        std::array<std::byte, 8192> target{};
        nixlAgent agent{"process-target", nixlAgentConfig{}};
        nixlBackendH* backend{};
        success(agent.createBackend("LOOM", {{"provider", provider}, {"domain", "lo"}}, backend),
                "create target process backend");
        nixl_opt_args_t options;
        options.backends = {backend};
        const auto regions = registration(target);
        success(agent.registerMem(regions, &options), "register target process memory");
        std::string metadata;
        success(agent.getLocalMD(metadata), "export target process metadata");
        const std::array<std::uint64_t, 2> header{reinterpret_cast<std::uintptr_t>(target.data()),
                                                  metadata.size()};
        child.put(std::as_bytes(std::span{header}));
        child.put(std::as_bytes(std::span{metadata}));
        std::byte done{};
        // The target makes no NIXL calls while the other process reads and writes.
        child.get({&done, 1});
        notification(agent, "process-source", "process-write");
        for (std::size_t i = 0; i < target.size(); ++i)
            require(target[i] == (i >= 11 && i < 1011 ? std::byte{0x7d} : std::byte{}),
                    "target process data or guard mismatch");
        success(agent.deregisterMem(regions, &options), "deregister target process memory");
        return;
    }
    child.close();
    child_process target_process{pid};
    std::array<std::uint64_t, 2> header{};
    parent.get(std::as_writable_bytes(std::span{header}));
    require(header[1] <= 1024 * 1024, "invalid metadata size");
    std::string metadata(header[1], '\0');
    parent.get(std::as_writable_bytes(std::span{metadata}));
    std::array<std::byte, 8192> source{};
    source.fill(std::byte{0x7d});
    nixlAgent agent{"process-source", nixlAgentConfig{}};
    nixlBackendH* backend{};
    success(agent.createBackend("LOOM", {{"provider", provider}, {"domain", "lo"}}, backend),
            "create source process backend");
    nixl_opt_args_t options;
    options.backends = {backend};
    const auto regions = registration(source);
    success(agent.registerMem(regions, &options), "register source process memory");
    std::string name;
    success(agent.loadRemoteMD(metadata, name), "import target process metadata");
    nixl_xfer_dlist_t local{DRAM_SEG}, remote{DRAM_SEG};
    local.addDesc(slice(source, 27, 1000));
    remote.addDesc(nixlBasicDesc{header[0] + 11, 1000, 0});
    request_owner write{agent}, read{agent};
    success(agent.createXferReq(NIXL_WRITE, local, remote, name, write.handle, &options),
            "prepare process write");
    success(agent.createXferReq(NIXL_READ, local, remote, name, read.handle, &options),
            "prepare process read");
    auto notified = options;
    notified.notif = "process-write";
    require(agent.postXferReq(write.handle, &notified) >= NIXL_SUCCESS, "post process write");
    wait(agent, write.handle);
    source.fill(std::byte{});
    require(agent.postXferReq(read.handle) >= NIXL_SUCCESS, "post process read");
    wait(agent, read.handle);
    for (std::size_t i = 0; i < source.size(); ++i)
        require(source[i] == (i >= 27 && i < 1027 ? std::byte{0x7d} : std::byte{}),
                "source process data or guard mismatch");
    write.release();
    read.release();
    const std::byte done{};
    parent.put({&done, 1});
    target_process.join();
    success(agent.invalidateRemoteMD(name), "invalidate target process metadata");
    success(agent.deregisterMem(regions, &options), "deregister source process memory");
}

void run(const std::string& provider) {
    // Buffers outlive both agents, including cleanup after a failed assertion.
    std::array<std::byte, 8192> source{}, first{}, second{};
    nixlAgent sender{"loom-source", nixlAgentConfig{}};
    nixlAgent receiver{"loom-first", nixlAgentConfig{}};
    nixlAgent other{"loom-second", nixlAgentConfig{}};
    const nixl_b_params_t parameters{{"provider", provider}, {"domain", "lo"}};
    nixlBackendH *send_backend{}, *receive_backend{}, *other_backend{};
    success(sender.createBackend("LOOM", parameters, send_backend), "create source backend");
    success(receiver.createBackend("LOOM", parameters, receive_backend), "create first backend");
    success(other.createBackend("LOOM", parameters, other_backend), "create second backend");
    nixl_opt_args_t send_options, receive_options, other_options;
    send_options.backends = {send_backend};
    receive_options.backends = {receive_backend};
    other_options.backends = {other_backend};
    auto source_regions = registration(source);
    auto first_regions = registration(first);
    auto second_regions = registration(second);
    success(sender.registerMem(source_regions, &send_options), "register source");
    success(receiver.registerMem(first_regions, &receive_options), "register first target");
    success(other.registerMem(second_regions, &other_options), "register second target");
    std::string first_metadata, second_metadata, name;
    success(receiver.getLocalMD(first_metadata), "export first target");
    success(other.getLocalMD(second_metadata), "export second target");
    success(sender.loadRemoteMD(first_metadata, name), "import first target");
    success(sender.loadRemoteMD(second_metadata, name), "import second target");

    const std::string binary_message{"ready\0for\0data", 14};
    success(sender.genNotif("loom-first", binary_message, &send_options), "send notification");
    notification(receiver, "loom-source", binary_message);
    success(sender.genNotif("loom-second", {}, &send_options), "send empty notification");
    notification(other, "loom-source", {});
    require(sender.genNotif("loom-first", std::string(8192, 'x'), &send_options) ==
                NIXL_ERR_INVALID_PARAM,
            "oversized notification was accepted");
    notification_burst(sender, receiver, send_options);

    nixl_xfer_dlist_t local{DRAM_SEG}, remote_first{DRAM_SEG}, remote_second{DRAM_SEG};
    local.addDesc(slice(source, 13, 37));
    local.addDesc(slice(source, 2048, 513));
    remote_first.addDesc(slice(first, 29, 37));
    remote_first.addDesc(slice(first, 4096, 513));
    remote_second.addDesc(slice(second, 41, 37));
    remote_second.addDesc(slice(second, 1024, 513));
    request_owner write_first{sender}, write_second{sender}, read_first{sender};
    success(sender.createXferReq(
                NIXL_WRITE, local, remote_first, "loom-first", write_first.handle, &send_options),
            "prepare first write");
    success(
        sender.createXferReq(
            NIXL_WRITE, local, remote_second, "loom-second", write_second.handle, &send_options),
        "prepare second write");
    success(sender.createXferReq(
                NIXL_READ, local, remote_first, "loom-first", read_first.handle, &send_options),
            "prepare read");
    nixlBackendH* selected{};
    success(sender.queryXferBackend(write_first.handle, selected), "query selected backend");
    require(selected == send_backend, "transfer selected a different backend");

    for (unsigned iteration = 1; iteration <= 8; ++iteration) {
        const auto pattern = static_cast<std::byte>(iteration * 19);
        source.fill(pattern);
        first.fill(std::byte{0});
        second.fill(std::byte{0});
        auto notified = send_options;
        notified.notif = std::string{"written\0", 8} + std::to_string(iteration);
        require(sender.postXferReq(write_first.handle, &notified) >= NIXL_SUCCESS,
                "post first write");
        require(sender.postXferReq(write_second.handle) >= NIXL_SUCCESS, "post second write");
        // Neither target calls a progress or request API during the transfers.
        wait(sender, write_second.handle);
        // The notification is sent only after every write reaches the target.
        notification(receiver, "loom-source", *notified.notif);
        for (std::size_t i = 0; i < first.size(); ++i) {
            const bool in_first = (i >= 29 && i < 66) || (i >= 4096 && i < 4609);
            const bool in_second = (i >= 41 && i < 78) || (i >= 1024 && i < 1537);
            require(first[i] == (in_first ? pattern : std::byte{0}), "first target mismatch");
            require(second[i] == (in_second ? pattern : std::byte{0}), "second target mismatch");
        }
        wait(sender, write_first.handle);
        source.fill(std::byte{0});
        require(sender.postXferReq(read_first.handle) >= NIXL_SUCCESS, "post read");
        wait(sender, read_first.handle);
        for (std::size_t i = 0; i < source.size(); ++i) {
            const bool transferred = (i >= 13 && i < 50) || (i >= 2048 && i < 2561);
            require(source[i] == (transferred ? pattern : std::byte{0}), "read or guard mismatch");
        }
    }

    // Release an active batch. On return, accepted DMA must have retired.
    source.fill(std::byte{0x6d});
    require(sender.postXferReq(write_first.handle, &send_options) >= NIXL_SUCCESS,
            "post canceled write");
    write_first.release();
    const auto after_release = first;
    source.fill(std::byte{0});
    std::this_thread::sleep_for(20ms);
    require(first == after_release, "write continued after request release");
    read_first.release();
    success(sender.invalidateRemoteMD("loom-first"), "invalidate first target");
    success(sender.invalidateRemoteMD("loom-second"), "invalidate second target");
    require(sender.postXferReq(write_second.handle) == NIXL_ERR_NOT_FOUND,
            "invalidated metadata remained usable");
    write_second.release();
    success(sender.deregisterMem(source_regions, &send_options), "deregister source");
    success(receiver.deregisterMem(first_regions, &receive_options), "deregister first target");
    success(other.deregisterMem(second_regions, &other_options), "deregister second target");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string provider = argc > 1 ? argv[1] : "sockets";
        if (argc > 2)
            cross_process(provider);
        else
            run(provider);
        std::cout << "LOOM: registered reads, writes, notifications, peer selection, reuse and "
                     "teardown passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
