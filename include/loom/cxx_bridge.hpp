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

/// Submission rejection must retain its numeric libfabric status across FFI.
[[nodiscard]] inline auto submission_status(loom::void_result result) noexcept -> std::int32_t {
    return result ? 0 : result.error().value();
}

/// Use the target libfabric constants rather than hard-coded errno values in Rust.
[[nodiscard]] inline auto error_again() noexcept -> std::int32_t {
    return std::to_underlying(loom::errc::again);
}

[[nodiscard]] inline auto error_timeout() noexcept -> std::int32_t {
    return std::to_underlying(loom::errc::timeout);
}

// ============================================================================
// Shared structs for cxx bridge (must be trivially copyable)
// ============================================================================

/// Completion event data passed to Rust
struct CompletionEventData;
struct CqReadResult;

/// Completion queue attributes for creation
struct CqAttributes;

/// Fabric query hints
struct FabricHints;
struct FabricAttributesData;

/// Memory region info returned after registration
struct MrInfo;
struct MemoryRegistrationAttributes;

/// Remote memory descriptor for RMA operations
struct RemoteMemory;
struct RmaTarget;
struct RmaOptions;

// ============================================================================
// Opaque wrapper types (own their resources, movable)
// ============================================================================

/// Native provider scratch space. Rust moves only the owning unique_ptr.
class OperationContextHandle {
public:
    OperationContextHandle() = default;
    OperationContextHandle(const OperationContextHandle&) = delete;
    auto operator=(const OperationContextHandle&) -> OperationContextHandle& = delete;

    [[nodiscard]] auto address() const noexcept -> std::uintptr_t {
        return reinterpret_cast<std::uintptr_t>(&provider_);
    }

private:
    ::fi_context2 provider_{};
};

[[nodiscard]] inline auto create_operation_context() -> std::unique_ptr<OperationContextHandle> {
    return std::make_unique<OperationContextHandle>();
}

[[nodiscard]] inline auto operation_context_address(const OperationContextHandle& context) noexcept
    -> std::uintptr_t {
    return context.address();
}

/// Wrapper around fabric_info that owns the data
class FabricInfoHandle {
public:
    explicit FabricInfoHandle(loom::fabric_info info)
        : owner_(std::make_shared<loom::fabric_info>(std::move(info))), entry_(owner_.get()) {}

    [[nodiscard]] auto get() const noexcept -> const loom::fabric_info& { return *entry_; }

    [[nodiscard]] auto next() const -> std::unique_ptr<FabricInfoHandle> {
        auto cursor = entry_->begin();
        if (cursor == entry_->end() || ++cursor == entry_->end())
            return {};
        auto next = std::make_unique<FabricInfoHandle>(*this);
        next->entry_ = std::addressof(*cursor);
        return next;
    }

private:
    std::shared_ptr<const loom::fabric_info> owner_;
    const loom::fabric_info* entry_;
};

/// Wrapper around fabric that owns the resource
class FabricHandle {
public:
    explicit FabricHandle(loom::fabric fab) : fabric_(std::move(fab)) {}

    template <typename Self>
    [[nodiscard]] auto get(this Self&& self) noexcept -> decltype(auto) {
        return (std::forward<Self>(self).fabric_);
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
        return (std::forward<Self>(self).domain_);
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
        return (std::forward<Self>(self).endpoint_);
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
        return (std::forward<Self>(self).cq_);
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
        return (std::forward<Self>(self).av_);
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
        return (std::forward<Self>(self).mr_);
    }

private:
    loom::memory_region mr_;
};

// ============================================================================
// Factory functions
// ============================================================================

/// Query for fabric providers matching the hints
auto query_fabric(const FabricHints& hints) -> std::unique_ptr<FabricInfoHandle>;
auto fabric_info_next(const FabricInfoHandle& info) -> std::unique_ptr<FabricInfoHandle>;
auto fabric_info_attributes(const FabricInfoHandle& info) -> FabricAttributesData;

/// Create a fabric instance from fabric_info
auto create_fabric(const FabricInfoHandle& info) -> std::unique_ptr<FabricHandle>;

/// Create a domain from a fabric and fabric_info
auto create_domain(const FabricHandle& fab, const FabricInfoHandle& info)
    -> std::unique_ptr<DomainHandle>;

/// Create an endpoint from a domain and fabric_info
auto create_endpoint(const DomainHandle& dom, const FabricInfoHandle& info)
    -> std::unique_ptr<EndpointHandle>;

/// Create a completion queue
auto create_completion_queue(const DomainHandle& dom, const CqAttributes& attr)
    -> std::unique_ptr<CompletionQueueHandle>;

/// Create an address vector
auto create_address_vector(const DomainHandle& dom) -> std::unique_ptr<AddressVectorHandle>;

/// Register a memory region
auto register_memory(const DomainHandle& dom,
                     std::uintptr_t ptr,
                     std::size_t len,
                     std::uint64_t access) -> std::unique_ptr<MemoryRegionHandle>;

