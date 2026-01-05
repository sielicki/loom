# loom-fabric

Rust bindings for [loom](https://github.com/sielicki/loom), a modern C++23 libfabric binding library.

## Overview

`loom-fabric` provides safe, ergonomic Rust wrappers around loom's C++ API via [cxx](https://cxx.rs/), enabling high-performance RDMA and fabric networking from Rust applications.

## Features

- **Type-safe**: Strong types prevent common errors
- **Async-ready**: Optional tokio integration for async I/O
- **Zero-copy**: Direct access to registered memory regions
- **Provider-agnostic**: Works with any libfabric provider (verbs, EFA, TCP, etc.)

## Requirements

- Rust 1.70+
- C++23 compiler (clang 16+ or gcc 13+)
- libfabric 1.18+

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
    endpoint.send(data, 0)?;

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
    endpoint.send(data).await?;

    // Async receive
    let mut buf = vec![0u8; 1024];
    let n = endpoint.recv(&mut buf).await?;
    println!("Received {} bytes", n);

    Ok(())
}
```

## Building from Source

```bash
# From the loom repository root
cmake -B build -DLOOM_BUILD_RUST_BINDINGS=ON
cmake --build build

# Or directly with cargo
cd bindings/rust
cargo build
```

## License

BSD-2-Clause OR GPL-2.0-only (same as loom and libfabric)
