# Monarch integration

Build Monarch's Loom backend and run its host-memory workloads on Linux:

```sh
nix build .#monarch-loom --cores 4
./result/bin/monarch-rdma-tests --exact \
  backend::loom::tests::sockets_process_round_trip
./result/bin/monarch-rdma-tests --exact \
  backend::loom::tests::rxm_process_round_trip
```

The build runs nine Loom workloads and eleven upstream TCP baseline tests.
Loom tests disable both TCP fallback and ibverbs, require the typed Loom
backend handle and remote context, and transfer data through Monarch's
`RdmaAction` interface. They cover sockets and TCP/RxM, multiple peers,
same-worker transfers, repeated actions, and separate operating-system
processes. The separate-process target exports memory and waits on a
bootstrap socket while its worker progresses RMA. That socket carries
serialized Monarch metadata and coordination only; payloads use Loom.

Three deterministic retirement tests delay CQ consumption after native
acceptance. They verify that observer drop, timeout, and a later invalid
operation in a partially submitted batch retain the first operation's
allocation, registration, and context until its completion is consumed.
The invalid operation and subsequent unsubmitted operations are not posted.
Shutdown checks retain cached backend handles while stopping their actors
and verify that the workers still exit.
Another delayed-CQ workload submits 320 writes to disjoint remote subranges,
verifies the 256-operation native limit, and checks that progress resumes
without duplicate submissions when completion consumption is restored.

## Backend contract

The patched Monarch backend is disabled by default. Enable it with
`MONARCH_RDMA_ALLOW_LOOM=true`. `MONARCH_RDMA_LOOM_PROVIDER` and
`MONARCH_RDMA_LOOM_DOMAIN` are discovery filters; empty values impose no
filter. Capabilities and obligations are checked against returned typed
attributes, without inferring behavior from names.

The initial profile is CPU memory, IPv4 RDM, registered reads and writes,
ordinary scalar keys, and context/context2 storage. It rejects endpoint-bound,
raw-key, and other unsupported registration requirements. Metadata carries a
schema version, numeric protocol identity, portable IPv4 address, remote
address model, length, and a 64-bit key. Compatibility checks happen before
native submission. Its reported transport level is conservatively `Tcp`;
discovery does not establish NIC throughput or GPU support.

Each backend has one worker thread. All Loom resources are created, used,
and destroyed there; actors hold a `Send + Sync` command handle. The worker
polls while idle, including while only exporting memory. A progress pass
reads at most 64 CQ entries, handles at most eight commands, and attempts
one operation per active batch. There are at most 64 queued commands,
64 active batches, 64 operations per batch, 256 accepted native operations,
1024 exports, and 1024 cached
peer addresses. Rejected `FI_EAGAIN` submissions retain their preparation
for retry; accepted submissions are never reposted. Conflicting local
read/write ranges are serialized.

Writes request delivery completion and then await the target's visibility
acknowledgment through Monarch's actor transport. Metadata schema 2 binds
that request to the native endpoint address, export ID, and a registration
generation that is never reused by the worker. Released, stale, or mismatched
exports fail the acknowledgment. The current host profile needs no device
flush; it exercises the same request and failure path planned for GPU writes.
A fault test verifies that delivered bytes cannot turn stale metadata, an
unrelated control actor, or a target visibility failure into transfer success.

Successful reads permit local buffer
access. Timeout or observer drop does not grant buffer reuse: accepted DMA
retains its leases, while expiration stops unsubmitted work. External access
must remain excluded until that work retires. Remote users must quiesce
before releasing an export. Zero-length buffers are currently rejected.

A child actor owns worker shutdown, so Monarch's cached backend handles
cannot keep a stopped actor's worker alive. Shutdown stops new submissions,
drains accepted work, and closes the endpoint before releasing registrations.
Failed close retains the native resource graph and leases. Draining can
still block when a provider fails to retire work; bounded failure recovery
remains open. GPU registration and visibility, multirail support, reusable
allocation-aware registration caches, and allocation-free operation are
additional milestones. The exercised providers run on loopback; these tests
make no claim about RDMA hardware or GPU correctness.

## Pinned build

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

The baseline build checks eleven upstream TCP-backend tests: reads, writes, round
trips, multi-chunk transfers, released-buffer errors, and transfers between
buffers owned by the same actor process. Each selected test must exist and
finish within 60 seconds. `monarch-consumer` uses Monarch's existing backend;
`monarch-loom` additionally applies the Loom backend patch and builds against
this checkout's Rust bindings and C++ sources.

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

Use `nix develop .#monarch-loom` for the patched backend. The same copy
commands above produce its writable source tree; run
`cargo test --locked -p monarch_rdma --lib backend::loom::tests::` there.
The ignored `process_target` test is launched automatically by the two
process workloads and requires their bootstrap environment.
