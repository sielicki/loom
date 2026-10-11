# Open MPI integration

Build the pinned Open MPI OFI baseline on Linux:

```sh
nix build .#openmpi-consumer --cores 4
./result/bin/openmpi-ofi-check
```

The check runs the two-rank workload with sockets and TCP/RxM. To run one
provider directly:

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
packing and unpacking, truncation, cancellation, zero-byte messages, and
collective progress. The configuration is host memory with ordinary RDM
endpoints. Probes and claimed receives run in both directions and verify
source, tag, and message length. It does not establish GPU,
scalable-endpoint, or NIC support.

This package uses Open MPI's existing OFI backend. It establishes the
consumer build and acceptance workloads for the forthcoming Loom adapter;
these transfers do not yet run through Loom.

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
