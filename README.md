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

### Allocator-Aware Design

All loom objects support custom allocators via `std::pmr::memory_resource`,
enabling integration with arena allocators, memory pools, or GPU memory:

```cpp
#include <loom/loom.hpp>
#include <loom/async/completion_queue.hpp>
#include <memory_resource>

auto main() -> int {
    using namespace loom;

    // Create a monotonic buffer for zero-allocation steady-state operation
    std::array<std::byte, 1024 * 1024> buffer{};  // 1MB arena
    std::pmr::monotonic_buffer_resource arena{buffer.data(), buffer.size()};

    // All loom objects accept an optional memory_resource* parameter
    auto info = query_fabric({}, &arena).value();
    auto fab = fabric::create(info, &arena).value();
    auto dom = domain::create(fab, info, &arena).value();

    // Completion queues, address vectors, counters - all allocator-aware
    auto cq = completion_queue::create(dom, {.size = 256}, &arena).value();
    auto av = address_vector::create(dom, {}, &arena).value();
    auto cntr = counter::create(dom, {}, &arena).value();

    // Endpoints and memory regions too
    auto ep = endpoint::create(dom, info, &arena).value();

    std::array<std::byte, 4096> data{};
    auto mr = memory_region::register_memory(
        dom, std::span{data},
        mr_access_flags::send | mr_access_flags::recv,
        &arena
    ).value();

    // Deferred/triggered work also supports custom allocators
    auto deferred = trigger::deferred_work::create_send(
        dom, ep,
        trigger::threshold_condition{&cntr, 1},
        nullptr,
        msg::send_message<void*>{},
        msg::send_flag::none,
        &arena  // Custom allocator for internal structures
    ).value();

    return 0;
}
```

### Compile-Time Provider Traits

loom provides compile-time provider characteristics for zero-overhead abstraction
and static dispatch based on fabric capabilities:

```cpp
#include <loom/loom.hpp>

using namespace loom;

// Provider traits give compile-time access to provider capabilities
static_assert(provider_traits<provider::verbs_tag>::supports_native_atomics);
static_assert(provider_traits<provider::efa_tag>::uses_staged_atomics);
static_assert(provider_traits<provider::slingshot_tag>::supports_auto_progress);

// Concepts enable compile-time capability checking
static_assert(native_atomic_provider<provider::verbs_tag>);
static_assert(staged_atomic_provider<provider::efa_tag>);
static_assert(inject_capable_provider<provider::shm_tag>);
static_assert(auto_progress_provider<provider::slingshot_tag>);
static_assert(manual_progress_provider<provider::tcp_tag>);

// Query provider-specific limits at compile time
constexpr auto verbs_inject_size = provider_traits<provider::verbs_tag>::max_inject_size;  // 64
constexpr auto shm_inject_size = provider_traits<provider::shm_tag>::max_inject_size;      // 4096

// Compile-time helper functions
static_assert(can_inject<provider::verbs_tag>(32));   // true: 32 <= 64
static_assert(!can_inject<provider::verbs_tag>(128)); // false: 128 > 64

// Get default MR mode for a provider
constexpr auto verbs_mr = get_mr_mode<provider::verbs_tag>();
// verbs_mr == mr_mode_flags::basic | mr_mode_flags::local | mr_mode_flags::prov_key

// Check progress requirements
static_assert(requires_manual_progress<provider::verbs_tag>());
static_assert(!requires_manual_progress<provider::slingshot_tag>());
```

### Protocol Abstraction Layer

Define custom protocol types with compile-time capability validation:

