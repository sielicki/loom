# NIXL integration

The consumer build is pinned to NIXL commit
`b69182fe73ac64221dcdbeeee744161f76006dda` in the root flake. NIXL is built
in C++26 mode using the same Nix compiler as Loom.

Build the consumer on Linux:

```sh
nix build .#nixl-consumer --no-link
nix build .#checks.x86_64-linux.nixl-consumer --no-link
```

The build installs the NIXL library, backend headers, and a `nixl.pc`
pkg-config module. The flake check builds an external C++26 consumer in a
separate derivation using only the installed package, constructs an agent,
and loads the packaged POSIX plugin through NIXL's public API. Taskflow's pkg-config
metadata is supplied locally; build dependencies come from the pinned
flake inputs. Replace `x86_64-linux` in the check command with the native
Linux system when appropriate.

Build and run the Loom backend:

```sh
nix build .#nixl-loom
./result/bin/nixl-loom-workload sockets
./result/bin/nixl-loom-workload 'tcp;ofi_rxm'
./result/bin/nixl-loom-workload sockets --processes
./result/bin/nixl-loom-workload 'tcp;ofi_rxm' --processes
```

The build runs all four workloads, two plugin ABI contract checks, and an
injected CUDA driver policy test. The installed executable sets
`NIXL_PLUGIN_DIR` to its packaged `libplugin_LOOM.so`; transfers explicitly
select the LOOM backend. To develop with the same dependencies:

```sh
nix develop .#nixl-loom
cmake -S examples/nixl -B build/nixl -DCMAKE_BUILD_TYPE=Debug
cmake --build build/nixl
ctest --test-dir build/nixl --output-on-failure
```

The exercised configuration is Linux host memory, IPv4 RDM endpoints on
loopback, and the sockets and TCP/RxM providers from Loom's pinned libfabric.
The backend validates a typed profile requiring registered RMA, messages,
thread-safe resources, and supported native context modes. Provider names
only select discovery results; they do not establish capability guarantees.
Peer metadata carries a versioned numeric envelope and protocol identity.
Memory metadata also preserves the descriptor's memory space and device ID;
imports and prepared transfers reject mismatched identities. Memory envelope
version 3 and connection envelope version 2 are required on both peers;
older metadata is rejected.

Each backend owns a worker that polls even when its agent has no local
requests. Submission and progress share a mutex. Up to 64 prepared requests,
4096 segments per request, and 64 queued standalone notifications are
supported. Each progress pass polls at most 64 CQ entries and attempts one
segment per active request. `FI_EAGAIN` leaves that segment queued; accepted
segments are not reposted. Requests retain their local registrations and
peer addresses until release. Application allocations must remain alive and
free of conflicting accesses until the transfer completes or release returns.

Writes request delivery completion, then ask the receiving backend to make
each target registration visible. The sender waits for an acknowledgement
before reporting success or sending the transfer notification. This protocol
also runs for host registrations, exercising the same completion path in
CPU tests. Reads establish local visibility before completing.

Visibility messages carry nonreused request and registration IDs and contain
at most 128 registration IDs per packet. Larger transfers wait for successive
acknowledgements. Notifications, visibility requests, and replies use distinct
tags and receive storage; a full application notification queue cannot consume
the control receives. Reply storage is bounded separately. Stale registration
IDs and visibility failures fail the transfer and suppress its notification.

Transfer notifications preserve binary payloads
and sender identity. The maximum packet is 8192 bytes or the provider's
message limit, whichever is smaller, including a 24-byte header and the
sender name. Oversized notifications are rejected. Receive storage and
notification queues are bounded, applying backpressure when the application
does not drain notifications.

Release stops unsubmitted segments and drains accepted DMA before returning.
Destruction also retires posted receives before deregistering their storage
and closing the endpoint. Draining and close can block when a provider or
peer fails to retire work; bounded failure recovery is still open.

The workloads check registered reads and writes to distinct peers,
discontiguous subranges and untouched guard bytes, prepared-request reuse,
binary and empty notifications, notification bursts, write visibility before
notification, invalidated metadata, and active-request release. A separate
process exports memory and waits on a bootstrap socket while Loom's worker
services remote reads and writes. The bootstrap socket exchanges metadata
and test coordination only; payload transfers go through NIXL and Loom.
The plugin ABI checks also cover 257-segment batches, request reuse, 96 queued
notifications, and a rejected visibility acknowledgement after native RMA
has completed. Failed reposts preserve the previous completed request state.

## Experimental CUDA memory

Pass `{"cuda", "1"}` in the backend parameters to enable `VRAM_SEG`
registrations. The default is `0`; other values are rejected. Select a
provider and domain capable of direct GPU transfers. For example:

```cpp
nixl_b_params_t parameters{
    {"provider", "verbs;ofi_rxm"},
    {"domain", "mlx5_0"},
    {"cuda", "1"},
};
```

Replace those discovery filters with the runner's configuration. Setup
requires a loadable CUDA driver, GPUDirect support and a supported visibility
path on every visible GPU, `FI_HMEM`, and an endpoint that accepts
`FI_OPT_CUDA_API_PERMITTED=false`. This excludes provider copies that could
synchronize a running GPU kernel waiting for the transfer. Capability
decisions use numeric attributes. The build needs CUDA driver headers;
CUDA libraries are loaded only when this mode is enabled.

Register CUDA device allocations as `VRAM_SEG`, with `devId` set to the
allocation's actual CUDA ordinal. The backend validates the pointer's device,
owning context and GPUDirect eligibility, enables `SYNC_MEMOPS`, and registers
it through Loom with CUDA device-only metadata. Managed memory, contextless
virtual-memory allocations and mismatched device IDs are rejected. DMA-BUF,
ROCr and Level Zero registrations are not implemented in this adapter yet.

Device registrations retain the owning context identity. Read completion
flushes the local allocation. Write acknowledgement flushes the target
allocation in the receiving backend's worker. Both use the shared
[CUDA visibility policy](../hmem/cuda.hpp), which restores the worker's
previous context and establishes visibility to all CUDA devices without
synchronizing a whole device. Applications must finish producing source data
before posting; allocations and contexts must outlive all operations, and
remote registrations must remain valid through acknowledgement.

All seven automated checks pass on software providers and with address,
undefined-behavior and leak sanitizers. They test the wire protocol using
host memory and CUDA policy using an injected API. **Actual CUDA RMA and
GPU visibility have not been validated on hardware.** A NIXL workload with
an already running GPU verifier, multiple devices, multiple processes,
and injected driver/provider failures remains required. Multiple rails,
bounded failure recovery, and allocation-free progress are also open.
