# loom-fabric

Rust bindings for [loom](https://github.com/sielicki/loom), a modern C++26 libfabric binding library.

## Overview

`loom-fabric` provides safe, ergonomic Rust wrappers around loom's C++ API via [cxx](https://cxx.rs/), enabling high-performance RDMA and fabric networking from Rust applications.

## Features

- **Type-safe**: Strong types prevent common errors
- **Async-ready**: Optional tokio integration for async I/O
- **Zero-copy**: Direct access to registered memory regions
- **Provider-agnostic**: Works with any libfabric provider (verbs, EFA, TCP, etc.)

## Requirements

- Rust 1.70+
- C++26 compiler (Clang 18+ or GCC 14+ with a C++26-capable standard library)
- libfabric 2.7.0 or newer

## Installation

Add to your `Cargo.toml`:

```toml
[dependencies]
loom-fabric = "0.1"

# For async/tokio support:
loom-fabric = { version = "0.1", features = ["tokio"] }
```

## Usage

### Basic Example

```rust
use loom_fabric::{
    query_fabric, Fabric, Domain, Endpoint, CompletionQueue,
    Capabilities, CqBindFlags,
};

fn main() -> loom_fabric::Result<()> {
    // Query for a fabric provider
    let info = query_fabric(Capabilities::MSG | Capabilities::TAGGED)?;

    // Create fabric resources
    let fabric = Fabric::new(&info)?;
    let domain = Domain::new(&fabric, &info)?;
    let cq = CompletionQueue::new(&domain, 128)?;
    let mut endpoint = Endpoint::new(&domain, &info)?;

    // Bind and enable
    endpoint.bind_cq(&cq, CqBindFlags::TRANSMIT | CqBindFlags::RECV)?;
    endpoint.enable()?;

    // Send data
    let data = b"Hello, fabric!";
    // SAFETY: data remains alive and unchanged through the completion below.
    unsafe { endpoint.send(data, 0)? };

    // Wait for completion
    let event = cq.wait(Some(1000))?; // 1 second timeout
    println!("Sent {} bytes", event.bytes_transferred);

    Ok(())
}
```

### Async Example (with tokio)

```rust
use loom_fabric::{
    query_fabric, Fabric, Domain, AddressVector,
    AsyncEndpoint, Capabilities,
};

#[tokio::main]
async fn main() -> loom_fabric::Result<()> {
    let info = query_fabric(Capabilities::MSG)?;
    let fabric = Fabric::new(&info)?;
    let domain = Domain::new(&fabric, &info)?;
    let av = AddressVector::new(&domain)?;

    let mut endpoint = AsyncEndpoint::new(&domain, &info, 128)?;
    endpoint.bind_av(&av)?;
    endpoint.enable()?;

    // Async send
    let data = b"Hello, async fabric!";
    // SAFETY: this future is awaited to completion without cancellation.
    unsafe { endpoint.send(data).await? };

    // Async receive
    let mut buf = vec![0u8; 1024];
    // SAFETY: buf is not accessed until completion; the future is not cancelled.
    let n = unsafe { endpoint.recv(&mut buf).await? };
    println!("Received {} bytes", n);

    Ok(())
}
```

### Provider discovery

`query_fabrics(&FabricHints)` enumerates matching configurations, including
provider, fabric, and domain filters. Each `FabricInfo` retains its native
discovery data independently; selecting a later entry and dropping the
rest is safe. `FabricInfo::attributes()` returns an owned snapshot with
typed endpoint, address, threading, progress, mode, and registration
requirements, along with numeric protocol identity and provider limits.

Names are selection filters and diagnostic identities. Validate the returned
attributes before creating resources; names do not establish capabilities.
Unknown native mode requirements cause `attributes()` to fail explicitly,
so the caller can examine another entry. Device registration can still fail
after discovery reports HMEM support. Endpoint threading can be reported,
but requesting that threading hint is currently unsupported by the core.

The existing `query_fabric` and `query_fabric_with_hints` functions continue
to return the first result. Use enumeration when device or domain selection
matters. Discovery does not yet expose NIC topology, tag layout, CQ data
width, or the complete TX/RX attributes.

### Async progress

Async CQs use native wait descriptors when available. If a provider cannot
expose its wait object, waits poll with a one-millisecond Tokio timer between
empty reads. Enable time in the Tokio runtime. `AsyncCompletionQueue::with_polling`
and `AsyncEndpoint::with_polling` also support providers that reject native
wait objects during CQ creation. `AsyncCompletionQueue::wait_fd()` returns
an optional borrowed descriptor; it replaces the unconditional `AsRawFd` API.
An exported region on a manual-progress provider still needs an active
progress task for its entire advertised lifetime, including while idle.

Managed messaging and RMA futures share an endpoint borrow, so multiple sends,
receives, reads or writes can remain outstanding on one `AsyncEndpoint`. Poll
them together, for example with `futures::join!`, using nonconflicting buffers.
Native calls are serialized internally, and the endpoint remains confined to
one thread. Configuration, replacement and close require exclusive access after
those futures finish or are dropped. Dropping a future still does not cancel
accepted native work or permit its buffer to be reused.

### Registered RMA

`Endpoint::read` and `Endpoint::write` submit a registered local slice to
an explicit `RmaTarget`. Build its `peer` with the endpoint's bound address
vector and exchange `MemoryRegion::remote_memory()` with the remote process.
The exported address follows the negotiated registration model, including
MR-relative offsets, and the key remains 64 bits. `RemoteMemory::subregion`
checks range and address arithmetic before narrowing the exported view.

`Endpoint::read_raw` and `write_raw` take a pointer and byte count without
constructing a Rust reference to the allocation. They preserve the same
bounds, domain, completion, and lifetime rules as the slice methods. This
allows callers to represent device addresses without treating them as host
memory. GPU visibility remains the caller's responsibility.

`MemoryRegion::register_with_options` accepts a `MemoryRegistration` with
typed `HmemDevice` and `HmemAllocationKind` attributes. Device variants carry
the actual CUDA, ROCm, Level Zero, Neuron, or SynapseAI ordinal. Device and
host allocation flags describe allocations made through that interface;
ordinary system memory uses `Unspecified`. Negative ordinals and device
allocation flags on the system interface are rejected before registration.
Provider errors never trigger a fallback to system memory.

For domains requiring `MrMode::ENDPOINT`, use the unsafe
`MemoryRegion::register_for_endpoint(&mut endpoint, ptr, len, attributes)`
factory. It registers, binds, and enables the region before returning it.
The endpoint-free factories reject this mode before native registration.
The new factory also accepts ordinary domains, where no binding is needed.

Endpoint-bound registration clones retain the endpoint, its bound queues and
address vectors, and its domain. Dropping the endpoint wrapper defers native
close until the last registration is released. Explicit close returns
`Error::BoundRegistrations` while these registrations remain alive, including
clones retained by accepted async operations. Progress those operations to
retirement and release the registrations before retrying close. This ownership
does not extend the caller's allocation lifetime. Use `AsyncEndpoint::inner_mut`
to create registrations before starting managed operations.

`MemoryRegion::key()` and `remote_memory()` now return `Result`. Check their
results before exchanging metadata: raw-key registrations and unavailable keys
cannot be exported as ordinary scalar descriptors. Raw-key mapping is available
through the C++ API; the Rust scalar interface rejects those registrations.

Negotiate `Capabilities::HMEM` and satisfy the returned MR requirements
before registering accelerator memory. Retain the allocation through all
registration clones and accepted operations. Registration does not synchronize
device producers or establish completion visibility. Those obligations remain
with the consumer.

Consumers that require transfers without provider CUDA API calls can set
`Endpoint::set_cuda_api_permitted(false)` before enabling the endpoint.
An unsupported or rejected option is an error and must prevent selecting
that transfer profile. Application registration and visibility operations
may still call CUDA APIs.

Each operation checks the registration's domain and any endpoint binding,
the local buffer bounds, and the remote view's length.
`RmaOptions` selects local, transmit, or delivery completion and
optional fencing; a CQ completion is always requested. Unsupported policies
remain ordinary submission errors. Local completion permits local buffer
reuse but does not establish remote delivery or GPU visibility.

Use `OperationContext::new()` for stable native context storage with the
low-level endpoint, retaining one context per accepted operation until its
completion or successful close. `AsyncEndpoint::read` and `write` manage
these contexts and retain a clone of the registration until actual
retirement, including after observer drop or a queue error. Memory-region
clones share one native registration. These APIs still borrow the allocation:
reads prohibit local buffer access, and writes prohibit local mutation,
until the accepted operation retires. Dropping an observer is not cancellation.

The provider tests exercise host-memory scalar transfers on sockets and
TCP/RxM and reject invalid device attributes. Native-operation fixtures cover
endpoint-bound registration, failure cleanup, and resource destruction order.
GPU hardware validation, DMA-BUF registration, owned allocation leases,
immediate data, and vectored RMA remain additional Rust frontend work.

### Submission errors and backpressure

`send`, `recv`, tagged and RMA submissions, and `inject` return
`Error::Submission { code }` when libfabric rejects an operation. `code`
retains the negative native status. `error.is_would_block()` identifies
`FI_EAGAIN` using the linked libfabric constants; `error.is_timeout()`
likewise recognizes `FI_ETIMEDOUT`.

A rejected submission accepted no work. Drive the progress required by
the selected provider before retrying, and retry only rejected work in a
partially accepted batch. Successful submissions keep their buffer and
context lifetime obligations until completion. `Error::Completion`
describes a failure after acceptance and is kept distinct from a retryable
submission rejection. C++ exceptions remain `Error::Cxx`.

`CompletionEvent` preserves the receive-buffer address, optional source address,
and native error metadata. `error_info: Some(...)` distinguishes an error entry
with zero-valued fields from an event without that information. Provider error
numbers, binary diagnostic bytes (including embedded zeros), and truncation
overflow length survive further reads and queue destruction. Buffer and context
addresses are opaque values; the event does not retain those allocations.

Use `CompletionQueue::with_attributes` or `AsyncCompletionQueue::with_attributes`
with `CompletionQueueOptions { source_addresses: true, ..Default::default() }`
to request source-preserving reads. Bound endpoints must negotiate
`Capabilities::SOURCE`; unavailable sources remain `None`, while address zero
is preserved as `Some(0)`.

API change: `CompletionEvent` is `Clone` rather than `Copy`, and
`Error::Completion { event }` now owns the complete failed event. Match
`event.context` and `event.error` in place of the former `context` and `code`
fields. `into_result()`, synchronous waits and async routing preserve the same
metadata, including when an observer is dropped before taking its result.

Async submissions allocate native `fi_context2` scratch storage. The bound
completion queue retains it when a future is dropped and releases it when
the matching completion is consumed. Polling a clone of the underlying
queue performs the same retirement. User buffers remain borrowed independently
of the future; registered RMA additionally retains its registration lease.

`Endpoint::close` and `AsyncEndpoint::close` expose native shutdown errors
as `Error::Close { code }`. Successful close discards outstanding work and
may discard CQ entries without reporting completions; it permits buffer
reuse. Async endpoint close also releases managed contexts. Failed close
preserves ownership for retry. If destruction cannot close an endpoint, it retains the native
resource graph and contexts instead of freeing storage still in use.
`AsyncEndpoint::inner_mut` now returns a `Result`: pending managed work
prevents replacing the endpoint, and a closed async endpoint cannot reopen
on queues that may still contain old completions.

## Building from Source

```bash
# From the loom repository root
cmake -B build -DLOOM_BUILD_RUST_BINDINGS=ON
cmake --build build

# Or directly with cargo
cd bindings/rust
cargo build
```

Run the ordinary tests with `cargo test --all-features`. This includes the
`test_fixtures` feature, which builds deterministic native CQ fixtures to check
metadata ownership across the bridge without hardware. It is disabled by default.
The TCP receive queue saturation regression requires that software provider
explicitly:

```bash
cargo test --all-features receive_backpressure_survives_the_cpp_bridge -- --ignored
cargo test --all-features dropped_receives_retain_contexts_until_close -- --ignored
cargo test --all-features completion_decoding_retires_only_its_native_context -- --ignored
cargo test --all-features --test rma -- --include-ignored
cargo test --all-features --test discovery -- --include-ignored
```

## License

BSD-2-Clause OR GPL-2.0-only (same as loom and libfabric)

Borrowed-buffer submissions are unsafe: libfabric keeps using buffers after
submission, and dropping a Rust future does not cancel the fabric operation.
Keep submitted buffers and registrations alive until their completions are
consumed. Receive buffers must remain inaccessible and send buffers must
remain unmodified while an operation is pending. An unrelated queue error
also does not release an outstanding operation's buffer.
