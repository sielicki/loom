// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <nccl_net.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error(std::string{#condition} + " at line " +                       \
                                     std::to_string(__LINE__));                                    \
    } while (false)
#define OK(expression) CHECK((expression) == ncclSuccess)
using handle = std::array<std::byte, NCCL_NET_HANDLE_MAXSIZE>;
using deadline = std::chrono::steady_clock;

void tick(deadline::time_point start) {
    CHECK(deadline::now() - start < std::chrono::seconds{20});
    std::this_thread::yield();
}
struct library {
    void* object;
    ncclNet_v12_t& net;
    explicit library(const char* path) : object(dlopen(path, RTLD_NOW | RTLD_LOCAL)), net(load()) {}
    auto load() -> ncclNet_v12_t& {
        if (!object)
            throw std::runtime_error(dlerror());
        auto* symbol = dlsym(object, "ncclNetPlugin_v12");
        CHECK(symbol);
        return *static_cast<ncclNet_v12_t*>(symbol);
    }
    ~library() { dlclose(object); }
};
void inspect(ncclNet_v12_t& net) {
    int count{};
    OK(net.devices(&count));
    CHECK(count == 1);
    ncclNetProperties_v12_t props{};
    OK(net.getProperties(0, &props));
    CHECK(props.ptrSupport == NCCL_PTR_HOST);
    CHECK(props.maxRecvs == 1 && props.maxMultiRequestSize == 1);
    CHECK(props.maxP2pBytes >= 1024 * 1024 && props.regIsGlobal == 0);
    CHECK(net.regMrDmaBuf == nullptr && net.getDeviceMr == nullptr);
}
void finish(ncclNet_v12_t& net, void*& request, int expected, bool failure = false) {
    if (!request)
        return;
    int done{}, size{-1};
    const auto result = net.test(request, &done, &size);
    if (done) {
        CHECK(result == (failure ? ncclSystemError : ncclSuccess));
        if (!failure)
            CHECK(size == expected);
        request = nullptr;
    } else {
        CHECK(result == ncclSuccess);
    }
}
void local(ncclNet_v12_t& net) {
    inspect(net);
    void* ctx{};
    OK(net.init(&ctx, 1, nullptr, nullptr, nullptr));
    handle wire{};
    void* listener{};
    OK(net.listen(ctx, 0, wire.data(), &listener));
    void *receive{}, *send{};
    OK(net.accept(listener, &receive, nullptr));
    CHECK(!receive);
    auto start = deadline::now();
    while (!receive || !send) {
        if (!send)
            OK(net.connect(ctx, 0, wire.data(), &send, nullptr));
        if (!receive)
            OK(net.accept(listener, &receive, nullptr));
        tick(start);
    }
    OK(net.closeListen(listener));
    constexpr int count = NCCL_NET_MAX_REQUESTS;
    constexpr std::size_t segment = 8192;
    std::vector<std::byte> source(count * segment), target(source.size());
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<std::byte>((i * 37 + i / segment) & 0xff);
    void *source_mr{}, *target_mr{};
    OK(net.regMr(send, source.data(), source.size(), NCCL_PTR_HOST, &source_mr));
    OK(net.regMr(receive, target.data(), target.size(), NCCL_PTR_HOST, &target_mr));
    void* invalid{};
    CHECK(net.regMr(send, source.data(), source.size(), NCCL_PTR_CUDA, &invalid) ==
          ncclInvalidArgument);
    CHECK(net.isend(send, source.data(), 1, 0, target_mr, nullptr, &invalid) ==
          ncclInvalidArgument);
    CHECK(net.isend(send, source.data() + source.size(), 1, 0, source_mr, nullptr, &invalid) ==
          ncclInvalidArgument);

    // Fill every receive slot, with tags spanning the sign bit and the low bits.
    std::array<void*, count> receives{}, sends{};
    const auto tag = [](int index) {
        return std::bit_cast<int>(0x80000000U | static_cast<unsigned>(index));
    };
    start = deadline::now();
    for (int i = 0; i < count; ++i) {
        auto* data = static_cast<void*>(target.data() + i * segment);
        auto size = segment;
        auto value = tag(i);
        while (!receives[i]) {
            OK(net.irecv(receive, 1, &data, &size, &value, &target_mr, nullptr, &receives[i]));
            tick(start);
        }
    }
    auto* extra_data = static_cast<void*>(target.data());
    auto extra_size = segment;
    int extra_tag{17};
    void* extra{};
    OK(net.irecv(receive, 1, &extra_data, &extra_size, &extra_tag, &target_mr, nullptr, &extra));
    CHECK(!extra);
    // Reversed submission order proves matching rather than FIFO copying.
    int next = count - 1;
    std::array<bool, count> submitted{};
    while (next >= 0 || std::ranges::any_of(sends, [](void* p) { return p; }) ||
           std::ranges::any_of(receives, [](void* p) { return p; })) {
        if (next >= 0) {
            OK(net.isend(send,
                         source.data() + next * segment,
                         segment,
                         tag(next),
                         source_mr,
                         nullptr,
                         &sends[next]));
            if (sends[next]) {
                submitted[next] = true;
                --next;
            }
        }
        for (int i = 0; i < count; ++i) {
            if (submitted[i])
                finish(net, sends[i], segment);
            finish(net, receives[i], segment);
        }
        tick(start);
    }
    CHECK(source == target);

    // Reuse completed slots, report receive length rather than capacity, and
    // preserve registrations after the caller releases their public handles.
    for (const auto bytes : {0, 137, 64}) {
        const bool truncated = bytes == 64;
        auto* data = static_cast<void*>(target.data());
        std::size_t capacity = truncated ? 4 : segment;
        int value{42};
        void *recv_request{}, *send_request{};
        start = deadline::now();
        while (!recv_request) {
            OK(net.irecv(receive, 1, &data, &capacity, &value, &target_mr, nullptr, &recv_request));
            tick(start);
        }
        while (!send_request) {
            OK(net.isend(send, source.data(), bytes, value, source_mr, nullptr, &send_request));
            tick(start);
        }
        if (truncated) {
            OK(net.deregMr(send, source_mr));
            OK(net.deregMr(receive, target_mr));
        }
        while (recv_request || send_request) {
            finish(net, send_request, bytes);
            finish(net, recv_request, bytes, truncated);
            tick(start);
        }
        if (!truncated) {
            CHECK(std::equal(source.begin(), source.begin() + bytes, target.begin()));
            void* flush{};
            int size = bytes;
            OK(net.iflush(receive, 1, &data, &size, &target_mr, &flush));
            CHECK(!flush);
        }
    }
    OK(net.closeSend(send));
    OK(net.closeRecv(receive));
    OK(net.finalize(ctx));

    // Closing an incomplete accept must retire its posted bootstrap receive.
    OK(net.init(&ctx, 2, nullptr, nullptr, nullptr));
    OK(net.listen(ctx, 0, wire.data(), &listener));
    receive = nullptr;
    OK(net.accept(listener, &receive, nullptr));
    CHECK(!receive);
    OK(net.closeListen(listener));
    wire[0] ^= std::byte{1};
    CHECK(net.connect(ctx, 0, wire.data(), &send, nullptr) == ncclInvalidArgument);
    OK(net.finalize(ctx));
    std::puts(
        "NCCL Loom ABI: matching, request bounds, lengths, registration lifetime and cleanup pass");
}

