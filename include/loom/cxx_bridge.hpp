// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file cxx_bridge.hpp
 * @brief CXX bridge layer for Rust bindings.
 *
 * This header provides C++ functions and types designed to be consumed
 * by the Rust cxx bridge. It wraps loom's C++ API in a form suitable
 * for FFI.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "loom/async/completion_queue.hpp"
#include "loom/core/address_vector.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/fabric.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/types.hpp"
#include "rust/cxx.h"

namespace loom::cxx_bridge {

// ============================================================================
// Error handling helpers
// ============================================================================

/// Convert a loom error to an exception for cxx Result<T>
inline void throw_if_error(const std::error_code& ec) {
    if (ec) {
        throw std::runtime_error(ec.message());
    }
}

template <typename T>
inline auto unwrap_or_throw(loom::result<T>&& res) -> T {
    if (!res) {
        throw std::runtime_error(res.error().message());
    }
    return std::move(*res);
}

inline void unwrap_void_or_throw(loom::void_result&& res) {
    if (!res) {
        throw std::runtime_error(res.error().message());
    }
}

// ============================================================================
// Shared structs for cxx bridge (must be trivially copyable)
// ============================================================================

/// Completion event data passed to Rust
struct CompletionEventData {
    std::uintptr_t context;
    std::size_t bytes_transferred;
    std::uint64_t flags;
    std::uint64_t tag;
    std::uint64_t data;
    bool has_error;
    std::int32_t error_code;
};

/// Completion queue attributes for creation
struct CqAttributes {
    std::size_t size;
    bool wait_obj;
};

/// Fabric query hints
struct FabricHints {
    rust::String provider_name;
    std::uint64_t capabilities;
    std::uint32_t ep_type;  // 0=unspec, 1=msg, 2=rdm, 3=dgram
};

/// Memory region info returned after registration
struct MrInfo {
    std::uint64_t key;
    std::uintptr_t descriptor;
};

/// Remote memory descriptor for RMA operations
struct RemoteMemory {
    std::uint64_t addr;
    std::uint64_t key;
    std::size_t length;
};

// ============================================================================
// Opaque wrapper types (own their resources, movable)
// ============================================================================

/// Wrapper around fabric_info that owns the data
class FabricInfoHandle {
public:
    explicit FabricInfoHandle(loom::fabric_info info) : info_(std::move(info)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).info_;
    }

private:
    loom::fabric_info info_;
};

/// Wrapper around fabric that owns the resource
class FabricHandle {
public:
    explicit FabricHandle(loom::fabric fab) : fabric_(std::move(fab)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).fabric_;
    }

private:
    loom::fabric fabric_;
};

/// Wrapper around domain that owns the resource
class DomainHandle {
public:
    explicit DomainHandle(loom::domain dom) : domain_(std::move(dom)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).domain_;
    }

private:
    loom::domain domain_;
};

/// Wrapper around endpoint that owns the resource
class EndpointHandle {
public:
    explicit EndpointHandle(loom::endpoint ep) : endpoint_(std::move(ep)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).endpoint_;
    }

private:
    loom::endpoint endpoint_;
};

/// Wrapper around completion_queue that owns the resource
class CompletionQueueHandle {
public:
    explicit CompletionQueueHandle(loom::completion_queue cq) : cq_(std::move(cq)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).cq_;
    }

private:
    loom::completion_queue cq_;
};

/// Wrapper around address_vector that owns the resource
class AddressVectorHandle {
public:
    explicit AddressVectorHandle(loom::address_vector av) : av_(std::move(av)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).av_;
    }

private:
    loom::address_vector av_;
};

/// Wrapper around memory_region that owns the resource
class MemoryRegionHandle {
public:
    explicit MemoryRegionHandle(loom::memory_region mr) : mr_(std::move(mr)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).mr_;
    }

private:
    loom::memory_region mr_;
};

// ============================================================================
// Factory functions
// ============================================================================

