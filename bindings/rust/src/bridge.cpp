// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <rdma/fi_cm.h>

#include <vector>

#include "loom-fabric/src/ffi.rs.h"

namespace loom::cxx_bridge {

auto query_fabric(const FabricHints& hints) -> std::unique_ptr<FabricInfoHandle> {
    loom::fabric_hints loom_hints{};

    const auto provider_name = std::string(hints.provider_name);
    if (!provider_name.empty()) {
        loom_hints.provider_name = provider_name;
    }

    loom_hints.capabilities = loom::caps{hints.capabilities};

    switch (hints.ep_type) {
        case 1:
            loom_hints.ep_type = loom::endpoint_types::msg;
            break;
        case 2:
            loom_hints.ep_type = loom::endpoint_types::rdm;
            break;
        case 3:
            loom_hints.ep_type = loom::endpoint_types::dgram;
            break;
        default:
            break;
    }

    auto result = loom::query_fabric(loom_hints);
    if (!result) {
        throw std::runtime_error(result.error().message());
    }

    return std::make_unique<FabricInfoHandle>(std::move(*result));
}

auto create_fabric(const FabricInfoHandle& info) -> std::unique_ptr<FabricHandle> {
    auto result = loom::fabric::create(info.get());
    return std::make_unique<FabricHandle>(unwrap_or_throw(std::move(result)));
}

auto create_domain(const FabricHandle& fab, const FabricInfoHandle& info)
    -> std::unique_ptr<DomainHandle> {
    auto result = loom::domain::create(fab.get(), info.get());
    return std::make_unique<DomainHandle>(unwrap_or_throw(std::move(result)));
}

auto create_endpoint(const DomainHandle& dom, const FabricInfoHandle& info)
    -> std::unique_ptr<EndpointHandle> {
    auto result = loom::endpoint::create(dom.get(), info.get());
    return std::make_unique<EndpointHandle>(unwrap_or_throw(std::move(result)));
}

auto create_completion_queue(const DomainHandle& dom, const CqAttributes& attr)
    -> std::unique_ptr<CompletionQueueHandle> {
    loom::completion_queue_attr cq_attr{};
    cq_attr.size = loom::queue_size{attr.size};
    cq_attr.wait_obj = attr.wait_obj;

    auto result = loom::completion_queue::create(dom.get(), cq_attr);
    return std::make_unique<CompletionQueueHandle>(unwrap_or_throw(std::move(result)));
}

auto create_address_vector(const DomainHandle& dom) -> std::unique_ptr<AddressVectorHandle> {
    auto result = loom::address_vector::create(dom.get());
    return std::make_unique<AddressVectorHandle>(unwrap_or_throw(std::move(result)));
}

auto register_memory(const DomainHandle& dom,
                     std::uintptr_t ptr,
                     std::size_t len,
                     std::uint64_t access) -> std::unique_ptr<MemoryRegionHandle> {
    auto span = std::span{reinterpret_cast<std::byte*>(ptr), len};
    auto result = loom::memory_region::register_memory(dom.get(), span, loom::mr_access{access});
    return std::make_unique<MemoryRegionHandle>(unwrap_or_throw(std::move(result)));
}

void endpoint_enable(EndpointHandle& ep) {
    unwrap_void_or_throw(ep.get().enable());
}

void endpoint_bind_cq(EndpointHandle& ep, const CompletionQueueHandle& cq, std::uint64_t flags) {
    unwrap_void_or_throw(ep.get().bind_cq(cq.get(), loom::cq_bind_flags{flags}));
}

void endpoint_bind_av(EndpointHandle& ep, const AddressVectorHandle& av) {
    unwrap_void_or_throw(ep.get().bind_av(av.get()));
}

auto endpoint_send(EndpointHandle& ep, rust::Slice<const std::uint8_t> data, std::uintptr_t ctx)
    -> std::int32_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().send(span, context));
}

auto endpoint_recv(EndpointHandle& ep, rust::Slice<std::uint8_t> buf, std::uintptr_t ctx)
    -> std::int32_t {
    auto span = std::span{reinterpret_cast<std::byte*>(buf.data()), buf.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().recv(span, context));
}

auto endpoint_tagged_send(EndpointHandle& ep,
                          rust::Slice<const std::uint8_t> data,
                          std::uint64_t tag,
                          std::uintptr_t ctx) -> std::int32_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().tagged_send(span, tag, context));
}

auto endpoint_tagged_recv(EndpointHandle& ep,
                          rust::Slice<std::uint8_t> buf,
                          std::uint64_t tag,
                          std::uint64_t ignore,
                          std::uintptr_t ctx) -> std::int32_t {
    auto span = std::span{reinterpret_cast<std::byte*>(buf.data()), buf.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().tagged_recv(span, tag, ignore, context));
}

auto endpoint_inject(EndpointHandle& ep, rust::Slice<const std::uint8_t> data) -> std::int32_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    return submission_status(ep.get().inject(span));
}