auto register_memory_with_options(const DomainHandle& dom,
                                  std::uintptr_t ptr,
                                  std::size_t len,
                                  const MemoryRegistrationAttributes& attributes)
    -> std::unique_ptr<MemoryRegionHandle>;

// ============================================================================
// Endpoint operations
// ============================================================================

/// Enable an endpoint
void endpoint_enable(EndpointHandle& ep);

/// Return the native close status, retaining endpoint ownership on failure.
auto endpoint_close(EndpointHandle& ep) noexcept -> std::int32_t;

/// Bind a completion queue to an endpoint for transmit and/or receive
void endpoint_bind_cq(EndpointHandle& ep, const CompletionQueueHandle& cq, std::uint64_t flags);

/// Bind an address vector to an endpoint
void endpoint_bind_av(EndpointHandle& ep, const AddressVectorHandle& av);

/// Post a send operation
auto endpoint_send(EndpointHandle& ep, rust::Slice<const std::uint8_t> data, std::uintptr_t ctx)
    -> std::int32_t;

/// Post a receive operation
auto endpoint_recv(EndpointHandle& ep, rust::Slice<std::uint8_t> buf, std::uintptr_t ctx)
    -> std::int32_t;

/// Post a tagged send operation
auto endpoint_tagged_send(EndpointHandle& ep,
                          rust::Slice<const std::uint8_t> data,
                          std::uint64_t tag,
                          std::uintptr_t ctx) -> std::int32_t;

/// Post a tagged receive operation
auto endpoint_tagged_recv(EndpointHandle& ep,
                          rust::Slice<std::uint8_t> buf,
                          std::uint64_t tag,
                          std::uint64_t ignore,
                          std::uintptr_t ctx) -> std::int32_t;

/// Post an inject (small message, no completion)
auto endpoint_inject(EndpointHandle& ep, rust::Slice<const std::uint8_t> data) -> std::int32_t;

/// Registered RMA submissions preserve the destination and completion policy.
auto endpoint_read_raw(EndpointHandle& ep,
                       std::uint8_t* buffer,
                       std::size_t length,
                       const MemoryRegionHandle& mr,
                       const RmaTarget& target,
                       const RmaOptions& options,
                       std::uintptr_t context) -> std::int32_t;

auto endpoint_write_raw(EndpointHandle& ep,
                        const std::uint8_t* buffer,
                        std::size_t length,
                        const MemoryRegionHandle& mr,
                        const RmaTarget& target,
                        const RmaOptions& options,
                        std::uintptr_t context) -> std::int32_t;

/// Get the local address of an endpoint
auto endpoint_get_name(const EndpointHandle& ep) -> rust::Vec<std::uint8_t>;

// ============================================================================
// Completion queue operations
// ============================================================================

/// Poll for a completion (non-blocking)
auto cq_poll(const CompletionQueueHandle& cq) -> CqReadResult;

/// Prepare the provider for a native wait on the CQ descriptor.
auto cq_try_wait(const CompletionQueueHandle& cq, const FabricHandle& fabric) -> bool;

/// Wait for a completion with optional timeout (milliseconds, -1 = infinite)
auto cq_wait(const CompletionQueueHandle& cq, std::int32_t timeout_ms) -> CqReadResult;

/// Get the wait file descriptor for async integration
auto cq_get_wait_fd(const CompletionQueueHandle& cq) -> std::int32_t;

/// Check if the CQ supports blocking wait
auto cq_supports_wait(const CompletionQueueHandle& cq) -> bool;

// ============================================================================
// Address vector operations
// ============================================================================

/// Insert an address into the address vector
auto av_insert(const AddressVectorHandle& av, rust::Slice<const std::uint8_t> addr)
    -> std::uint64_t;

// ============================================================================
// Memory region operations
// ============================================================================

/// Get the key for a memory region
auto mr_key(const MemoryRegionHandle& mr) -> std::uint64_t;

/// Get the local descriptor for a memory region
auto mr_descriptor(const MemoryRegionHandle& mr) -> std::uintptr_t;

auto mr_remote_memory(const MemoryRegionHandle& mr) -> RemoteMemory;

// ============================================================================
// Capability constants (exposed to Rust)
// ============================================================================

auto cap_msg() -> std::uint64_t;
auto cap_rma() -> std::uint64_t;
auto cap_tagged() -> std::uint64_t;
auto cap_atomic() -> std::uint64_t;
auto cap_read() -> std::uint64_t;
auto cap_write() -> std::uint64_t;
auto cap_recv() -> std::uint64_t;
auto cap_send() -> std::uint64_t;
auto cap_remote_read() -> std::uint64_t;
auto cap_remote_write() -> std::uint64_t;

/// CQ bind flags
auto cq_bind_transmit() -> std::uint64_t;
auto cq_bind_recv() -> std::uint64_t;

/// MR access flags
auto mr_access_read() -> std::uint64_t;
auto mr_access_write() -> std::uint64_t;
auto mr_access_remote_read() -> std::uint64_t;
auto mr_access_remote_write() -> std::uint64_t;
auto mr_access_send() -> std::uint64_t;
auto mr_access_recv() -> std::uint64_t;

}  // namespace loom::cxx_bridge