/// Query for fabric providers matching the hints
inline auto query_fabric(const FabricHints& hints) -> std::unique_ptr<FabricInfoHandle> {
    loom::fabric_hints loom_hints{};

    if (!hints.provider_name.empty()) {
        loom_hints.provider_name = std::string(hints.provider_name);
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

/// Create a fabric instance from fabric_info
inline auto create_fabric(const FabricInfoHandle& info) -> std::unique_ptr<FabricHandle> {
    auto result = loom::fabric::create(info.get());
    return std::make_unique<FabricHandle>(unwrap_or_throw(std::move(result)));
}

/// Create a domain from a fabric and fabric_info
inline auto create_domain(const FabricHandle& fab, const FabricInfoHandle& info)
    -> std::unique_ptr<DomainHandle> {
    auto result = loom::domain::create(fab.get(), info.get());
    return std::make_unique<DomainHandle>(unwrap_or_throw(std::move(result)));
}

/// Create an endpoint from a domain and fabric_info
inline auto create_endpoint(const DomainHandle& dom, const FabricInfoHandle& info)
    -> std::unique_ptr<EndpointHandle> {
    auto result = loom::endpoint::create(dom.get(), info.get());
    return std::make_unique<EndpointHandle>(unwrap_or_throw(std::move(result)));
}

/// Create a completion queue
inline auto create_completion_queue(const DomainHandle& dom, const CqAttributes& attr)
    -> std::unique_ptr<CompletionQueueHandle> {
    loom::completion_queue_attr cq_attr{};
    cq_attr.size = loom::queue_size{attr.size};
    cq_attr.wait_obj = attr.wait_obj;

    auto result = loom::completion_queue::create(dom.get(), cq_attr);
    return std::make_unique<CompletionQueueHandle>(unwrap_or_throw(std::move(result)));
}

/// Create an address vector
inline auto create_address_vector(const DomainHandle& dom) -> std::unique_ptr<AddressVectorHandle> {
    auto result = loom::address_vector::create(dom.get());
    return std::make_unique<AddressVectorHandle>(unwrap_or_throw(std::move(result)));
}

/// Register a memory region
inline auto
register_memory(const DomainHandle& dom, std::uintptr_t ptr, std::size_t len, std::uint64_t access)
    -> std::unique_ptr<MemoryRegionHandle> {
    auto span = std::span{reinterpret_cast<std::byte*>(ptr), len};
    auto result = loom::memory_region::register_memory(dom.get(), span, loom::mr_access{access});
    return std::make_unique<MemoryRegionHandle>(unwrap_or_throw(std::move(result)));
}

// ============================================================================
// Endpoint operations
// ============================================================================

/// Enable an endpoint
inline void endpoint_enable(EndpointHandle& ep) {
    unwrap_void_or_throw(ep.get().enable());
}

/// Bind a completion queue to an endpoint for transmit and/or receive
inline void
endpoint_bind_cq(EndpointHandle& ep, const CompletionQueueHandle& cq, std::uint64_t flags) {
    unwrap_void_or_throw(ep.get().bind_cq(cq.get(), loom::cq_bind_flags{flags}));
}

/// Bind an address vector to an endpoint
inline void endpoint_bind_av(EndpointHandle& ep, const AddressVectorHandle& av) {
    unwrap_void_or_throw(ep.get().bind_av(av.get()));
}

/// Post a send operation
inline void
endpoint_send(EndpointHandle& ep, rust::Slice<const std::uint8_t> data, std::uintptr_t ctx) {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    unwrap_void_or_throw(ep.get().send(span, context));
}

/// Post a receive operation
inline void endpoint_recv(EndpointHandle& ep, rust::Slice<std::uint8_t> buf, std::uintptr_t ctx) {
    auto span = std::span{reinterpret_cast<std::byte*>(buf.data()), buf.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    unwrap_void_or_throw(ep.get().recv(span, context));
}

/// Post a tagged send operation
inline void endpoint_tagged_send(EndpointHandle& ep,
                                 rust::Slice<const std::uint8_t> data,
                                 std::uint64_t tag,
                                 std::uintptr_t ctx) {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    unwrap_void_or_throw(ep.get().tagged_send(span, tag, context));
}

/// Post a tagged receive operation
inline void endpoint_tagged_recv(EndpointHandle& ep,
                                 rust::Slice<std::uint8_t> buf,
                                 std::uint64_t tag,
                                 std::uint64_t ignore,
                                 std::uintptr_t ctx) {
    auto span = std::span{reinterpret_cast<std::byte*>(buf.data()), buf.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    unwrap_void_or_throw(ep.get().tagged_recv(span, tag, ignore, context));
}

/// Post an inject (small message, no completion)
inline void endpoint_inject(EndpointHandle& ep, rust::Slice<const std::uint8_t> data) {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    unwrap_void_or_throw(ep.get().inject(span));
}

/// Get the local address of an endpoint
inline auto endpoint_get_name(const EndpointHandle& ep) -> rust::Vec<std::uint8_t> {
    auto result = ep.get().get_name();
    if (!result) {
        throw std::runtime_error(result.error().message());
    }

    rust::Vec<std::uint8_t> vec;
    vec.reserve(result->size());
    for (auto b : *result) {
        vec.push_back(static_cast<std::uint8_t>(b));
    }
    return vec;
}

// ============================================================================
// Completion queue operations
// ============================================================================

/// Poll for a completion (non-blocking)
inline auto cq_poll(const CompletionQueueHandle& cq) -> CompletionEventData {
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

/// Wait for a completion with optional timeout (milliseconds, -1 = infinite)
inline auto cq_wait(const CompletionQueueHandle& cq, std::int32_t timeout_ms)
    -> CompletionEventData {
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

/// Get the wait file descriptor for async integration
inline auto cq_get_wait_fd(const CompletionQueueHandle& cq) -> std::int32_t {
    auto result = cq.get().get_wait_fd();
    if (!result) {
        throw std::runtime_error(result.error().message());
    }
    return *result;
}

/// Check if the CQ supports blocking wait
inline auto cq_supports_wait(const CompletionQueueHandle& cq) -> bool {
    return cq.get().supports_blocking_wait();
}

// ============================================================================
// Address vector operations
// ============================================================================

/// Insert an address into the address vector
inline auto av_insert(AddressVectorHandle& av, rust::Slice<const std::uint8_t> addr)
    -> std::uint64_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(addr.data()), addr.size()};
    auto result = av.get().insert(span);
    if (!result) {
        throw std::runtime_error(result.error().message());
    }
    return result->get();
}

// ============================================================================
// Memory region operations
// ============================================================================

/// Get the key for a memory region
inline auto mr_key(const MemoryRegionHandle& mr) -> std::uint64_t {
    return mr.get().key().get();
}

/// Get the local descriptor for a memory region
inline auto mr_descriptor(const MemoryRegionHandle& mr) -> std::uintptr_t {
    return std::bit_cast<std::uintptr_t>(mr.get().descriptor().raw());
}

// ============================================================================
// Capability constants (exposed to Rust)
// ============================================================================

inline auto cap_msg() -> std::uint64_t {
    return loom::capability::msg.get();
}
inline auto cap_rma() -> std::uint64_t {
    return loom::capability::rma.get();
}
inline auto cap_tagged() -> std::uint64_t {
    return loom::capability::tagged.get();
}
inline auto cap_atomic() -> std::uint64_t {
    return loom::capability::atomic.get();
}
inline auto cap_read() -> std::uint64_t {
    return loom::capability::read.get();
}
inline auto cap_write() -> std::uint64_t {
    return loom::capability::write.get();
}
inline auto cap_recv() -> std::uint64_t {
    return loom::capability::recv.get();
}
inline auto cap_send() -> std::uint64_t {
    return loom::capability::send.get();
}
inline auto cap_remote_read() -> std::uint64_t {
    return loom::capability::remote_read.get();
}
inline auto cap_remote_write() -> std::uint64_t {
    return loom::capability::remote_write.get();
}

/// CQ bind flags
inline auto cq_bind_transmit() -> std::uint64_t {
    return loom::cq_bind::transmit.get();
}
inline auto cq_bind_recv() -> std::uint64_t {
    return loom::cq_bind::recv.get();
}

/// MR access flags
inline auto mr_access_read() -> std::uint64_t {
    return loom::mr_access_flags::read.get();
}
inline auto mr_access_write() -> std::uint64_t {
    return loom::mr_access_flags::write.get();
}
inline auto mr_access_remote_read() -> std::uint64_t {
    return loom::mr_access_flags::remote_read.get();
}
inline auto mr_access_remote_write() -> std::uint64_t {
    return loom::mr_access_flags::remote_write.get();
}
inline auto mr_access_send() -> std::uint64_t {
    return loom::mr_access_flags::send.get();
}
inline auto mr_access_recv() -> std::uint64_t {
    return loom::mr_access_flags::recv.get();
}

}  // namespace loom::cxx_bridge