```cpp
#include <loom/loom.hpp>
#include <loom/protocol/protocol.hpp>

using namespace loom;

// Define a protocol with static capability flags
template <typename Provider>
class my_rdma_protocol {
public:
    using provider_type = Provider;

    // Static capability declarations - checked at compile time
    static constexpr bool supports_rma = true;
    static constexpr bool supports_tagged = true;
    static constexpr bool supports_atomic = native_atomic_provider<Provider>;
    static constexpr bool supports_inject = provider_traits<Provider>::supports_inject;

    explicit my_rdma_protocol(endpoint& ep, memory_region& mr)
        : ep_(ep), mr_(mr) {}

    [[nodiscard]] auto name() const noexcept -> const char* {
        return "my_rdma_protocol";
    }

    // Required by basic_protocol concept
    [[nodiscard]] auto send(std::span<const std::byte> data, context_ptr<void> ctx = {})
        -> void_result {
        return ep_.send(data, ctx);
    }

    [[nodiscard]] auto recv(std::span<std::byte> buf, context_ptr<void> ctx = {})
        -> void_result {
        return ep_.recv(buf, ctx);
    }

    // Required by tagged_protocol concept
    [[nodiscard]] auto tagged_send(std::span<const std::byte> data, std::uint64_t tag,
                                   context_ptr<void> ctx = {}) -> void_result {
        return ep_.tagged_send(data, tag, ctx);
    }

    [[nodiscard]] auto tagged_recv(std::span<std::byte> buf, std::uint64_t tag,
                                   std::uint64_t ignore = 0, context_ptr<void> ctx = {})
        -> void_result {
        return ep_.tagged_recv(buf, tag, ignore, ctx);
    }

    // Required by rma_protocol concept
    [[nodiscard]] auto read(std::span<std::byte> local, rma_addr remote,
                            mr_key key, context_ptr<void> ctx = {}) -> void_result {
        return rma::read(ep_, local, mr_, remote_memory{remote.get(), key.get(), local.size()}, ctx);
    }

    [[nodiscard]] auto write(std::span<const std::byte> local, rma_addr remote,
                             mr_key key, context_ptr<void> ctx = {}) -> void_result {
        return rma::write(ep_, local, mr_, remote_memory{remote.get(), key.get(), local.size()}, ctx);
    }

    // Required by inject_capable concept (when supported)
    [[nodiscard]] auto inject(std::span<const std::byte> data,
                              fabric_addr dest = fabric_addrs::unspecified) -> void_result
        requires inject_capable_provider<Provider>
    {
        return ep_.inject(data, dest);
    }

private:
    endpoint& ep_;
    memory_region& mr_;
};

// Compile-time concept validation
static_assert(basic_protocol<my_rdma_protocol<provider::verbs_tag>>);
static_assert(tagged_protocol<my_rdma_protocol<provider::verbs_tag>>);
static_assert(rma_protocol<my_rdma_protocol<provider::verbs_tag>>);
static_assert(has_static_capabilities<my_rdma_protocol<provider::verbs_tag>>);
static_assert(provider_aware_protocol<my_rdma_protocol<provider::verbs_tag>>);

// Use protocol-generic functions
void use_any_protocol(auto& proto) requires basic_protocol<decltype(proto)> {
    std::array<std::byte, 64> buf{};
    protocol::send(proto, std::span{buf});  // Generic send via protocol layer

    // Conditionally use RMA if available
    if constexpr (rma_protocol<decltype(proto)>) {
        protocol::read(proto, std::span{buf}, rma_addr{0x1000}, mr_key{42});
    }

    // Query capabilities at compile time
    using caps = protocol_capabilities<std::remove_cvref_t<decltype(proto)>>;
    if constexpr (caps::static_rma) {
        // RMA path - zero runtime overhead
    }
}
```

### Compile-Time Fabric Queries

Use `fabric_query` for type-safe, compile-time capability requirements:

```cpp
#include <loom/loom.hpp>

using namespace loom;

// Define capability requirements at compile time
using rdm_query = fabric_query<rdm_tagged_messaging>;
using rma_query = fabric_query<rdma_write_ops, rdma_read_ops>;
using atomic_query = fabric_query<atomic_ops>;
using gpu_query = fabric_query<gpu_direct_rdma>;

// Combine multiple capability requirements
using full_query = fabric_query<rdm_tagged_messaging, rdma_write_ops, atomic_ops>;

// Query capabilities are computed at compile time
static_assert(rdm_query::get_endpoint_type() == endpoint_types::rdm);
static_assert(rma_query::get_required_caps().has(capability::rma));
static_assert(full_query::get_required_caps().has(capability::tagged));
static_assert(full_query::get_required_caps().has(capability::rma));
static_assert(full_query::get_required_caps().has(capability::atomic));

auto main() -> int {
    // Runtime query using compile-time hints
    auto providers = rdm_query::query();  // Returns result<provider_range>
    if (!providers) {
        return 1;
    }

    // Iterate over matching providers
    for (const auto& info : *providers) {
        auto fabric_attr = info.get_fabric_attr();
        if (fabric_attr) {
            std::println("Found provider: {}", fabric_attr->provider_name);
        }
    }

    // Or get just the first match
    auto first = atomic_query::query_first();  // Returns result<optional<fabric_info>>

    // Convenience function for common case
    auto tagged_providers = query_providers<rdm_tagged_messaging>();

    // Filter with a predicate
    auto filtered = query_providers<rdma_write_ops>([](const fabric_info& info) {
        auto attr = info.get_fabric_attr();
        return attr && attr->provider_name == "verbs";
    });

    return 0;
}
```