auto endpoint_get_name(const EndpointHandle& ep) -> rust::Vec<std::uint8_t> {
    if (!ep.get()) {
        throw std::runtime_error("invalid endpoint");
    }
    auto* handle = static_cast<fid_ep*>(ep.get().internal_ptr());
    std::size_t length{};
    auto ret = fi_getname(&handle->fid, nullptr, &length);
    if (ret != -FI_ETOOSMALL && ret != 0) {
        throw std::runtime_error(loom::make_error_code_from_fi_errno(-ret).message());
    }
    std::vector<std::uint8_t> address(length);
    ret = fi_getname(&handle->fid, address.data(), &length);
    if (ret != 0) {
        throw std::runtime_error(loom::make_error_code_from_fi_errno(-ret).message());
    }
    rust::Vec<std::uint8_t> vec;
    vec.reserve(length);
    for (const auto byte : std::span{address}.first(length)) {
        vec.push_back(byte);
    }
    return vec;
}

auto cq_poll(const CompletionQueueHandle& cq) -> CompletionEventData {
    auto event_opt = cq.get().poll();

    CompletionEventData data{};
    if (!event_opt) {
        data.context = 0;
        data.has_error = false;
        data.error_code = -11;  // EAGAIN
        return data;
    }

    const auto& event = *event_opt;
    data.context = std::bit_cast<std::uintptr_t>(event.context.raw());
    data.bytes_transferred = event.bytes_transferred;
    data.flags = event.flags;
    data.tag = event.tag;
    data.data = event.data;
    data.has_error = event.has_error();
    data.error_code = event.error ? event.error.value() : 0;

    return data;
}

auto cq_wait(const CompletionQueueHandle& cq, std::int32_t timeout_ms) -> CompletionEventData {
    std::optional<std::chrono::milliseconds> timeout;
    if (timeout_ms >= 0) {
        timeout = std::chrono::milliseconds{timeout_ms};
    }

    auto result = cq.get().wait(timeout);

    CompletionEventData data{};
    if (!result) {
        data.context = 0;
        data.has_error = true;
        data.error_code = result.error().value();
        return data;
    }

    const auto& event = *result;
    data.context = std::bit_cast<std::uintptr_t>(event.context.raw());
    data.bytes_transferred = event.bytes_transferred;
    data.flags = event.flags;
    data.tag = event.tag;
    data.data = event.data;
    data.has_error = event.has_error();
    data.error_code = event.error ? event.error.value() : 0;

    return data;
}

auto cq_try_wait(const CompletionQueueHandle& cq, const FabricHandle& fabric) -> bool {
    auto ready = cq.get().try_wait(fabric.get());
    if (!ready) {
        throw std::system_error(ready.error());
    }
    return *ready;
}

auto cq_get_wait_fd(const CompletionQueueHandle& cq) -> std::int32_t {
    auto result = cq.get().get_wait_fd();
    if (!result) {
        throw std::runtime_error(result.error().message());
    }
    return *result;
}

auto cq_supports_wait(const CompletionQueueHandle& cq) -> bool {
    return cq.get().supports_blocking_wait();
}

auto av_insert(const AddressVectorHandle& av, rust::Slice<const std::uint8_t> addr)
    -> std::uint64_t {
    if (!av.get() || addr.empty()) {
        throw std::runtime_error("invalid address vector or empty address");
    }
    auto* handle = static_cast<fid_av*>(av.get().internal_ptr());
    fi_addr_t address = FI_ADDR_NOTAVAIL;
    const auto ret = fi_av_insert(handle, addr.data(), 1, &address, 0, nullptr);
    if (ret != 1) {
        throw std::runtime_error(
            loom::make_error_code_from_fi_errno(ret < 0 ? -ret : FI_EINVAL).message());
    }
    return address;
}

auto mr_key(const MemoryRegionHandle& mr) -> std::uint64_t {
    return mr.get().key();
}

auto mr_descriptor(const MemoryRegionHandle& mr) -> std::uintptr_t {
    return std::bit_cast<std::uintptr_t>(mr.get().descriptor().raw());
}

auto cap_msg() -> std::uint64_t {
    return loom::capability::msg.get();
}

auto cap_rma() -> std::uint64_t {
    return loom::capability::rma.get();
}

auto cap_tagged() -> std::uint64_t {
    return loom::capability::tagged.get();
}

auto cap_atomic() -> std::uint64_t {
    return loom::capability::atomic.get();
}

auto cap_read() -> std::uint64_t {
    return loom::capability::read.get();
}

auto cap_write() -> std::uint64_t {
    return loom::capability::write.get();
}

auto cap_recv() -> std::uint64_t {
    return loom::capability::recv.get();
}

auto cap_send() -> std::uint64_t {
    return loom::capability::send.get();
}

auto cap_remote_read() -> std::uint64_t {
    return loom::capability::remote_read.get();
}

auto cap_remote_write() -> std::uint64_t {
    return loom::capability::remote_write.get();
}

auto cq_bind_transmit() -> std::uint64_t {
    return loom::cq_bind::transmit.get();
}

auto cq_bind_recv() -> std::uint64_t {
    return loom::cq_bind::recv.get();
}

auto mr_access_read() -> std::uint64_t {
    return loom::mr_access_flags::read.get();
}

auto mr_access_write() -> std::uint64_t {
    return loom::mr_access_flags::write.get();
}

auto mr_access_remote_read() -> std::uint64_t {
    return loom::mr_access_flags::remote_read.get();
}

auto mr_access_remote_write() -> std::uint64_t {
    return loom::mr_access_flags::remote_write.get();
}

auto mr_access_send() -> std::uint64_t {
    return loom::mr_access_flags::send.get();
}

auto mr_access_recv() -> std::uint64_t {
    return loom::mr_access_flags::recv.get();
}

}  // namespace loom::cxx_bridge
