// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/endpoint_contract.hpp>
#include <loom/core/endpoint_options.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <nccl_net.h>

#include "cuda.hpp"

namespace {
namespace cuda = loom_examples::cuda;
template <typename T>
auto checked(loom::result<T> value) -> T {
    if (!value)
        throw std::system_error(value.error());
    if constexpr (!std::is_void_v<T>)
        return std::move(*value);
}

void require(bool condition) {
    if (!condition)
        throw std::invalid_argument("invalid Loom NCCL argument or transport contract");
}

template <typename F>
auto boundary(F&& function) noexcept -> ncclResult_t {
    try {
        function();
        return ncclSuccess;
    } catch (const cuda::error& error) {
        std::fprintf(stderr, "Loom NCCL: %s\n", error.what());
        return ncclUnhandledCudaError;
    } catch (const std::invalid_argument& error) {
        std::fprintf(stderr, "Loom NCCL: %s\n", error.what());
        return ncclInvalidArgument;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Loom NCCL: %s\n", error.what());
        return ncclSystemError;
    } catch (...) {
        return ncclInternalError;
    }
}

struct profile : loom::message_endpoint_profile<loom::rdm_endpoint_tag> {
    static constexpr loom::caps required_caps =
        loom::message_endpoint_profile<loom::rdm_endpoint_tag>::required_caps |
        loom::capability::tagged | loom::capability::directed_recv;
    static constexpr loom::mode supported_modes =
        loom::mode_bits::context | loom::mode_bits::context2;
    static constexpr loom::threading_mode threading = loom::threading_mode::safe;
};
struct cuda_profile : profile {
    static constexpr loom::caps required_caps = profile::required_caps | loom::capability::hmem;
};

auto cuda_enabled() -> bool {
    const auto* value = std::getenv("NCCL_LOOM_CUDA");
    if (!value)
        return false;
    const std::string_view text{value};
    int enabled{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), enabled);
    require(error == std::errc{} && end == text.data() + text.size() &&
            (enabled == 0 || enabled == 1));
    return enabled != 0;
}
constexpr auto supported_mr = loom::mr_mode_flags::local | loom::mr_mode_flags::virt_addr |
                              loom::mr_mode_flags::allocated | loom::mr_mode_flags::prov_key |
                              loom::mr_mode_flags::endpoint;
constexpr std::size_t max_comms = 256;
constexpr std::size_t wire_size = 40;
using wire_address = std::array<std::byte, wire_size>;
enum class wire_version : std::uint64_t { ipv4_v1 = 0x4c4f4f4d4e430001 };
enum class direction { send, receive };
enum class phase { free, pending, complete };

auto info(bool gpu) -> loom::fabric_info {
    const auto mr = gpu ? supported_mr | loom::mr_mode_flags::hmem : supported_mr;
    loom::fabric_hints hints;
    if (const auto* provider = std::getenv("NCCL_LOOM_PROVIDER"))
        hints.provider_name = provider;
    else
        hints.provider_name = "sockets";
    if (const auto* domain = std::getenv("NCCL_LOOM_DOMAIN"))
        hints.domain_name = domain;
    else
        hints.domain_name = "lo";
    hints.ep_type = profile::ep_type;
    hints.addr_format = loom::address_format::inet;
    hints.capabilities = gpu ? cuda_profile::required_caps : profile::required_caps;
    hints.mode_bits = profile::supported_modes;
    hints.threading = profile::threading;
    hints.memory_region_mode = mr;
    auto result = checked(loom::query_fabric(hints));
    const auto* native = static_cast<const fi_info*>(result.internal_ptr());
    require(native->ep_attr && native->domain_attr && native->tx_attr && native->rx_attr);
    require(result.get_caps().has(hints.capabilities));
    require(result.get_address_format() == loom::address_format::inet &&
            (native->domain_attr->mr_mode & ~loom::detail::to_fi_mr_mode(mr)) == 0 &&
            native->tx_attr->op_flags == 0 && native->rx_attr->op_flags == 0 &&
            std::popcount(native->ep_attr->mem_tag_format) >= 32 &&
            native->ep_attr->max_msg_size >= wire_size);
    return result;
}

