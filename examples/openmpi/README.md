# Open MPI integration

Build the Loom-backed OFI MTL on Linux:

```sh
nix build .#openmpi-loom --cores 4
./result/bin/openmpi-ofi-check
```

The patch routes transport creation, address-vector insertion/removal,
message submission, injection, cancellation, and completion progress through
the C++26 Loom bridge. Open MPI retains discovery, process address exchange,
tag selection, datatype packing, matching, and synchronous-send acknowledgements.
The workload checks that Loom owns one live transport after `MPI_Init_thread`
and that `MPI_Finalize` closes it. Every patched MTL payload submission uses
Loom; there is no alternate payload transport in the adapter.

The initial profile is host memory, ordinary RDM endpoints, and IP addresses.
Scalable endpoints and native HMEM must be disabled explicitly. Discovery
entries requiring host registration, endpoint-bound or raw-key registration,
or unsupported context/buffer modes are rejected before resource creation.
The complete selected discovery entry is copied into Loom, preserving native
TX/RX defaults, ordering, CQ-data widths, and tag masks. Capabilities are
checked numerically; provider names are only discovery filters.

Open MPI owns each request and its provider context. A matched probe retains
that same context until the claimed receive completes. Loom delivers batches
of completions to Open MPI's existing callbacks, including probe no-match,
truncation, and cancellation errors. Error diagnostics remain owned through
callback return. The callback adapter preserves the fields used by these MTL
callbacks; native overflow length is not exposed by Loom's completion event.

The tested threading target is `MPI_THREAD_FUNNELED`. Open MPI retains its
submission/progress synchronization policy. Finalization closes the endpoint
before releasing the AV, CQ, domain, and fabric. Failed endpoint close retains
that graph and reports an error. Bounded retirement after provider failure,
GPU visibility, hardware providers, scalable contexts, and the OFI BTL remain
separate milestones. This integration makes no allocation-free or performance
claim.

The bridge's configuration-rejection and failed-close retention checks pass
with ASan, UBSan, and leak detection. Full MPI-process leak checking is still
open: the workload completes, but shutdown reports allocations retained by
PMIx and Open MPI, also reproduced with the unmodified OFI baseline. The
upstream memory patcher conflicts with ASan's interception; diagnostic MPI
runs used `--mca memory '^patcher'` while keeping sanitizer and leak checks
enabled. The ordinary acceptance runs retain the default memory-hook policy.

Build the pinned Open MPI OFI baseline on Linux:

```sh
nix build .#openmpi-consumer --cores 4
./result/bin/openmpi-ofi-check
```

Both packages install the same check entry point. It runs the two-rank workload
with sockets and TCP/RxM, using both CQ-data (`ofi_tag_full`) and tag-encoded
(`ofi_tag_1`) source ranks. To run one provider directly:

```sh
./result/bin/mpirun --np 2 --oversubscribe --bind-to none \
  --mca pml cm --mca mtl ofi \
  --mca mtl_ofi_enable_sep 0 --mca mtl_ofi_disable_hmem 1 \
  --mca mtl_ofi_provider_include sockets \
  ./result/bin/openmpi-ofi-workload
```

Use `'tcp;ofi_rxm'` as the provider filter to exercise TCP/RxM. The Nix
build runs the upstream `make check` suite. Run the installed MPI workloads
outside the Nix build sandbox: the upstream OFI component requests both
local and remote communication, requiring an active non-loopback IP
interface even for these same-host transfers. A sandbox with only loopback
cannot satisfy that discovery request. Workload failures propagate to the
check's exit status; each provider has a 90-second deadline. Explicit CM
PML and OFI MTL selection prevents fallback to another point-to-point transport.

The workload checks unmatched probes, claimed-message isolation while
processing unrelated traffic, matched receives, synchronous sends, datatype
packing and unpacking, a 256 KiB transfer in each direction, truncation,
cancellation, zero-byte messages, and
collective progress. The configuration is host memory with ordinary RDM
endpoints. Probes and claimed receives run in both directions and verify
source, tag, and message length. It does not establish GPU,
scalable-endpoint, or NIC support.

The `openmpi-consumer` package uses Open MPI's existing OFI backend and serves
as the comparison baseline. Select `openmpi-loom` to exercise Loom.

Open MPI is pinned to `873cd3a294c7194a5f88937de36d5bc0b1c18d88`.
Its Open MPI Autoconf macros and MPI binding-generator metadata use the
exact submodule revisions recorded by that commit. PMIx, PRRTE, hwloc,
libevent and the compiler come from the locked Nix dependency set; libfabric
uses Loom's pinned and patched build.

Develop against the same sources and configure options:

```sh
nix develop .#openmpi-consumer
openmpi_work=$(mktemp -d)
cp -R "$OPENMPI_SOURCE/." "$openmpi_work"
chmod -R u+w "$openmpi_work"
cd "$openmpi_work"
./autogen.pl --no-3rdparty=libevent,hwloc,pmix,prrte
openmpi-configure --prefix="$PWD/install"
make -j4
make check
make install
```

For the adapter, use `nix develop .#openmpi-loom` instead and apply its patch
after copying the source, before running `autogen.pl`:

```sh
patch -p1 < "$OPENMPI_LOOM_PATCH"
```

The bridge is also available separately as `.#openmpi-bridge`. It builds
against the installed Loom package using only public headers and exports a
C ABI to keep the upstream component's C sources and request layouts intact.
