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

The build runs all four workloads and two plugin ABI contract checks. The
installed executable sets `NIXL_PLUGIN_DIR` to its packaged
`libplugin_LOOM.so`; transfers explicitly
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
version 2 is required on both peers; older metadata is rejected.

Each backend owns a worker that polls even when its agent has no local
requests. Submission and progress share a mutex. Up to 64 prepared requests,
4096 segments per request, and 64 queued standalone notifications are
supported. Each progress pass polls at most 64 CQ entries and attempts one
segment per active request. `FI_EAGAIN` leaves that segment queued; accepted
segments are not reposted. Requests retain their local registrations and
peer addresses until release. Application allocations must remain alive and
free of conflicting accesses until the transfer completes or release returns.

Writes request delivery completion. Transfer notifications are submitted
after all data operations complete; notifications preserve binary payloads
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

These are software-provider acceptance tests. GPU/HMEM, multiple rails,
injected partial-submission failures, and NIC-specific configurations remain
separate validation work. No GPU-visibility or allocation-free claim is made.