void put(wire_address& wire, std::size_t word, std::uint64_t value) {
    for (std::size_t byte = 0; byte < 8; ++byte)
        wire[word * 8 + byte] = static_cast<std::byte>(value >> (56 - byte * 8));
}
auto get(const wire_address& wire, std::size_t word) -> std::uint64_t {
    std::uint64_t value{};
    for (std::size_t byte = 0; byte < 8; ++byte)
        value = (value << 8) | std::to_integer<unsigned char>(wire[word * 8 + byte]);
    return value;
}

struct comm;
struct context {
    std::unique_ptr<cuda::driver> gpu{cuda_enabled() ? std::make_unique<cuda::driver>() : nullptr};
    loom::fabric_info selected{info(bool(gpu))};
    std::mutex mutex;
    std::atomic<std::size_t> live{};
    std::unordered_map<void*, std::unique_ptr<comm>> connecting;
};
struct registration {
    loom::memory_region region;
    std::optional<cuda::allocation> gpu;
    std::uintptr_t base{};
    std::size_t size{};

    auto covers(const void* data, std::size_t length) const noexcept -> bool {
        const auto pointer = reinterpret_cast<std::uintptr_t>(data);
        return pointer >= base && pointer - base <= size && length <= size - (pointer - base);
    }
};
struct operation {
    comm* owner{};
    loom::request_context context;
    loom::completion_registration completion;
    std::shared_ptr<registration> memory;
    phase state{phase::free};
    direction kind{};
    std::size_t size{};
    std::error_code error;
};

struct comm {
    context& parent;
    direction kind;
    std::mutex mutex;
    loom::fabric_info selected;
    loom::fabric fabric;
    loom::domain domain;
    loom::completion_queue cq;
    loom::address_vector av;
    loom::endpoint endpoint;
    loom::fabric_addr peer{loom::fabric_addrs::unspecified};
    loom::mr_mode mr_mode;
    std::uint64_t tag_mask{};
    std::uint64_t protocol{};
    std::size_t limit{};
    bool ready{};
    bool closing{};
    wire_address outgoing{};
    wire_address incoming{};
    std::array<std::shared_ptr<registration>, 2> bootstrap_memory;
    std::array<operation, 2> bootstrap;
    std::array<operation, NCCL_NET_MAX_REQUESTS> requests;
    std::unordered_map<void*, std::shared_ptr<registration>> registrations;

    comm(context& ctx, direction dir) : parent(ctx), kind(dir) {
        require(parent.live.load() < max_comms);
        selected = checked(loom::fabric_info::copy_native(
            *static_cast<const fi_info*>(parent.selected.internal_ptr())));
        const auto* native = static_cast<const fi_info*>(selected.internal_ptr());
        tag_mask = native->ep_attr->mem_tag_format;
        protocol = native->ep_attr->protocol;
        limit = std::min<std::size_t>(native->ep_attr->max_msg_size, INT_MAX);
        mr_mode = selected.get_domain_attr()->memory_region_mode;
        fabric = checked(loom::fabric::create(selected));
        domain = checked(loom::domain::create(fabric, selected));
        cq = checked(loom::completion_queue::create(domain, {.size = loom::queue_size{64}}));
        av = checked(loom::address_vector::create(domain));
        endpoint =
            parent.gpu
                ? checked(loom::contract_endpoint<cuda_profile>::create(domain, selected)).release()
                : checked(loom::contract_endpoint<profile>::create(domain, selected)).release();
        if (parent.gpu) {
            // A provider staging copy may synchronize the device while a running
            // NCCL kernel waits for this receive. Require a path without CUDA calls.
            checked(loom::set_cuda_api_permitted(endpoint, false));
        }
        checked(endpoint.bind_cq(cq,
                                 loom::cq_bind::transmit | loom::cq_bind::recv |
                                     loom::cq_bind::selective_completion));
        checked(endpoint.bind_av(av));
        checked(endpoint.enable());
        const auto address = checked(endpoint.get_local_address());
        const auto* ipv4 = std::get_if<loom::ipv4_address>(&address);
        require(ipv4 && ipv4->port());
        put(outgoing, 0, std::to_underlying(wire_version::ipv4_v1));
        put(outgoing, 1, protocol);
        put(outgoing, 2, tag_mask);
        put(outgoing, 3, (std::uint64_t{ipv4->to_uint32()} << 16) | ipv4->port());
        put(outgoing, 4, limit);
        bootstrap_memory[0] = register_buffer(outgoing.data(), outgoing.size());
        bootstrap_memory[1] = register_buffer(incoming.data(), incoming.size());
        for (auto& work : requests)
            work.owner = this;
        for (auto& work : bootstrap)
            work.owner = this;
        ++parent.live;
    }
    ~comm() { --parent.live; }

