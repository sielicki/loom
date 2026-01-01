# loom

[<img src="https://upload.wikimedia.org/wikipedia/commons/thumb/7/79/Warp-weighted_loom_twill.jpg/250px-Warp-weighted_loom_twill.jpg">](https://en.wikipedia.org/wiki/Loom)

C++ bindings for libfabric, done right.

loom provides an opinionated, idiomatic, and strongly-typed C++23 interface over [libfabric](https://github.com/ofiwg/libfabric), which is a low-level vendor-agnostic fabric abstraction layer for HPC/RDMA networking.

## license

`BSD-2-Clause OR GPL-2.0-only` (identical to ofiwg/libfabric, see [COPYING](./COPYING))

## Requirements

- C++23 compiler
- libfabric

## Examples

```cpp
#include <loom/loom.hpp>
#include <loom/async/completion_queue.hpp>
#include <print>
#include <array>

auto main() -> int {
    using namespace loom;

    // Query for providers supporting tagged messaging
    auto info_result = query_fabric({.capabilities = capability::msg | capability::tagged,
                                     .ep_type = endpoint_types::rdm});
    if (!info_result) {
        std::println(stderr, "No suitable fabric provider found");
        return 1;
    }
    auto& info = *info_result;

    // Create fabric and domain from the first matching provider
    auto fabric = fabric::create(info).value();
    auto domain = domain::create(fabric, info).value();

    // Create completion queue for operation notifications
    auto cq = completion_queue::create(domain, {.size = 128}).value();

    // Create address vector for peer addressing (required for RDM endpoints)
    auto av = address_vector::create(domain).value();

    // Create and configure endpoint
    auto ep = endpoint::create(domain, info).value();
    ep.bind_cq(cq, cq_bind::transmit | cq_bind::recv).value();
    ep.bind_av(av).value();
    ep.enable().value();

    // Allocate and register a buffer for RDMA operations
    std::array<std::byte, 4096> buffer{};
    auto mr = memory_region::register_memory(
        domain,
        std::span{buffer},
        mr_access_flags::send | mr_access_flags::recv
    ).value();

    // Post a receive for incoming messages
    auto recv_result = ep.recv(std::span{buffer});
    if (!recv_result) {
        std::println(stderr, "Failed to post receive");
        return 1;
    }

    // Poll for completions
    while (auto event = cq.poll()) {
        if (event->has_error()) {
            std::println(stderr, "Operation failed");
        } else {
            std::println("Received {} bytes", event->bytes_transferred);
        }
        break;
    }

    return 0;
}
```

### Atomic Operations

loom provides type-safe remote atomic operations with automatic datatype deduction:

```cpp
#include <loom/loom.hpp>
#include <loom/async/completion_queue.hpp>

// Assuming endpoint and memory regions are already set up...
// remote_mr_info contains {addr, key, length} received from peer

auto main() -> int {
    using namespace loom;

    // ... fabric/domain/endpoint setup as above ...

    // Register buffers for atomic operations
    std::uint64_t local_value = 42;
    std::uint64_t result_value = 0;

    auto local_mr = memory_region::register_memory(
        domain,
        static_cast<void*>(&local_value), sizeof(local_value),
        mr_access_flags::read | mr_access_flags::write
    ).value();

    auto result_mr = memory_region::register_memory(
        domain,
        static_cast<void*>(&result_value), sizeof(result_value),
        mr_access_flags::read | mr_access_flags::write
    ).value();

    // Describe the remote memory location (received out-of-band from peer)
    remote_memory remote{
        .addr = peer_remote_addr,
        .key = peer_remote_key,
        .length = sizeof(std::uint64_t)
    };

    // Atomic fetch-and-add: adds local_value to remote, returns old value
    atomic::fetch_add(ep, local_value, &result_value, result_mr, remote).value();

    // Typed compare-and-swap: if remote == 100, set to 200
    std::uint64_t compare = 100;
    std::uint64_t swap = 200;
    std::uint64_t old_val = 0;
    atomic::cas(ep, compare, swap, &old_val, result_mr, remote).value();

    // Check if atomic operations are supported before using them
    if (atomic::is_valid(ep, atomic::operation::sum, atomic::datatype::uint64)) {
        atomic::add(ep, local_value, remote).value();
    }

    return 0;
}
```

### Triggered Operations

Triggered operations allow deferring work until a counter reaches a threshold,
enabling efficient pipelining and dependency chains:

```cpp
#include <loom/loom.hpp>
#include <loom/async/completion_queue.hpp>

auto main() -> int {
    using namespace loom;

    // ... fabric/domain/endpoint setup ...

    // Create counters for tracking completions and triggering work
    auto recv_counter = counter::create(domain, {.threshold = 1}).value();
    auto send_counter = counter::create(domain, {.threshold = 1}).value();

    // Bind counters to endpoint for automatic incrementing
    ep.bind_counter(recv_counter).value();
    ep.bind_counter(send_counter).value();

    // Allocate buffers
    std::array<std::byte, 4096> recv_buf{};
    std::array<std::byte, 4096> send_buf{};

    auto recv_mr = memory_region::register_memory(
        domain, std::span{recv_buf},
        mr_access_flags::recv | mr_access_flags::write
    ).value();

    auto send_mr = memory_region::register_memory(
        domain, std::span{send_buf},
        mr_access_flags::send | mr_access_flags::read
    ).value();

    // Create deferred work: send will execute when recv_counter reaches 1
    // This creates a pipeline: receive completes -> send automatically fires
    auto deferred_send = trigger::deferred_work::create_send(
        domain,
        ep,
        trigger::threshold_condition{&recv_counter, 1},  // Trigger condition
        &send_counter,                                    // Increment on completion
        msg::send_message<void*>{
            .iov = {{send_buf.data(), send_buf.size()}},
            .desc = {send_mr.descriptor()},
        }
    ).value();

    // Post the initial receive - when it completes, recv_counter increments,
    // which triggers the deferred send automatically
    ep.recv(std::span{recv_buf}).value();

    // Wait for the send to complete (counter reaches threshold)
    send_counter.wait().value();

    std::println("Pipeline complete: received data and sent response");

    return 0;
}
```

### RMA (Remote Memory Access)

One-sided read/write operations that bypass the remote CPU:

```cpp
#include <loom/loom.hpp>

auto main() -> int {
    using namespace loom;

    // ... fabric/domain/endpoint setup with RMA capabilities ...
    // Query with: capability::rma | capability::remote_read | capability::remote_write

    std::array<std::byte, 4096> local_buf{};
    auto local_mr = memory_region::register_memory(
        domain, std::span{local_buf},
        mr_access_flags::read | mr_access_flags::write |
        mr_access_flags::remote_read | mr_access_flags::remote_write
    ).value();

    // Remote memory info (exchanged out-of-band with peer)
    remote_memory remote{
        .addr = peer_addr,
        .key = peer_key,
        .length = 4096
    };

    // One-sided write: copy local data to remote memory
    std::memcpy(local_buf.data(), "Hello, RDMA!", 12);
    rma::write(ep, std::span{local_buf}.first(12), local_mr, remote).value();

    // One-sided read: copy remote data to local memory
    rma::read(ep, std::span{local_buf}, local_mr, remote).value();

    // Write with immediate data (notifies remote of write completion)
    rma::write_data(ep, std::span{local_buf}, local_mr, remote,
                    0xDEADBEEF /* immediate data */).value();

    return 0;
}
```

## Potential Future Work

- sender/receiver integration/implementation with [stdexec](https://github.com/NVIDIA/stdexec)/[beman::execution](https://github.com/bemanproject/execution).
- integration/implementation with [asio](https://think-async.com/Asio/).
- competing implementation of [NCCL/RCCL ofi plugin](https://github.com/aws/aws-ofi-nccl).
- competing implementation of [nvshmem ofi plugin](https://github.com/NVIDIA/nvshmem/tree/devel/src/modules/transport/libfabric).
- competing implementation of [nixl ofi plugin](https://github.com/ai-dynamo/nixl/tree/main/src/plugins/libfabric).
- stability/performance improvements, bug fixes (surely there are a few in this early draft.)
- builds/ci, mostly targeting manylinux targets.
- convert doxygen to html or markdown.
- rust bindings via [cxx](https://docs.rs/cxx/latest/cxx/) which compete with libfabric-sys in upstream libfabric but work better with tokio.
- zig bindings and integration with their new async stuff.