void exchange(int fd, std::span<std::byte> data, bool writing) {
    while (!data.empty()) {
        const auto size =
            writing ? ::write(fd, data.data(), data.size()) : ::read(fd, data.data(), data.size());
        CHECK(size > 0);
        data = data.subspan(static_cast<std::size_t>(size));
    }
}
void process(const char* path, int fd, bool sending) {
    library plugin{path};
    auto& net = plugin.net;
    void* ctx{};
    OK(net.init(&ctx, 3, nullptr, nullptr, nullptr));
    handle wire{};
    void *listen{}, *comm{};
    if (!sending)
        OK(net.listen(ctx, 0, wire.data(), &listen));
    exchange(fd, wire, !sending);
    const auto start = deadline::now();
    while (!comm) {
        if (sending)
            OK(net.connect(ctx, 0, wire.data(), &comm, nullptr));
        else
            OK(net.accept(listen, &comm, nullptr));
        tick(start);
    }
    if (listen)
        OK(net.closeListen(listen));
    std::vector<std::byte> data(1024 * 1024);
    if (sending)
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<std::byte>((i * 13 + i / 31) & 0xff);
    void *mr{}, *request{};
    OK(net.regMr(comm, data.data(), data.size(), NCCL_PTR_HOST, &mr));
    while (!request) {
        if (sending)
            OK(net.isend(comm, data.data(), data.size(), 123, mr, nullptr, &request));
        else {
            auto* buffer = static_cast<void*>(data.data());
            auto size = data.size();
            int tag{123};
            OK(net.irecv(comm, 1, &buffer, &size, &tag, &mr, nullptr, &request));
        }
        tick(start);
    }
    while (request) {
        finish(net, request, data.size());
        tick(start);
    }
    if (!sending)
        for (std::size_t i = 0; i < data.size(); ++i)
            CHECK(data[i] == static_cast<std::byte>((i * 13 + i / 31) & 0xff));
    // Keep the sender's endpoint alive until the receiver has consumed its CQ.
    std::array<std::byte, 1> ack{};
    exchange(fd, ack, !sending);
    OK(net.deregMr(comm, mr));
    OK(sending ? net.closeSend(comm) : net.closeRecv(comm));
    OK(net.finalize(ctx));
}
}  // namespace

int main(int argc, char** argv) {
    try {
        CHECK(argc == 3 || argc == 4);
        CHECK(setenv("NCCL_LOOM_PROVIDER", argv[2], 1) == 0);
        CHECK(setenv("NCCL_LOOM_DOMAIN", "lo", 1) == 0);
        if (argc == 3) {
            library plugin{argv[1]};
            local(plugin.net);
        } else {
            int sockets[2];
            CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            const auto child = fork();
            CHECK(child >= 0);
            if (!child) {
                close(sockets[0]);
                process(argv[1], sockets[1], true);
                close(sockets[1]);
                return 0;
            }
            close(sockets[1]);
            process(argv[1], sockets[0], false);
            close(sockets[0]);
            int status{};
            CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0);
            std::puts("NCCL Loom ABI: separate-process registered transfer passes");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