### Asio Integration

loom provides first-class integration with [Asio](https://think-async.com/Asio/) for
async I/O, supporting all Asio completion token types (callbacks, `use_awaitable`,
`use_future`, `deferred`):

```cpp
#include <loom/asio.hpp>
#include <asio/io_context.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>

auto main() -> int {
    using namespace loom;

    // Query for fabric provider
    auto info = query_fabric({.ep_type = endpoint_types::rdm}).value();
    auto fabric = fabric::create(info).value();
    auto domain = domain::create(fabric, info).value();
    auto cq = completion_queue::create(domain, {.size = 128}).value();
    auto av = address_vector::create(domain).value();

    // Create Asio io_context
    ::asio::io_context ioc;

    // Create an Asio-integrated endpoint using the convenience helper
    auto ep = loom::asio::make_endpoint(ioc, domain, info, cq, av).value();

    // Register the completion queue with the fabric service for polling
    auto& service = loom::asio::fabric_service::use(ioc);
    service.register_cq(cq);
    service.start();

    // Register memory for RDMA operations
    std::array<std::byte, 4096> buffer{};
    auto mr = memory_region::register_memory(
        domain, std::span{buffer},
        mr_access_flags::send | mr_access_flags::recv
    ).value();

    // Async send with callback
    ep.async_send(::asio::buffer(buffer), [](std::error_code ec, std::size_t n) {
        if (!ec) {
            std::println("Sent {} bytes", n);
        }
    });

    // Or use std::future
    auto future = ep.async_receive(::asio::buffer(buffer), ::asio::use_future);
    ioc.run();
    auto bytes = future.get();  // Blocks until receive completes

    return 0;
}
```

#### Coroutine Support

```cpp
#include <loom/asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

asio::awaitable<void> communicate(loom::asio::endpoint& ep) {
    std::array<std::byte, 1024> buf{};

    // Await async operations naturally
    std::size_t n = co_await ep.async_receive(
        ::asio::buffer(buf),
        ::asio::use_awaitable
    );
    std::println("Received {} bytes", n);

    // Tagged messaging with coroutines
    co_await ep.async_tagged_send(
        ::asio::buffer(buf),
        0x1234,  // tag
        ::asio::use_awaitable
    );
}

auto main() -> int {
    ::asio::io_context ioc;
    // ... setup endpoint as above ...

    ::asio::co_spawn(ioc, communicate(ep), ::asio::detached);
    ioc.run();
    return 0;
}
```

#### Registered Buffers

Use `registered_buffer` for zero-copy operations with RDMA-registered memory:

```cpp
#include <loom/asio.hpp>

// ... fabric/domain setup ...

std::array<std::byte, 4096> storage{};
auto mr = memory_region::register_memory(
    domain, std::span{storage},
    mr_access_flags::send | mr_access_flags::recv
).value();

// Create a DynamicBuffer backed by registered memory
loom::asio::registered_buffer buf(mr, std::span{storage});

// Use with async_read/async_write or directly
auto prepared = buf.prepare(1024);  // Get writable buffer
// ... receive data into prepared ...
buf.commit(bytes_received);         // Commit received data

auto data = buf.data();             // Get readable view
// ... process data ...
buf.consume(bytes_processed);       // Release processed data
```

#### Building with Asio

Enable Asio integration via CMake:

```bash
cmake -B build -DLOOM_USE_ASIO=ON
```

Or with Nix:

```bash
nix build .#loom-asio
nix develop .#asio  # Development shell with Asio
```

### P2300 std::execution Integration

loom provides first-class integration with P2300-style sender/receiver async programming.
This works with multiple implementations:

- [stdexec](https://github.com/NVIDIA/stdexec) (NVIDIA's reference implementation)
- [beman::execution](https://github.com/bemanproject/execution) (Beman project)
- `std::execution` (future C++26 standard)

The integration is designed to be generic - code written against loom's execution interface
will work with any conforming P2300 implementation with minimal changes.

```cpp
#include <loom/execution.hpp>
#include <stdexec/execution.hpp>  // Or beman/execution/execution.hpp

using namespace loom;

auto main() -> int {
    // ... fabric/domain/endpoint setup as in previous examples ...

    // Create sender factories for operations
    auto msg_ops = loom::async(ep);       // For messaging
    auto rma_ops = loom::rma_async(ep);   // For RMA

    // Create senders for async operations
    std::array<std::byte, 4096> buffer{};
    auto send_op = msg_ops.send(buffer, &cq);
    auto recv_op = msg_ops.recv(buffer, &cq);

    // Compose with P2300 algorithms
    auto pipeline = send_op
                  | stdexec::then([] { std::println("sent!"); });

    // Parallel operations with when_all
    std::array<std::byte, 4096> buf1{}, buf2{};
    auto both = stdexec::when_all(
        msg_ops.send(buf1, &cq),
        msg_ops.recv(buf2, &cq)
    );

    // Chain operations with let_value
    auto send_then_recv = msg_ops.send(buf1, &cq)
                        | stdexec::let_value([&] {
                              return msg_ops.recv(buf2, &cq);
                          });

    // Error handling
    auto with_error = msg_ops.send(buffer, &cq)
                    | stdexec::upon_error([](std::error_code ec) {
                          std::println(stderr, "Error: {}", ec.message());
                          return -1;
                      });

    return 0;
}
```

#### RMA Operations with Senders

```cpp
#include <loom/execution.hpp>

// ... setup ...

auto rma_ops = loom::rma_async(ep);

// RMA read sender
auto read_op = rma_ops.read(local_buffer, remote_addr, key, &cq);

// RMA write sender
auto write_op = rma_ops.write(local_buffer, remote_addr, key, &cq);

// Read-modify-write pattern
auto rmw = rma_ops.read(buffer, addr, key, &cq)
         | stdexec::then([&] {
               // Modify buffer
               process(buffer);
               return buffer;
           })
         | stdexec::let_value([&](auto& buf) {
               return rma_ops.write(buf, addr, key, &cq);
           });

// Atomic operations
auto atomic_op = rma_ops.fetch_add<std::uint64_t>(addr, 1, key, &cq)
               | stdexec::then([](std::uint64_t old_val) {
                     return old_val + 1;
                 });
```

#### Scheduler

loom provides a scheduler type for scheduling work on a completion queue context:

```cpp
#include <loom/execution.hpp>

// Create a scheduler from a completion queue
auto cq_ptr = std::make_shared<loom::completion_queue>(std::move(cq));
loom::scheduler sched{cq_ptr};

// Schedule work on the CQ context
auto work = stdexec::schedule(sched)
          | stdexec::then([] { return 42; });

// Use with starts_on for explicit context control
auto on_cq = stdexec::starts_on(sched, some_sender);
```

#### Building with P2300

Enable stdexec integration:

```bash
cmake -B build -DLOOM_USE_STDEXEC=ON
```

Or with beman::execution:

```bash
cmake -B build -DLOOM_USE_BEMAN_EXECUTION=ON
```

With Nix (recommended):

```bash
nix build .#loom-stdexec
nix develop .#stdexec  # Development shell with stdexec
```

#### Backend Detection

The P2300 backend is automatically detected at compile time:

```cpp
#include <loom/execution.hpp>

if constexpr (loom::execution::has_execution_backend) {
    std::println("Using: {}", loom::execution::execution_backend_name);
    // Prints: "stdexec", "beman::execution", or "std::execution"
}

// Check for specific features
if constexpr (loom::execution::features::has_when_all) {
    auto both = loom::execution::when_all(sender1, sender2);
}
```

## Potential Future Work

- competing implementation of [NCCL/RCCL ofi plugin](https://github.com/aws/aws-ofi-nccl).
- competing implementation of [nvshmem ofi plugin](https://github.com/NVIDIA/nvshmem/tree/devel/src/modules/transport/libfabric).
- competing implementation of [nixl ofi plugin](https://github.com/ai-dynamo/nixl/tree/main/src/plugins/libfabric).
- stability/performance improvements, bug fixes (surely there are a few in this early draft.)
- builds/ci, mostly targeting manylinux targets.
- convert doxygen to html or markdown.
- rust bindings via [cxx](https://docs.rs/cxx/latest/cxx/) which compete with libfabric-sys in upstream libfabric but work better with tokio.
- zig bindings and integration with their new async stuff.
