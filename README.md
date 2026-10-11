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
Timer progress remains scheduled when a completion callback throws. After catching
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
every buffer in the sequence.

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
for each IOV and preserve multiple remote segments.

The Asio and sender messaging and RMA paths explicitly request a CQ
completion, including when the endpoint uses selective completion. They
extend the endpoint's creation flags for the operation's direction,
preserving configured fence and delivery requirements while clearing inherited
`FI_MORE`. Each operation can complete without a later submission to end a batch.
Configure those defaults before creating the endpoint or scalable context;
subsequent raw `FI_SETOPSFLAG` changes are not tracked by Loom.

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
