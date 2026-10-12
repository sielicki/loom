# loom

[<img src="https://upload.wikimedia.org/wikipedia/commons/thumb/7/79/Warp-weighted_loom_twill.jpg/250px-Warp-weighted_loom_twill.jpg">](https://en.wikipedia.org/wiki/Loom)

C++ bindings for libfabric, done right.

loom provides an opinionated, idiomatic, and strongly-typed C++26 interface over [libfabric](https://github.com/ofiwg/libfabric), which is a low-level vendor-agnostic fabric abstraction layer for HPC/RDMA networking.

## license

`BSD-2-Clause OR GPL-2.0-only` (identical to ofiwg/libfabric, see [COPYING](./COPYING))

## Requirements

- C++26 compiler
- libfabric 2.7.0 or newer

The Nix flake pins upstream libfabric 2.7.0 and retains the external
libfabric flake's provider packaging. Use `nix develop` for the development
environment or `nix build .#libfabric` to build that dependency directly.
`flake.lock` records the exact source revision. CMake can also use an
installed libfabric through `pkg-config`.

The Nix dependency patches name-server cleanup, PSM3 endpoint retirement,
and discarded discovery addresses. Registrations left by clients are freed
at shutdown, and endpoint close removes its matching name and releases its
owned context. Native Linux builds test
name-server cleanup with AddressSanitizer, LeakSanitizer, and
UndefinedBehaviorSanitizer. On x86-64 they also test repeated PSM3 endpoint
creation and close using its self transport, without a fabric device. System
libfabric installations need their own fixes.

Set `LOOM_LIBFABRIC_STATIC=ON` to link the libfabric archive, including its
private dependencies reported by `pkg-config --static`. This requires an
installed archive; a shared-only libfabric installation is rejected.
To embed static libfabric in a shared Loom library, build libfabric with
`--enable-embedded --with-pic` and install the resulting `src/.libs/libfabric.a`
alongside its pkg-config metadata. Upstream's embedded mode builds the archive
for use by a parent project and does not install it automatically.

The default Nix dependency disables OPX: its GPU support requires vendor
HFI1 kernel headers not present in the pinned dependency set. Re-enabling
OPX requires supplying compatible headers and the corresponding driver.

Installed CMake packages provide `core`, `execution`, and `asio` components.
Request the components used by your target:

```cmake
find_package(loom CONFIG REQUIRED COMPONENTS core)
target_link_libraries(app PRIVATE loom::loom)
```

Use `COMPONENTS execution` with `loom::execution`, or `COMPONENTS asio` with
`loom::asio`. These components require the corresponding frontend to have been
enabled when Loom was built and its dependency to be available to the consumer.
The `core` component requires neither frontend dependency. Without a component
list, `find_package(loom)` also discovers any available installed frontends;
missing optional dependencies do not prevent using the core library.

### Libfabric release audit

Reviewed every stable release after 1.22.0 through 2.7.0, including the
2.0 prerelease notes folded into that release. The previous Nix source
reported 2.5.0a1; 1.22.0 was the initial local review build.

| Release | Changes relevant to loom | Wrapper impact |
| --- | --- | --- |
| [2.0.0](https://github.com/ofiwg/libfabric/releases/tag/v2.0.0) | API simplifications, `fi_fabric2`, sub-MRs and single-use registrations | Existing calls remain usable; the new registration APIs need separate lifetime designs. |
| [2.1.0](https://github.com/ofiwg/libfabric/releases/tag/v2.1.0) | EFA-direct context requirements and provider fixes | Preserve `FI_CONTEXT2` and keep application metadata outside provider storage. |
| [2.2.0](https://github.com/ofiwg/libfabric/releases/tag/v2.2.0) | Interface rescanning and firewall addresses | Add typed `FI_RESCAN` query and `FI_FIREWALL_ADDR` insertion options. |
| [2.3.0](https://github.com/ofiwg/libfabric/releases/tag/v2.3.0) | Mixed IPv4/IPv6 address format | Discovery now preserves `FI_SOCKADDR_IP`. |
| [2.3.1](https://github.com/ofiwg/libfabric/releases/tag/v2.3.1) | Provider corrections | Receive the fixes through the dependency update. |
| [2.4.0](https://github.com/ofiwg/libfabric/releases/tag/v2.4.0) | Upstream Rust bindings and clarified wait-object contracts | Keep loom's higher-level binding; query the wait-object type before retrieving a descriptor. |
| [2.5.0](https://github.com/ofiwg/libfabric/releases/tag/v2.5.0) | Counter limits and ROCr device ordinals | Runtime counter limits are exposed and checked; registration passes the selected device. |
| [2.5.1](https://github.com/ofiwg/libfabric/releases/tag/v2.5.1) | Memory-registration and provider corrections | Receive the fixes through the update; callers still own buffer lifetimes. |
| [2.6.0](https://github.com/ofiwg/libfabric/releases/tag/v2.6.0) | EFA feature discovery and hardware counters | Use runtime provider limits and capabilities for operational decisions. |
| [2.7.0](https://github.com/ofiwg/libfabric/releases/tag/v2.7.0) | XPU contexts, device exports, address lookup and MR descriptors | A new XPU integration needs context ownership, export wrappers and device-side APIs. |

The context, device-registration, address-format, and counter-limit corrections
are implemented. Remaining portable additions include rescan/firewall options.
XPU support is a separate feature project,
particularly relevant to the GPU communication work listed below; adding
only the capability bit would not provide a usable implementation.

Provider-specific fixes and tuning settings generally require no new loom
API. Hardware behavior still needs testing on the corresponding NICs and
accelerators. The authoritative change history is the tagged
[release notes](https://github.com/ofiwg/libfabric/blob/v2.7.0/NEWS.md).

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

### Native discovery handoff

`fabric_info::copy_native(native_info)` owns a copy of one selected libfabric
discovery entry. It preserves native attributes, addresses, authentication
keys, and NIC metadata without repeating discovery. The input's `next` chain
is excluded. Referenced fabric, domain, and context handles remain borrowed;
the copy does not extend their lifetimes or establish a checked endpoint contract.
An optional memory resource allocates Loom's wrapper; libfabric allocates the
native copy. Native duplication failures return `errc::other`, since
`fi_dupinfo` returns no error code. PMR allocation exceptions propagate after
releasing the native copy. The pinned libfabric also fixes failure cleanup
that otherwise closes the source NIC when duplication fails.

### Native addresses

`FI_SOCKADDR_IB` addresses decode to `ib_socket_address`, an alternative in
`loom::address`. It preserves the GID, partition key, flow information,
service ID and mask, and scope ID. Scalar fields use host byte order; GID
bytes use network order. Connection setup and AV insertion encode the full
native socket address, including through `FI_SOCKADDR`. `FI_SOCKADDR_IP`
accepts only IPv4 and IPv6 addresses. Exhaustive address visitors must handle
the new alternative. The existing GID/QPN/LID `ib_address` remains a separate
identifier type and cannot be submitted as a socket address.

Provider-specific formats, including EFA, CXI, UCX, and `FI_ADDR_STR`, decode
to `opaque_address`. Its `format()` and owned `bytes()` preserve the native
representation, including a string address's terminating null byte. Endpoint
and AV lookups negotiate larger buffers and return errors for malformed or
unsupported representations. Connect and AV insertion accept these addresses;
binary batches require equal byte lengths, while string batches use the
native array of string pointers and may have different lengths.

Construct opaque addresses only from native bytes supplied by a compatible
peer. Loom preserves provider-specific fields without interpreting them;
matching address formats alone do not establish peer compatibility. Allocation
exceptions can propagate from `address_from_raw`. Exhaustive visitors must
handle `opaque_address`, and consumers must rebuild for the changed variant.
`get_address_data` still points to the C++ object; use `opaque_address::bytes()`
to access the native representation.

### Tagged message reservations

`msg::tagged_claim` keeps one native context stable from a matched probe
through its receive or discard. Construct it with an enabled endpoint,
then call `probe(tag, ignore, source)`. Route each CQ event matching
`context()` to `observe()` exactly once. For a probe, `observe()` returns
`no_match` or `reserved`; other completion errors remain errors. `completion()`
retains the full event, including length, tag, source, immediate data and diagnostics.

After `reserved`, submit `receive(buffer, descriptor)` or `discard()` and
observe its terminal completion. Rejected submissions retain the reservation
for retry. Duplicate or out-of-order submissions are rejected. These calls
always request a completion and replace the endpoint's default operation flags.
The receive buffer and registration remain borrowed until completion or
successful endpoint close; requesting cancellation does not end that lifetime.

The claim cannot be copied or moved. Its endpoint wrapper must outlive it
and remain in place. Destroying an active claim terminates unless that endpoint
has been successfully closed. There is no implicit discard or blocking wait.
After closing the endpoint, `retire_after_close()` clears local state even when
the provider discarded pending CQ entries. Do not access the claim concurrently.
Serialize endpoint access and progress according to its threading requirements.

### Atomic Operations

loom provides type-safe remote atomic operations with automatic datatype deduction:

Provider-aware atomics also use libfabric's atomic interface. The selected
provider supplies the atomicity guarantees in hardware or software; query
support with `atomic::is_valid`, `is_fetch_valid`, or `is_compare_valid`.
`provider_atomic_context` requires no application staging buffer. Its legacy
staging entry points submit provider atomic operations rather than a separate
RMA read/modify/write sequence, which cannot guarantee atomicity.

For RDM endpoints, use `execute_to`, `fetch_to`, `compare_swap_to`, or
`inject_to` with the peer's address-vector handle. Typed `add_to`,
`fetch_add_to`, and `cas_to` preserve the same destination and independent
operand registrations. The original helpers use an unspecified destination
for connected endpoints. `compare_swap_to` accepts separate comparison,
replacement, and result registrations.

Ordinary atomic calls borrow every operand, result, registration, and
context until completion. Keep typed arguments alive; passing a temporary
does not extend its lifetime. `inject_to` releases its input on return and
generates no success completion. Provider operation/type/count and inject
limits still apply.

`execute_to`, `fetch_to`, and `compare_swap_to` accept `op_flags` before
the context. These overloads replace endpoint defaults unless called with
`<operation_defaults::inherit>`, which preserves the transmit creation
flags. `op_flag::completion` requests a CQ event under selective completion.
`op_flag::inject` releases only constant input operands on return; result
buffers and their registrations remain borrowed until completion.
Atomic senders request CQ completions and inherit transmit defaults. They
retain copies of their operands while borrowing any result registration.

```cpp
// ep is bound to a dedicated cq and an AV containing peer_address.
// remote is the target's exported remote_memory, exchanged with that peer.
std::uint64_t increment = 42;
std::uint64_t previous = 0;
auto input_mr = loom::memory_region::register_memory(
    domain, std::as_writable_bytes(std::span{&increment, 1}),
    loom::mr_access_flags::read).value();
auto result_mr = loom::memory_region::register_memory(
    domain, std::as_writable_bytes(std::span{&previous, 1}),
    loom::mr_access_flags::write).value();

std::size_t max_count{};
if (!loom::atomic::is_fetch_valid(ep, loom::atomic::operation::sum,
                                  loom::atomic::datatype::uint64, &max_count)
    || max_count < 1) {
    // Reject this transport profile before submitting work.
    return;
}
loom::request_context context;
auto submitted = loom::atomic::fetch_add_to(
    ep, increment, &previous, &input_mr, &result_mr,
    peer_address, remote, &context);
if (!submitted) {
    // Nothing was accepted. Preserve the numeric error, including EAGAIN.
    return;
}
for (;;) {
    if (auto event = cq.poll()) {
        // Check event->error before using previous. This dedicated CQ has
        // only this operation; a shared CQ needs dispatch by context.
        break;
    }
}
// The completion retires the borrowed buffers and registrations.
```

### Triggered Operations

Triggered operations allow deferring work until a counter reaches a threshold,
enabling efficient pipelining and dependency chains. Include
`capability::trigger` in the fabric query's capabilities, and use
`trigger::supports_trigger(ep)` to check what was negotiated:

```cpp
#include <loom/loom.hpp>
#include <loom/async/completion_queue.hpp>

auto main() -> int {
    using namespace loom;

    // ... fabric/domain/endpoint setup ...

    // Create counters for tracking completions and triggering work
    auto recv_counter = counter::create(domain, {.threshold = 1}).value();
    auto send_counter = counter::create(domain, {.threshold = 1, .wait_obj = true}).value();

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
    deferred_send.mark_completed();

    std::println("Pipeline complete: received data and sent response");

    return 0;
}
```

### Shared receive contexts

Set `fabric_hints::shared_receive_context = true` when querying endpoints
that will use an SRX. The returned endpoint attributes expose the same
requirement. Create `shared_rx_context` in that domain, bind it to each
endpoint with `ep.bind_srx(srx)`, then enable the endpoints. All bound RDM
endpoints must share one address vector.

The core `recv`, `recvv`, `tagged_recv`, `tagged_recvv`, and `msg` receive
helpers accept either an endpoint or an SRX through the `receive_target`
concept. They preserve source filters, descriptors, contexts, tag masks,
and per-call flags. Message helpers also support explicit inheritance of
the SRX's receive defaults. Provider operation support is checked at
submission; a receive-capable object does not guarantee every operation.

```cpp
recv(srx, buffer, fabric_addrs::unspecified, mr.descriptor(), &context).value();
// Poll the receiving endpoints' completion queues until this receive retires.
```

SRX receives complete on the CQ belonging to whichever bound endpoint
receives the message. Bind CQs and counters to those endpoints. The former
`shared_rx_context::bind_cq/bind_counter` methods have been removed because
libfabric does not associate those resources with an SRX. Endpoint-oriented
Asio and sender adapters do not yet route shared receives across these queues;
use the core submission and polling interfaces.

Keep receive buffers, registrations, and provider context storage alive until
their operations retire. Multi-receive storage remains outstanding through
the terminal buffer-release event. `srx.cancel(context)` requests cancellation;
its return alone does not release the storage. Close bound endpoints before
`srx.close()` and close the SRX before its domain. A failed explicit close
retains the handle for retry; successful close releases remaining receives.

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

`remote.subregion(offset, length)` and `remote.offset_by(offset)` return
`result<remote_memory>`. Check the result before using the descriptor: ranges
outside the parent or whose start or exclusive end address would overflow
return `errc::invalid_argument`. An empty range at the parent's end is valid
when its address is representable. Both helpers preserve the full remote key
and work with virtual-address and offset-based registrations.

Registered message helpers and vectored RMA keep up to 16 native entries
per descriptor or remote-IOV array inline. Larger arrays use heap storage
and preserve every entry; the endpoint's advertised IOV limits still apply.
This avoids temporary-array allocations for small submissions. Provider
work and asynchronous operation state can allocate independently.

Scalar exports are checked: `memory_region::key()`, `remote_memory::from_mr()`,
provider-specific exports and MR-cache exports return `result`. Empty regions
report `invalid_argument`, unavailable native keys report `again`, and raw-key
domains report `not_supported` through these scalar interfaces. Registered Asio
buffer key accessors follow the same contract.

For domains requiring `mr_mode_flags::endpoint`, construct the MR cache with
both its domain and endpoint:

```cpp
auto cache = make_mr_cache<provider::verbs_tag>(domain, ep);
auto registered = cache.lookup(buffer, mr_access_flags::send).value();
```

Each cache belongs to that endpoint. New host, HMEM, and DMA-BUF registrations
are bound and enabled before a handle is returned; failed preparation leaves
no cache entry. Ordinary domains keep their registration behavior. Omitting
the endpoint when required, or supplying an endpoint from another domain,
returns `invalid_argument` before registration. `ep.belongs_to(domain)` checks
the native domain identity, including for scalable endpoint contexts;
`domain.get_memory_region_mode()` exposes the negotiated registration modes.

The domain and endpoint wrappers must keep their native handles and outlive
the cache. The cache must outlive every returned handle. Retire all operations,
release the handles, then clear or destroy the cache before closing the
endpoint. `clear()` evicts only unreferenced registrations, so calling it while
handles remain alive does not permit endpoint teardown. Cache locking does
not serialize external endpoint operations against registration and binding.

HMEM cache hits require the same memory interface and device, a containing
range, and sufficient permissions. DMA-BUF hits also require the same borrowed
file descriptor and allocation base, with a containing file-offset range.
Unmapped buffers retain their file offsets even though their local address is
null. Device registrations keep their exact ranges and contribute to the same
hit, miss, registration, and byte statistics as host registrations.
Use separate caches for device contexts whose address spaces can alias.

Reusable ranges are indexed in an AVL tree with subtree end bounds. Lookups
skip earlier registrations that cannot contain the requested range, including
on misses between disjoint allocations. Unmapped DMA-BUF ranges are indexed by
descriptor and full-width file offset; mapped registrations use virtual addresses.
Invalidation still operates on local addresses. Overlapping candidates require
permission and memory-identity checks. Invalidation removes ranges from this
index immediately while existing handles retain their registrations.
Unbounded caches share read access for hits and release of reusable handles;
misses recheck under exclusive access before registering. Registration,
invalidation and eviction remain serialized. Hit counters remain exact under
concurrent reuse.

Retention is unbounded by default. To cap it, pass `mr_cache_limits` to the
constructor or factory (after the endpoint, when one is supplied):

```cpp
auto cache = make_mr_cache<provider::verbs_tag>(
    domain, mr_cache_limits{.max_entries = 256,
                            .max_registered_bytes = 64 * 1024 * 1024});
```

Both bounds apply across host, HMEM and DMA-BUF registrations. Host bytes include
page rounding; device bytes use the exact registered length. The cache evicts
the oldest idle entries before a new registration, even if that registration
later fails. Live handles can temporarily exceed either bound. Their releases
restore the bounds as entries become idle; zero limits disable idle retention.
Bounded caches serialize transitions between idle and active use to maintain
eviction order, while lookups of entries with live handles still share access.
Keep a handle alive through every native operation and any remote use of its
exported key. The limits are fixed at construction and available through
`cache.limits()`; statistics include registrations retained by live handles.

Keep allocations and DMA-BUF descriptors valid while their registrations are
cached. Before freeing or remapping memory, or closing/reusing a descriptor,
invalidate the registered range and retire all handles that still use it.
Invalidation prevents subsequent reuse but preserves existing handles until
release. The cache does not monitor allocator or descriptor changes; use
`clear()` or `evict_unreferenced()` to release idle registrations explicitly.

For raw-key registrations, use `mr.export_raw()` and exchange the returned
`raw_remote_memory` fields through the consumer's wire protocol. Preserve
`base_addr`, `addr`, `length` and every key byte. The native mapping base and
the RMA address are distinct, including for offset-based registrations.
On the initiating peer:

```cpp
auto mapping = mapped_remote_memory::map(domain, std::move(received_metadata)).value();
auto remote = mapping.remote().value();
// Submit RMA or atomics with remote, then retire every accepted operation.
mapping.close().value();
```

The mapping retains its raw bytes and unmaps its native key on destruction.
Call `close()` explicitly to observe failures; a failed close retains ownership
for retry. The initiating domain, backing memory resources and exported target
registration must outlive their use. Move construction transfers ownership;
move assignment exchanges mappings without implicitly closing either one.
Use mapped descriptors only with endpoints in the initiating domain; copying a
descriptor does not retain its mapping.

To count remote writes and modifying atomics to a specific registration,
request `memory_registration{.access = mr_access_flags::remote_write,
.rma_events = true}` and call `mr.bind(counter)`. Accessing endpoints must
have `capability::rma_event`. When the domain requires
`mr_mode_flags::rma_event`, call `mr.enable()` after all bindings succeed.
If it also requires `mr_mode_flags::endpoint`, bind the MR to its endpoint
before enabling it. Bound counters and endpoints must outlive the MR, and
further bindings are forbidden after enablement. Ordinary registrations do
not need the event flag merely because their domain supports that mode.

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

    // Deferred work also supports custom allocators.
    auto completed = counter::create(dom, {.wait_obj = true}, &arena).value();
    auto deferred = trigger::deferred_work::create_counter_add(
        dom,
        trigger::threshold_condition{&cntr, 1},
        completed, 1,
        &arena  // Custom allocator for internal structures
    ).value();

    cntr.add(1).value();
    completed.wait().value();
    deferred.mark_completed();
    return 0;
}
```

Deferred work and its referenced resources must outlive native execution.
After observing completion or a successful domain flush, call
`mark_completed()`. To abandon pending work, call `cancel()` and handle
failure while retaining the work. Destruction attempts cancellation if
needed and terminates if it fails; freeing a descriptor still referenced
by the provider would otherwise cause a use-after-free.

### Compile-Time Provider Traits

loom provides compile-time provider characteristics for zero-overhead abstraction
and static dispatch based on fabric capabilities:

```cpp
#include <loom/loom.hpp>

using namespace loom;

// Provider traits give compile-time access to provider capabilities
static_assert(provider_traits<provider::verbs_tag>::supports_native_atomics);
static_assert(provider_traits<provider::efa_tag>::uses_staged_atomics);
static_assert(provider_traits<provider::tcp_tag>::supports_auto_progress);

// Concepts enable compile-time capability checking
static_assert(native_atomic_provider<provider::verbs_tag>);
static_assert(staged_atomic_provider<provider::efa_tag>);
static_assert(inject_capable_provider<provider::shm_tag>);
static_assert(auto_progress_provider<provider::tcp_tag>);
static_assert(manual_progress_provider<provider::slingshot_tag>);

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
static_assert(requires_manual_progress<provider::slingshot_tag>());
static_assert(requires_manual_progress<provider::shm_tag>());
static_assert(!requires_manual_progress<provider::tcp_tag>());
```

Progress traits describe the libfabric 2.7 base-provider defaults; EFA uses
its ordinary RDM profile. CXI requires manual control and data progress,
while SHM and UCX require manual data progress. Verbs and TCP advertise
automatic progress. The selected domain's returned attributes govern
actual polling: provider layering, hints, and configuration can change
the applicable requirements. A provider tag alone does not establish a
negotiated endpoint contract.

The base TCP and libfabric UCX providers do not expose atomics. A layered
provider may have different capabilities and needs its own established
contract. Atomic-capable providers still require validation of the
operation, datatype, and element count through libfabric's atomic queries.

Checked endpoints use a capability profile, with no provider-name matching:

```cpp
using header_profile = message_endpoint_profile<rdm_endpoint_tag, 32>;
using header_endpoint = contract_endpoint<header_profile>;
auto checked = header_endpoint::create(domain, info);
if (!checked) return std::unexpected(checked.error());
// Bind completion queues and other resources, then check enable().
// inject(span<const byte, 32>) fits this type's established bound.
```

The factory checks endpoint type, required capabilities, supported mode
obligations, and the chosen message inject bound against the selected
configuration. Profiles can also require exact threading and control/data
progress policies, checked against the opened domain. The default profile
accepts no mode obligations. A custom profile declaring `supported_modes`
requires the caller to satisfy those obligations for every operation.
Profiles expose `ep_type`, `required_caps`, and `optional_caps` for use with
`fabric_query`; additional profile requirements are checked at construction.

The inject bound is checked against `FI_OPT_INJECT_MSG_SIZE`, falling back
to creation attributes when that option is unsupported. Oversized fixed
spans are rejected at compile time; dynamic spans are checked before
submission. Native errors such as `FI_EAGAIN` propagate. Binding resources
and enabling revalidate the bound; a failure prevents further submissions.
A zero bound disables injection entirely.

`provider_endpoint<Provider, Bound, EndpointTag>` and the named aliases
remain convenience spellings for these profiles: a provider tag supplies
only the default bound, and the default endpoint type is RDM. They expose
no provider-identity or native-atomic claims. Use `contract_endpoint<Profile>`
to state requirements directly; provider traits remain discovery defaults.
Construction from an arbitrary endpoint and `make_provider_endpoint` are
removed. `underlying()` returns a const view;
`std::move(checked_endpoint).release()` returns to the generic API.
Native-handle mutation must preserve the checked configuration. This
contract does not establish MR policy, ordering, or support for particular
atomic operations, datatypes, and counts.

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

The `rdma_read_ops` and `rdma_write_ops` profiles require both initiating and
being the target of the corresponding RMA operation. Custom capability
descriptors can request only the directions an application needs.

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

    // Filter by a negotiated policy
    auto filtered = query_providers<rdma_write_ops>([](const fabric_info& info) {
        auto attr = info.get_domain_attr();
        return attr && attr->data_progress == progress_mode::auto_progress;
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

Asio submissions register their contexts with the endpoint's bound TX/RX queues.
Bind those queues before initiating operations and keep them alive through native
completion. A manual progress loop can route events with `cq.dispatch(event)`;
the old raw-pointer `dispatch_asio_completion` helper has been removed.

Before destroying an associated executor, drain pending operations or explicitly
close the Asio endpoint and check the result. `ep.close(ec)` calls native close;
success releases the native operation storage and posts pending handlers with
`errc::canceled`. Those handlers can run normally or be destroyed with their
executor. A failed close preserves the endpoint and pending work for progress,
cancellation, and retry. The overload without an error-code argument throws
`std::system_error` on failure.

Serialize explicit close with submissions and CQ dispatch: stop service polling
and join external progress threads before closing. Blocking operations must
finish first. Endpoint destruction and move assignment request cancellation and
retain accepted work until completion; they do not replace this shutdown step.
Keep buffers, registrations, queues, and executors alive until native work has
completed or explicit close succeeds. Low-level completion-token initiation on
a borrowed `loom::endpoint` requires draining its completions before shutdown.

Asio retains IOVs and descriptors for up to four buffers inside the operation
state. Larger sequences reserve each retained array once through the handler's
associated allocator. The inline capacity does not limit the buffer count;
provider IOV limits still apply. Blocking operations use the same storage.

Blocking `ep.send(buffers)` and `ep.receive(buffers)` drive the bound CQ until
their own native completion, without running the executor. Both submit the whole
buffer sequence and preserve registered-buffer descriptors. Receives return the
completed message length, including partial bytes reported with an error.
Unrelated completions remain available through normal CQ reads; these calls do
not dispatch unrelated handlers.

Blocking submissions preserve fence and delivery requirements while removing the
inherited `FI_MORE` batching hint. After submission, CQ read failures and metadata
allocation exceptions request cancellation and retain native storage until a
terminal completion. Calls have no timeout and can wait indefinitely if a
provider never retires accepted work.

The fabric service dispatches registered operations from any frontend. It retains
the first event without a registered owner, including context-free remote events
and errors, and pauses reads from that queue until
`service.take_unhandled(cq)` transfers the event to the caller. Handle that event
before continuing; other registered queues keep progressing. `poll_once()` counts
only dispatched operations. `deregister_cq(cq)` returns any retained event and
waits for concurrent polling to finish, so callers must inspect its return value
before destroying the queue. Keep registered queue wrappers at stable addresses.
Service calls and inline completion callbacks are serialized; callbacks may
reenter the service but must not wait for another thread to call the same service.
Polling does not allocate a queue snapshot. Registrations added by a callback
join the next poll, including an explicitly nested poll. Removed queues become
inactive immediately; their internal entries are reclaimed when the outermost
poll returns or unwinds after an exception.
With `use_fd_polling=true`, the service uses a POSIX readiness wait when the CQ
exposes `FI_WAIT_FD` and its parent fabric supports native wait preparation.
Create the CQ with `.wait_obj = true` to request wait support. The service owns
a duplicate descriptor and calls `fi_trywait` before each arm. Pending progress
is drained in bounded passes without waiting for a signal. Unsupported waits,
including imported queues without a recorded parent, use the polling timer;
`use_fd_polling=false` selects that timer explicitly. Its period is the shortest
interval among queues needing timer progress. Idle queues with armed descriptors
do not incur periodic reads. A paused queue resumes when its event is consumed.
While FD polling is active, read registered queues through the service, including
`poll_once()`, so every read is followed by fresh wait preparation. Keep each CQ's
native parent fabric alive until the queue is destroyed.

Progress remains scheduled when a completion callback throws. After catching
the exception from `io_context::run()`, reenter the event loop to continue, or call
`service.stop()` to stop polling.

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

Use `mutable_registered_buffer` and `const_registered_buffer` for messages
backed by a memory region. The buffers and registrations remain borrowed until
the completion handler runs.

```cpp
#include <loom/asio.hpp>

// ... fabric/domain setup and a connected Asio endpoint named ep ...
std::array<std::byte, 4096> storage{};
auto mr = loom::memory_region::register_memory(
    domain, std::span{storage},
    loom::mr_access_flags::send | loom::mr_access_flags::recv
).value();

loom::asio::mutable_registered_buffer buf{
    ::asio::buffer(storage), &mr, loom::asio::registered_buffer_id{0}};
ep.async_receive(loom::asio::buffer(buf, 1024),
    [](std::error_code error, std::size_t received) {
        // A successful receive reports the message length, not buffer capacity.
    });
```

Tagged and untagged messages preserve each buffer's MR descriptor, including
arrays of independently registered buffers and offset or shortened views. The
same views can wrap an MR created with `memory_registration` for HMEM; device
allocation lifetime and visibility synchronization remain the caller's
responsibility. RMA overloads that take an explicit MR use that registration for
every buffer in the sequence and check each local range before submission.
An empty or unmapped MR, a buffer outside its registered span, or a wrapping
range returns `errc::invalid_argument`. The native descriptor may be null for a
valid registration. `memory_region::contains` exposes the same range check.

MR-taking RMA and atomic operations, their senders, and registered Asio buffers
also check `memory_region::usable_with(endpoint)`: the MR must belong to the
endpoint's native domain, match any endpoint binding, and have completed any
required enablement. Unknown origin metadata in adopted implementations is
rejected. Failed binding or enablement does not change the previously accepted
state. These checks preserve wrapper moves but do not extend native resource
lifetimes. Descriptor-only interfaces still require the caller to establish
registration compatibility.

#### Building with Asio

Enable Asio integration via CMake:

```bash
cmake -B build -DLOOM_USE_ASIO=ON
```

Installed consumers link `loom::asio`. They must provide Asio through a
CMake package defining `asio::asio` or through pkg-config, including when
loom itself was built with `LOOM_FETCHCONTENT_ASIO=ON`.

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
- `std::execution` (C++26 standard, when supplied by the standard library)

Senders use standard member customizations. The `loom::execution` namespace
forwards concepts, completion operations, and algorithms to the selected backend.
The same concept and runtime completion tests run against stdexec and Beman.
The standard-library backend requires `__cpp_lib_senders >= 202406L`; the
installed libstdc++ 16 currently lacks it, so that backend is not runtime-tested.
All translation units exchanging loom senders must select the same backend.

Fabric senders require application-driven progress. Bind the endpoint to
the CQ passed to the sender and call `loom::execution::poll_completions(cq)`
from a progress loop. Its result contains `completed`, the number dispatched,
and an optional `unhandled` event. Handle that event before continuing: it may
belong to another frontend or carry a remote notification or queue failure.
Foreign contexts are compared with registered addresses and never dereferenced.

Starting an operation registers its completion and submits it. Any CQ reader
can call `cq.dispatch(event)` to deliver an event to its registered operation;
`false` leaves the intact event with that reader. Route each event once, before
its context storage can be reused. Native poll, batch and wait methods do not
invoke receivers implicitly. Dispatch unlinks the registration before invoking
the receiver, allowing it to destroy the completed state or submit more work.
The completion registry uses an intrusive splay tree: registration, lookup,
and reset have amortized logarithmic cost, allocate no storage, and compare
context pointer values without dereferencing them. A single operation can
still traverse a linear number of entries; queue locking serializes tree changes.
Keep operation states, buffers and their queue alive until delivery. A queue
error without an operation context does not authorize freeing pending states.
When using `sync_wait`, progress must run on another thread, with a provider
threading mode that permits concurrent submission and polling.

```cpp
#include <loom/execution.hpp>

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
                  | loom::execution::then([] { std::println("sent!"); });

    // Parallel operations with when_all
    std::array<std::byte, 4096> buf1{}, buf2{};
    auto both = loom::execution::when_all(
        msg_ops.send(buf1, &cq),
        msg_ops.recv(buf2, &cq)
    );

    // Chain operations with let_value
    auto send_then_recv = msg_ops.send(buf1, &cq)
                        | loom::execution::let_value([&] {
                              return msg_ops.recv(buf2, &cq);
                          });

    // Error handling
    auto with_error = msg_ops.send(buffer, &cq)
                    | loom::execution::upon_error([](std::error_code ec) {
                          std::println(stderr, "Error: {}", ec.message());
                          return -1;
                      });

    return 0;
}
```

Messaging senders also carry explicit peers and registration descriptors:

```cpp
auto messages = loom::async(ep);
auto send = messages.send_to(buffer, local_mr.descriptor(), peer, &cq);
auto recv = messages.recv_from(buffer, local_mr.descriptor(), source, &cq);
auto tagged_send = messages.tagged_send_to(buffer, local_mr.descriptor(), peer, 42, &cq);
auto tagged_recv = messages.tagged_recv_from(buffer, local_mr.descriptor(), source, 42, 0, &cq);
```

Each sender copies the descriptor and address; the endpoint, CQ, buffer, and
registration remain borrowed through completion. Use `fabric_addrs::unspecified`
for a connected destination or an unrestricted receive. A specific receive
source requires negotiated directed-receive support for that message type.
An empty descriptor is valid only when the selected memory-registration mode
allows it. Allocation failures during message preparation report
`errc::no_memory` through the receiver.

Asio provides `async_send_to(buffers, peer, token)` and
`async_receive_from(buffers, source, token)`, plus `async_tagged_send_to` and
`async_tagged_receive_from` with the peer before the tag arguments. These
preserve the per-buffer descriptors in registered buffer sequences. The
`initiate_fabric_*` messaging helpers accept an optional peer after their
existing arguments. The same registration lifetime and source-filter
requirements apply.

#### RMA Operations with Senders

Use `read_from` and `write_to` for registered buffers and explicit RDM
peers. The sender copies the local descriptor and remote metadata; keep
the endpoint, CQ, registration, and buffer alive until completion. The
transfer length is the local span's size and must fit `remote.length`.
Progress still comes from `loom::execution::poll_completions(cq)`.

```cpp
auto operations = loom::rma_async(ep);
auto read = operations.read_from(buffer, local_mr, peer, remote, &cq);
auto write = operations.write_to(buffer, local_mr, peer, remote, &cq);
```

Asio provides the same registered peer path through
`ep.async_read_from(buffers, local_mr, peer, remote, token)` and
`ep.async_write_to(buffers, local_mr, peer, remote, token)`. All buffers in
that sequence use `local_mr`; callers must register the whole sequence's
storage. Buffers and registration remain borrowed until completion.
Core `rma::readv_to` and `rma::writev_to` also accept a separate descriptor
for each IOV and preserve multiple remote segments. Vectored and deferred RMA
require equal local and remote byte totals. Unequal totals, overflowing sums,
and remote address ranges that wrap return `errc::invalid_argument` before
submission; segment boundaries may differ, and zero-byte transfers remain valid.

The Asio and sender messaging and RMA paths explicitly request a CQ
completion, including when the endpoint uses selective completion. They
extend the endpoint's creation flags for the operation's direction,
preserving applicable fence and delivery requirements while clearing inherited
`FI_MORE`. Each operation can complete without a later submission to end a batch.
Configure those defaults before creating the endpoint or scalable context;
subsequent raw `FI_SETOPSFLAG` changes are not tracked by Loom.

RMA reads discard write-only endpoint defaults, including injection, remote CQ
data, and write completion levels. Explicit injection, transmit-completion, or
delivery-completion flags on a read return `errc::invalid_argument` before
submission. Rust reads likewise require `RmaCompletion::Local`. Writes retain
their requested completion semantics.

Receive frontends require a CQ layout that reports message lengths (`msg`,
`data`, or the default `tagged`). They reject context-only queues before
submission rather than treating an unavailable byte count as a zero-byte message.

Tagged and untagged receive frontends complete once per operation. They reject
inherited `FI_MULTI_RECV` with `errc::not_supported` before posting any work.
Use the core message APIs for multi-receive and retain the buffer and context
until the provider emits the terminal `FI_MULTI_RECV` completion flag.

Core `msg::sendmsg`, `msg::recvmsg`, `msg::tagged_sendmsg`, and
`msg::tagged_recvmsg` also accept `<operation_defaults::inherit>` to extend
the corresponding transmit or receive defaults.
`<operation_defaults::inherit_without_more>` extends defaults without their
`FI_MORE` hint, which is useful when waiting before submitting another operation.
Their ordinary overloads use explicit replacement flags. Their message objects
carry peers, IOVs, registrations, contexts, and applicable tag and immediate-data
fields.

Core `read_to`, `write_to`, `readv_to`, and `writev_to` accept an `op_flags`
argument before the context. These overloads replace the defaults unless
called with `<operation_defaults::inherit>`. For example:

```cpp
auto submitted = loom::rma::write_to<loom::operation_defaults::inherit>(
    ep, buffer, local_mr, peer, remote, loom::op_flag::completion, context);
```

The original sender `read` and `write` overloads use an unspecified peer
and no local descriptor, for configurations where those are valid:

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
         | loom::execution::then([&](std::size_t bytes) {
               // Modify buffer
               process(buffer);
               return buffer;
           })
         | loom::execution::let_value([&](auto& buf) {
               return rma_ops.write(buf, addr, key, &cq);
           });

// Atomic operations
auto atomic_op = rma_ops.fetch_add<std::uint64_t>(addr, 1, key, result_mr, &cq)
               | loom::execution::then([](std::uint64_t old_val) {
                     return old_val + 1;
                 });
```

For registered atomic operands or explicit RDM peers, use the borrowed forms:

```cpp
auto operations = loom::rma_async(ep);
auto add = operations.fetch_add_to(operand, &operand_mr, result, &result_mr,
                                   peer, remote, &cq);
auto cas = operations.compare_swap_to(expected, &expected_mr,
                                      replacement, &replacement_mr,
                                      result, &result_mr, peer, remote, &cq);
```

These senders retain the addresses of the supplied objects and return the
original remote value through `set_value`. Inputs may be const; inputs and
result must have the same supported atomic value type. Temporaries cannot
bind to these overloads. Each registration must cover its associated object,
including when several objects occupy different offsets within one region.
Invalid registrations and remote ranges shorter than one value are rejected
before submission. A null registration is permitted only when the negotiated
MR mode allows it.

Core atomic operations check the full byte extent of every supplied operand
and result registration. Atomic reads ignore the source operand as specified
by libfabric; their result registration is still checked.

Keep the objects, registrations, endpoint, and CQ alive through completion.
Do not modify inputs or access the result while the operation is outstanding.
The result must be host-accessible because the sender reads it after completion.
The older `fetch_add` and `compare_swap` forms copy their inputs into operation
storage and require a provider configuration that accepts unregistered inputs.
Datatype and operation support remain subject to the provider's atomic
validity queries.

#### Scheduler

`loom::scheduler` retains a completion queue, but its schedule sender completes
inline on the thread calling `start`. It does not create a progress thread or
move work to the thread polling the CQ.

```cpp
#include <loom/execution.hpp>

// Create a scheduler from a completion queue
auto cq_ptr = std::make_shared<loom::completion_queue>(std::move(cq));
loom::scheduler sched{cq_ptr};

// Run work inline when started
auto work = loom::execution::schedule(sched)
          | loom::execution::then([] { return 42; });

// starts_on uses the same inline scheduling behavior
auto on_cq = loom::execution::starts_on(sched, some_sender);
```

#### Building with P2300

Select one backend per build and link `loom::execution` in your CMake target:

```bash
cmake -S . -B build-stdexec -DLOOM_USE_STDEXEC=ON
cmake -S . -B build-beman -DLOOM_USE_BEMAN_EXECUTION=ON
cmake -S . -B build-std -DLOOM_USE_STD_EXECUTION=ON
```

Install the selected dependency first, or enable `LOOM_FETCHCONTENT_STDEXEC`
or `LOOM_FETCHCONTENT_BEMAN_EXECUTION` to use the pinned upstream source.
An unavailable backend or conflicting choices produce a configuration error.
Standard-library execution needs no external package.

With Nix:

```bash
nix build .#loom-stdexec .#loom-beman
nix develop .#stdexec
nix develop .#beman
```

#### Backend Detection

The CMake target propagates its backend selection to consumers. Without that
target, define exactly one of `LOOM_EXECUTION_USE_STD`,
`LOOM_EXECUTION_USE_STDEXEC`, or `LOOM_EXECUTION_USE_BEMAN` before including
loom headers. With no explicit choice, detection prefers the standard library,
then stdexec, then Beman. Explicit choices never fall back to another backend.

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