    auto register_buffer(void* data, std::size_t size, int type = NCCL_PTR_HOST)
        -> std::shared_ptr<registration> {
        auto memory = std::make_shared<registration>();
        const auto access = loom::mr_access_flags::send | loom::mr_access_flags::recv;
        if (type == NCCL_PTR_CUDA) {
            require(bool(parent.gpu));
            memory->gpu = cuda::inspect(parent.gpu->functions, data);
            memory->region = checked(loom::memory_region::register_memory(
                domain,
                data,
                size,
                loom::memory_registration{
                    .access = access,
                    .device = loom::hmem_device::cuda(memory->gpu->device),
                    .allocation = loom::hmem_allocation_kind::device,
                }));
        } else {
            require(type == NCCL_PTR_HOST);
            memory->region =
                checked(loom::memory_region::register_memory(domain, data, size, access));
        }
        if (mr_mode.has(loom::mr_mode_flags::endpoint)) {
            checked(memory->region.bind(endpoint));
            checked(memory->region.enable());
        }
        memory->base = reinterpret_cast<std::uintptr_t>(data);
        memory->size = size;
        return memory;
    }
    void set_peer(const wire_address& wire) {
        require(get(wire, 0) == std::to_underlying(wire_version::ipv4_v1) &&
                get(wire, 1) == protocol && get(wire, 2) == tag_mask && (get(wire, 3) >> 48) == 0 &&
                (get(wire, 3) & 0xffff) != 0 && get(wire, 4) >= wire_size &&
                get(wire, 4) <= INT_MAX);
        const loom::ipv4_address address{static_cast<std::uint32_t>(get(wire, 3) >> 16),
                                         static_cast<std::uint16_t>(get(wire, 3))};
        peer = loom::fabric_addr{checked(av.insert(address)).value};
        limit = std::min<std::size_t>(limit, get(wire, 4));
    }
    auto tag(int value) const -> std::uint64_t {
        auto input = static_cast<std::uint32_t>(value);
        auto mask = tag_mask;
        std::uint64_t result{};
        while (input) {
            const auto bit = mask & (~mask + 1);
            if (input & 1)
                result |= bit;
            input >>= 1;
            mask &= mask - 1;
        }
        return result;
    }
    static void completed(void* object, const loom::completion_event& event) noexcept {
        auto& work = *static_cast<operation*>(object);
        work.error = event.error;
        if (work.kind == direction::receive)
            work.size = event.bytes_transferred;
        work.state = phase::complete;
    }
    void progress() {
        for (std::size_t i = 0; i < 64; ++i) {
            auto event = cq.read();
            if (!event && event.error() == loom::errc::again)
                return;
            require(cq.dispatch(checked(std::move(event))));
        }
    }
    void post(operation& work,
              direction dir,
              void* data,
              std::size_t size,
              std::shared_ptr<registration> memory,
              int value,
              bool control = false) {
        require(work.state == phase::free && !closing);
        work.context = loom::request_context{};
        checked(work.completion.bind(
            cq, loom::context_ptr<void>{work.context.context_ptr()}, &work, completed));
        struct rollback {
            operation& work;
            ~rollback() {
                if (work.state == phase::free)
                    work.completion.reset();
            }
        } pending{work};
        const iovec buffer{data, size};
        const auto descriptor = memory ? memory->region.descriptor() : loom::mr_descriptor{};
        const auto buffers = size ? std::span<const iovec>{&buffer, 1} : std::span<const iovec>{};
        const auto descriptors = memory ? std::span<const loom::mr_descriptor>{&descriptor, 1}
                                        : std::span<const loom::mr_descriptor>{};
        loom::result<void> result;
        if (control && dir == direction::send)
            result =
                loom::msg::sendmsg(endpoint,
                                   loom::msg::send_message<>{.iov = buffers,
                                                             .desc = descriptors,
                                                             .dest_addr = peer,
                                                             .context = work.context.context_ptr()},
                                   loom::msg::send_flag::completion);
        else if (control)
            result =
                loom::msg::recvmsg(endpoint,
                                   loom::msg::recv_message<>{.iov = buffers,
                                                             .desc = descriptors,
                                                             .src_addr = peer,
                                                             .context = work.context.context_ptr()},
                                   loom::msg::recv_flag::completion);
        else if (dir == direction::send)
            result = loom::msg::tagged_sendmsg(
                endpoint,
                loom::msg::tagged_send_message<>{.iov = buffers,
                                                 .desc = descriptors,
                                                 .dest_addr = peer,
                                                 .tag = tag(value),
                                                 .context = work.context.context_ptr()},
                loom::msg::send_flag::completion);
        else
            result = loom::msg::tagged_recvmsg(
                endpoint,
                loom::msg::tagged_recv_message<>{.iov = buffers,
                                                 .desc = descriptors,
                                                 .src_addr = peer,
                                                 .tag = tag(value),
                                                 .context = work.context.context_ptr()},
                loom::msg::recv_flag::completion);
        if (!result) {
            work.completion.reset();
            if (result.error() != loom::errc::again)
                checked(std::move(result));
            return;
        }
        work.kind = dir;
        work.size = size;
        work.error.clear();
        work.memory = std::move(memory);
        work.state = phase::pending;
    }
    void handshake() {
        progress();
        for (auto& work : bootstrap)
            if (work.state == phase::complete && work.error)
                throw std::system_error(work.error);
        if (bootstrap[1].state == phase::free)
            post(bootstrap[1],
                 direction::receive,
                 incoming.data(),
                 incoming.size(),
                 bootstrap_memory[1],
                 0,
                 true);
        if (kind == direction::receive && bootstrap[1].state == phase::complete &&
            peer == loom::fabric_addrs::unspecified) {
            require(bootstrap[1].size == wire_size);
            set_peer(incoming);
        }
        if (peer != loom::fabric_addrs::unspecified && bootstrap[0].state == phase::free)
            post(bootstrap[0],
                 direction::send,
                 outgoing.data(),
                 outgoing.size(),
                 bootstrap_memory[0],
                 0,
                 true);
        if (bootstrap[0].state == phase::complete && bootstrap[1].state == phase::complete) {
            require(bootstrap[1].size == wire_size);
            if (kind == direction::send) {
                // The acknowledgement must be from the endpoint in the exchanged handle.
                require(get(incoming, 0) == get(outgoing, 0) && get(incoming, 1) == protocol &&
                        get(incoming, 2) == tag_mask);
            }
            for (auto& work : bootstrap)
                work.memory.reset();
            bootstrap_memory = {};
            ready = true;
        }
    }
    void close() {
        closing = true;
        const auto pending = [](const operation& work) {
            return work.state == phase::pending;
        };
        if (std::ranges::any_of(requests, pending) || std::ranges::any_of(bootstrap, pending))
            checked(endpoint.close());
        // Successful native close retires every context, including discarded completions.
        for (auto& work : requests) {
            work.completion.reset();
            work.memory.reset();
        }
        for (auto& work : bootstrap) {
            work.completion.reset();
            work.memory.reset();
        }
        registrations.clear();
        bootstrap_memory = {};
        checked(endpoint.close());
    }
};

