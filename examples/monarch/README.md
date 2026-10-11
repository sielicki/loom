# Monarch integration

The consumer build pins Monarch to
`dd1bb6719b2cf2b6bdf8851df02f092a900e6f5e`, its Rust toolchain to
`nightly-2026-07-05`, and rdma-core to the revision selected by Monarch's
native build. Cargo dependencies are vendored from the upstream lockfile;
compilation runs offline. The root flake also pins the Nix dependencies.
One build patch makes Monarch's private rdma-core source copy writable
when its input comes from the read-only Nix store.
The build also copies CXX's shared build script into its vendored member
crates, preserving the behavior of Monarch's pinned CXX fork.
Another source patch fixes a race in generic type-name interning: concurrent
cache misses now retain one name without leaking the losing allocation.
The build runs typeuri's three unit tests, including a deterministic race
regression and its existing nested-type checks.

Build the RDMA crate's test harness and run its host-memory checks on Linux:

```sh
nix build .#monarch-consumer --cores 4
./result/bin/monarch-rdma-tests --exact \
  backend::tcp::manager_actor::tests::test_tcp_write_then_read_back
```

The build checks eleven upstream TCP-backend tests: reads, writes, round
trips, multi-chunk transfers, released-buffer errors, and transfers between
buffers owned by the same actor process. Each selected test must exist and
finish within 60 seconds. These tests use Monarch's existing backend; a
Loom backend and workload are still to be added.

Monarch's native RDMA crate compiles CUDA sources even for host-memory
tests. This package provides the pinned CUDA toolkit, including NVIDIA's
licensed compiler and runtime, and builds the real native dependencies.
Running the selected host tests requires neither a CUDA device nor an RDMA
NIC. This build does not establish GPU support or hardware correctness.

For development against the same patched source and build dependencies:

```sh
nix develop .#monarch-consumer
monarch_work=$(mktemp -d)
cp -R "$MONARCH_SOURCE/." "$monarch_work"
chmod -R u+w "$monarch_work"
cd "$monarch_work"
cargo test --locked -p monarch_rdma --lib \
  backend::tcp::manager_actor::tests::test_tcp_write_then_read_back -- --exact
```

The shell exports `CUDA_HOME`, `MONARCH_RDMA_CORE_SRC`, and
`MONARCH_GPU_PLATFORM` and includes Monarch's nightly Cargo and Rust
compiler. Initial Cargo dependency fetching in the development shell may
use the network; the packaged build uses the fixed vendor derivation.

The planned Loom adapter belongs at Monarch's `RdmaBackend` boundary. Its
worker must retain allocations and registrations until accepted operations
retire, continue progressing exported memory while idle, and preserve
those obligations after timeout or observer drop. Rust resource discovery,
thread ownership, and GPU registration remain integration work.