struct listener {
    std::unique_ptr<comm> receiving;
};

auto init(void** output,
          std::uint64_t,
          ncclNetCommConfig_v12_t*,
          ncclDebugLogger_t,
          ncclProfilerCallback_t) -> ncclResult_t {
    return boundary([&] {
        require(output);
        *output = nullptr;
        *output = new context;
    });
}
auto devices(int* count) -> ncclResult_t {
    return boundary([&] {
        require(count);
        *count = 0;
        context probe;
        *count = 1;
    });
}
auto properties(int dev, ncclNetProperties_v12_t* props) -> ncclResult_t {
    return boundary([&] {
        require(dev == 0 && props);
        context selection;
        if (selection.gpu) {
            comm probe{selection, direction::receive};
            probe.close();
        }
        const auto& selected = selection.selected;
        static char name[] = "Loom";
        *props = {};
        props->name = name;
        props->ptrSupport = NCCL_PTR_HOST | (selection.gpu ? NCCL_PTR_CUDA : 0);
        props->forceFlush = selection.gpu ? 1 : 0;
        props->speed = 10000;  // Nominal software-adapter speed; no hardware topology is claimed.
        props->port = 1;
        props->maxComms = max_comms;
        props->maxRecvs = 1;
        props->netDeviceType = NCCL_NET_DEVICE_HOST;
        props->netDeviceVersion = NCCL_NET_DEVICE_INVALID_VERSION;
        props->vProps = {.ndevs = 1, .devs = {0}};
        props->maxP2pBytes = props->maxCollBytes =
            std::min<std::size_t>(selected.get_endpoint_attr()->max_msg_size.get(), INT_MAX);
        props->maxMultiRequestSize = 1;
        props->railId = props->planeId = NCCL_NET_ID_UNDEF;
    });
}
auto listen(void* ctx, int dev, void* handle, void** output) -> ncclResult_t {
    return boundary([&] {
        require(ctx && dev == 0 && handle && output);
        *output = nullptr;
        auto& parent = *static_cast<context*>(ctx);
        std::lock_guard lock{parent.mutex};
        auto object = std::make_unique<listener>();
        object->receiving = std::make_unique<comm>(parent, direction::receive);
        std::memset(handle, 0, NCCL_NET_HANDLE_MAXSIZE);
        std::memcpy(handle, object->receiving->outgoing.data(), wire_size);
        *output = object.release();
    });
}
auto connect(void* ctx, int dev, void* handle, void** output, ncclNetDeviceHandle_v12_t** offload)
    -> ncclResult_t {
    return boundary([&] {
        require(ctx && dev == 0 && handle && output && (!offload || !*offload));
        *output = nullptr;
        auto& parent = *static_cast<context*>(ctx);
        std::lock_guard lock{parent.mutex};
        auto found = parent.connecting.find(handle);
        if (found == parent.connecting.end()) {
            auto object = std::make_unique<comm>(parent, direction::send);
            wire_address remote;
            std::memcpy(remote.data(), handle, wire_size);
            object->set_peer(remote);
            found = parent.connecting.emplace(handle, std::move(object)).first;
        }
        auto& object = *found->second;
        object.handshake();
        if (object.ready) {
            *output = found->second.release();
            parent.connecting.erase(found);
        }
    });
}
auto accept(void* handle, void** output, ncclNetDeviceHandle_v12_t** offload) -> ncclResult_t {
    return boundary([&] {
        require(handle && output && (!offload || !*offload));
        *output = nullptr;
        auto& object = *static_cast<listener*>(handle);
        require(bool(object.receiving));
        std::lock_guard lock{object.receiving->mutex};
        object.receiving->handshake();
        if (object.receiving->ready)
            *output = object.receiving.release();
    });
}
auto reg_mr(void* handle, void* data, std::size_t size, int type, void** output) -> ncclResult_t {
    return boundary([&] {
        require(handle && data && size && (type == NCCL_PTR_HOST || type == NCCL_PTR_CUDA) &&
                output);
        *output = nullptr;
        auto& object = *static_cast<comm*>(handle);
        std::lock_guard lock{object.mutex};
        require(object.ready && !object.closing);
        auto memory = object.register_buffer(data, size, type);
        const auto key = memory.get();
        object.registrations.emplace(key, std::move(memory));
        *output = key;
    });
}
auto dereg_mr(void* handle, void* memory) -> ncclResult_t {
    return boundary([&] {
        require(handle && memory);
        auto& object = *static_cast<comm*>(handle);
        std::lock_guard lock{object.mutex};
        require(object.registrations.erase(memory) == 1);
    });
}
auto submit(
    void* handle, direction dir, void* data, std::size_t size, int tag, void* memory, void** output)
    -> ncclResult_t {
    return boundary([&] {
        require(handle && output);
        *output = nullptr;
        auto& object = *static_cast<comm*>(handle);
        std::lock_guard lock{object.mutex};
        require(object.ready && !object.closing && object.kind == dir && size <= object.limit);
        std::shared_ptr<registration> registered;
        if (size) {
            auto found = object.registrations.find(memory);
            require(found != object.registrations.end());
            const auto& region = *found->second;
            require(region.covers(data, size));
            registered = found->second;
        }
        object.progress();
        auto slot = std::ranges::find(object.requests, phase::free, &operation::state);
        if (slot == object.requests.end())
            return;
        object.post(*slot, dir, data, size, std::move(registered), tag);
        if (slot->state == phase::pending)
            *output = &*slot;
    });
}
auto send(void* comm, void* data, std::size_t size, int tag, void* memory, void*, void** output)
    -> ncclResult_t {
    return submit(comm, direction::send, data, size, tag, memory, output);
}
auto receive(void* comm,
             int count,
             void** data,
             std::size_t* sizes,
             int* tags,
             void** memory,
             void**,
             void** output) -> ncclResult_t {
    if (count != 1 || !data || !sizes || !tags || (sizes[0] && !memory))
        return ncclInvalidArgument;
    return submit(
        comm, direction::receive, data[0], sizes[0], tags[0], memory ? memory[0] : nullptr, output);
}
auto flush(void* handle, int count, void** data, int* sizes, void** memory, void** output)
    -> ncclResult_t {
    return boundary([&] {
        require(handle && count == 1 && sizes && sizes[0] >= 0 && output);
        *output = nullptr;
        auto& object = *static_cast<comm*>(handle);
        std::lock_guard lock{object.mutex};
        require(object.kind == direction::receive && object.ready && !object.closing);
        if (!sizes[0])
            return;
        require(data && memory);
        const auto found = object.registrations.find(memory[0]);
        require(found != object.registrations.end());
        const auto& region = *found->second;
        require(region.covers(data[0], static_cast<std::size_t>(sizes[0])));
        if (region.gpu)
            cuda::flush(object.parent.gpu->functions, *region.gpu);
        // CUDA's host flush returns only after visibility is established. A null
        // request therefore denotes an already completed flush, as for host data.
    });
}
auto test(void* handle, int* done, int* sizes) -> ncclResult_t {
    return boundary([&] {
        require(handle && done);
        *done = 0;
        auto& work = *static_cast<operation*>(handle);
        std::lock_guard lock{work.owner->mutex};
        require(work.state != phase::free);
        work.owner->progress();
        if (work.state != phase::complete)
            return;
        *done = 1;
        if (sizes)
            *sizes = static_cast<int>(std::min<std::size_t>(work.size, INT_MAX));
        work.memory.reset();
        work.state = phase::free;
        if (work.error)
            throw std::system_error(work.error);
    });
}
auto close_comm(void* handle) -> ncclResult_t {
    return boundary([&] {
        require(handle);
        auto& object = *static_cast<comm*>(handle);
        {
            std::lock_guard lock{object.mutex};
            object.close();
        }
        delete &object;
    });
}
auto close_listen(void* handle) -> ncclResult_t {
    return boundary([&] {
        require(handle);
        auto& object = *static_cast<listener*>(handle);
        if (object.receiving) {
            std::lock_guard lock{object.receiving->mutex};
            object.receiving->close();
        }
        delete &object;
    });
}
auto finalize(void* handle) -> ncclResult_t {
    return boundary([&] {
        require(handle);
        auto& object = *static_cast<context*>(handle);
        {
            std::lock_guard lock{object.mutex};
            require(object.live == object.connecting.size());
            while (!object.connecting.empty()) {
                auto first = object.connecting.begin();
                first->second->close();
                object.connecting.erase(first);
            }
        }
        delete &object;
    });
}
}  // namespace

extern "C" {
ncclNet_v12_t ncclNetPlugin_v12{
    .name = "Loom",
    .init = init,
    .devices = devices,
    .getProperties = properties,
    .listen = listen,
    .connect = connect,
    .accept = accept,
    .regMr = reg_mr,
    .regMrDmaBuf = nullptr,
    .deregMr = dereg_mr,
    .isend = send,
    .irecv = receive,
    .iflush = flush,
    .test = test,
    .closeSend = close_comm,
    .closeRecv = close_comm,
    .closeListen = close_listen,
    .getDeviceMr = nullptr,
    .irecvConsumed = nullptr,
    .makeVDevice = nullptr,
    .finalize = finalize,
    .setNetAttr = nullptr,
};
}
