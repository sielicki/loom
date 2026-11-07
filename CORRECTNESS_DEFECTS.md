# Loom correctness defects

Assessment date: 2026-10-10.

This report records a review of Loom against the adjacent libfabric checkout's API contracts and provider implementations. The final review check used Loom commit `19c58a7` and libfabric commit `a79c42e5d`. Subsequent changes may resolve individual findings; this document preserves that review snapshot.

A [comparison with CONSUMER_READINESS.md](#comparison-with-consumer_readinessmd) below records overlap, additional defects, and verified fixes at Loom commit `c94da7b`, plus a later source check of `6a54890`. Original findings 3 and 5 received fixes after the initial review; their validation status is distinguished below.

The review found eight correctness defects and two incomplete operation paths. P1 findings block fundamental operations in affected configurations; P2 findings affect specific features, configurations, or reported guarantees. The EQ error-propagation bug fixed during the review was excluded from these findings.

## Correctness defects

### 1. [P1] Memory registrations collide on key zero

[src/memory.cpp](src/memory.cpp), in `memory_region::register_memory`, always requests key `0`; HMEM and DMA-BUF registration do likewise. When `FI_MR_PROV_KEY` is absent, Loom must supply distinct keys for distinct live registrations.

**Reproduced with TCP and sockets:** the first registration succeeds; the second fails with “Required key not available.” Both providers returned `mr_mode=0` in the reproduction.

Expose application-selected keys or allocate unique keys within the appropriate registration scope, while respecting the negotiated key requirements. Apply the same behavior to all registration entry points.

Upstream references: [memory registration contract](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_mr.3.md), [MR key map](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/prov/util/src/util_mr_map.c).

### 2. [P1] Remote-memory helpers generate incorrect addresses for offset-based registration

[include/loom/core/memory.hpp](include/loom/core/memory.hpp), in `remote_memory::from_mr`, always exports the local virtual address. Without `FI_MR_VIRT_ADDR`, libfabric requires an offset starting at zero. The provider-specific helper in [include/loom/core/provider_aware.hpp](include/loom/core/provider_aware.hpp) repeats this mistake; cached MR handles also use the generic helper.

**Loopback reproduction:** an offset-based write succeeds; the helper-generated address produces “Permission denied” and leaves the target unchanged.

Retain the registration's addressing requirements and use them when constructing remote-memory metadata. A local buffer pointer alone is insufficient to determine the remote address.

Upstream reference: [default registration and `FI_MR_VIRT_ADDR`](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_mr.3.md).

### 3. [P1] Incoming connection requests lose the information required to accept or reject them

[src/event_queue.cpp](src/event_queue.cpp), in `event_queue::poll` and `event_queue::wait`, discards `fi_eq_cm_entry::info`. Libfabric requires that `fi_info` when creating the accepting endpoint, and the application owns its release. Loom therefore loses the connection request information and leaks it.

The mock reproduction delivered `FI_CONNREQ` with a populated native `info` pointer, but the returned Loom event contained no connection information.

Preserve the connection information with explicit ownership so callers can create an accepting endpoint or reject the request, and ensure it is eventually freed.

Upstream reference: [connection notification and `FI_CONNREQ` ownership](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_eq.3.md).

### 4. [P1] Collective group creation supplies incomplete inputs and returns the wrong address

[src/collective.cpp](src/collective.cpp), in `create_group_impl`, passes a null AV set to `fi_join_collective`. CXI explicitly rejects this, while the software collective provider dereferences the set.

The implementation also stores the input collective address instead of obtaining the joined group's address through `fi_mc_addr()` after completion. A mock returning group address `999` made Loom continue reporting input address `123`.

Provide the required membership set and its lifetime, preserve asynchronous join completion, and expose the resulting multicast group's address after the join completes.

Upstream references: [collective join contract](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_collective.3.md), [CXI collective implementation](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/prov/cxi/src/cxip_coll.c), [software collective implementation](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/prov/coll/src/coll_coll.c).

### 5. [P2] EQ reads cannot correctly deliver asynchronous control events

[src/event_queue.cpp](src/event_queue.cpp) allocates only `fi_eq_cm_entry` for both polling and waiting. Control completions use the larger `fi_eq_entry`: **16 versus 24 bytes on the reproduction platform**. Providers may reject or truncate these reads, and Loom discards their context and data regardless.

This affects `FI_JOIN_COMPLETE`, `FI_MR_COMPLETE`, and `FI_AV_COMPLETE`. The mock control-event reproduction rejected the supplied buffer as too small. The sockets provider likewise rejects undersized event reads; the utility EQ implementation truncates them.

Read into sufficiently sized storage and decode each event according to its native layout, retaining context and event-specific data.

Upstream references: [asynchronous control event layout](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_eq.3.md), [sockets EQ](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/prov/sockets/src/sock_eq.c), [utility EQ](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/prov/util/src/util_eq.c).

### 6. [P2] Multi-receive submissions lose the location of each received message

Loom exposes `FI_MULTI_RECV` through [include/loom/core/messaging.hpp](include/loom/core/messaging.hpp), but `completion_event` in [include/loom/async/completion_queue.hpp](include/loom/async/completion_queue.hpp) has no buffer pointer, and [CQ conversion](src/completion_queue.cpp) discards `entry.buf`.

Libfabric supplies that pointer to locate messages within a multi-receive buffer. Provider-specific alignment prevents reliably reconstructing locations from lengths.

Expose the receive buffer pointer and preserve it through polling, waiting, and batch completion conversion.

Upstream references: [`buf` completion field](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_cq.3.md), [`FI_MULTI_RECV` placement semantics](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_msg.3.md).

### 7. [P2] DMA-BUF offsets are copied into a reserved field

[src/memory.cpp](src/memory.cpp), in `memory_region::register_dmabuf`, assigns the buffer offset to `fi_mr_attr.offset`, which must be zero. The offset belongs in `fi_mr_dmabuf.offset`.

The probe confirmed that requesting offset `4096` populates both fields. Keep the general registration offset zero while preserving the DMA-BUF offset.

Upstream reference: [memory registration offset contract](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_mr.3.md).

### 8. [P2] Ordering conversion advertises guarantees the provider did not return

[include/loom/detail/conversions.hpp](include/loom/detail/conversions.hpp), in `from_fi_order`, treats `FI_ORDER_STRICT` as an individual bit, although it is a composite mask. A provider reporting only `FI_ORDER_WAW` consequently appears strictly ordered.

**Reproduced:** native value `16` round-trips to `511`. Test the entire mask and preserve individual ordering guarantees instead of promoting partial support to strict ordering.

Upstream reference: [ordering constants](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/include/rdma/fabric.h).

## Incomplete operation paths

### 9. [P2] Atomic operations cannot express required destinations and registrations

[include/loom/core/atomic.hpp](include/loom/core/atomic.hpp) hardcodes `FI_ADDR_UNSPEC`, leaving connectionless endpoints without a peer-selection path. Additionally, `compare_swap` always passes a null replacement-operand descriptor, preventing compliance when that buffer requires registration.

Expose an explicit destination and independent operand, comparison, and result registrations where required.

Upstream references: [atomic operation parameters and connectionless destinations](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_atomic.3.md), [`FI_MR_LOCAL` requirements](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_mr.3.md).

### 10. [P2] Async RMA frontends cannot express registered, explicit-peer RDM operations

[Execution operations](include/loom/execution/operation_state.hpp) use [endpoint methods](src/endpoint.cpp) that supply null descriptors and `FI_ADDR_UNSPEC`. [Asio RMA](include/loom/asio/basic_endpoint.hpp) accepts registration but provides no peer argument; its vectored submission path also uses `FI_ADDR_UNSPEC`.

These frontends cannot represent operations already possible through core `rma::read_to` and `rma::write_to`. Carry local registration descriptors and peer addresses through the async operation state and submission interface.

Upstream references: [RMA operations](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_rma.3.md), [local registration requirements](https://github.com/ofiwg/libfabric/blob/a79c42e5de63eadae8e3dfc7e172af639a6161b9/man/fi_mr.3.md).

## Validation and scope

The reproduction probe was compiled and run against both Loom's compiled sources and its header-only implementation, using installed libfabric 2.7.0. Their observed results agreed. A separate sockets RDM loopback test verified the remote-address defect end to end. Hardware-specific conclusions are based on upstream contracts and provider source, not NIC or accelerator testing.

Representative results after the concurrent EQ error-propagation fix:

```text
connreq: delivered=1, info_size=0, info_len=0, context=(nil)
EQ -FI_EIO: delivered=1, has_error=1, error_reads=0
AV_COMPLETE: supplied_buffer=16 required=24, has_error=1, context=(nil), data=0
collective: set_is_null=1, input_address=123, actual_mc_address=999, loom_group_address=123
DMABUF: reserved_attr_offset=4096, dmabuf_offset=4096
ordering: WAW_reports_STRICT=1 roundtrip=511 expected=16
tcp: mr_mode=0, first=1 second=0 second_error=Required key not available
sockets: mr_mode=0, first=1 second=0 second_error=Required key not available
offset: completion_error=Success, target_byte=42
from_mr: completion_error=Permission denied, target_byte=0
```

Temporary reproduction artifacts from the review are listed below. They are outside the repository and may not survive temporary-directory cleanup:

- `/tmp/loom-libfabric-audit/probe.cpp`
- `/tmp/loom-libfabric-audit/rma_loopback.cpp`
- `/tmp/loom-libfabric-audit/header-results.txt`
- `/tmp/loom-libfabric-audit/compiled-results.txt`
- `/tmp/loom-libfabric-audit/rma-results.txt`

Source-backed findings without dedicated runtime reproductions include multi-receive completion information and the incomplete atomic and async RMA operation paths. The review did not modify implementation files or claim exhaustive provider coverage. Fixes to duplicated out-of-line implementations must cover both `src/` and the corresponding `include/loom/detail/impl/` headers.

## Comparison with CONSUMER_READINESS.md

Comparison date: 2026-10-10. Implementation checks in this section use a separate checkout of Loom commit `c94da7b`, with the same adjacent libfabric source reference as the original review. This pins the evidence while implementation changes continue in the working tree. The original findings and reproduction output above remain historical evidence rather than an assertion that every defect remains open.

The reports are broadly consistent. Eight of the ten original findings overlap with the consumer-readiness assessment, although this report makes the multi-receive consequence more explicit. The key-collision reproduction expands the existing registration requirements into a demonstrated failure of ordinary host registration; the collective-join defects are additional findings.

### Finding cross-reference

| Original finding                             | Coverage in CONSUMER_READINESS.md                                                                                                                                                                                                                     | Comparison result                                                                                                                                                                                                                                                            |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Registration key collisions               | [Registration configuration](CONSUMER_READINESS.md#3-make-registration-configurable-and-cache-policy-explicit) calls for requested keys and notes DMA-BUF key zero.                                                                                   | Additional concrete defect and software-provider evidence: ordinary host registration also fails on the second live MR when the application must assign keys.                                                                                                                |
| 2. Incorrect remote address model            | [Registration and resource lifetime findings](CONSUMER_READINESS.md#registration-and-resource-lifetime-findings) already identifies virtual addresses versus MR-relative offsets.                                                                     | Same defect; this report adds an end-to-end failing RMA transfer and a successful offset-based control.                                                                                                                                                                      |
| 3. Lost connection-request information       | [NCCL connection setup](CONSUMER_READINESS.md#connection-setup-needs-complete-addresses-and-owned-connection-events) already identifies lost `entry.info`, ownership, and private-data requirements.                                                  | Same defect at the pinned comparison revision. A later source check of `6a54890` confirms that `FI_CONNREQ` information is now retained in an owning `connection_info` field; this follow-up did not run an accept/reject handshake. Private-data delivery remains separate. |
| 4. Collective join inputs and result address | [Collective offload scope](CONSUMER_READINESS.md#keep-collective-offload-and-gin-as-separate-milestones) discusses optional consumer requirements but does not identify these join defects.                                                           | Additional correctness finding. Its P1 priority applies to the collective path; it does not make collective offload a prerequisite for an ordinary NCCL network plugin.                                                                                                      |
| 5. Incorrect generic EQ layout               | [usNIC and asynchronous AV requirements](CONSUMER_READINESS.md#the-usnic-path-exposes-prefix-and-asynchronous-av-gaps) already identifies incorrect generic-event decoding.                                                                           | Fixed in `c94da7b`; the comparison probe confirms preservation of control-event context and data. This does not establish support for asynchronous AV insertion or connection private data.                                                                                  |
| 6. Missing multi-receive buffer pointer      | [Completion semantics](CONSUMER_READINESS.md#4-preserve-full-completion-semantics) already calls for the received buffer pointer and field-validity rules.                                                                                            | This report identifies the specific unusable `FI_MULTI_RECV` path caused by losing that pointer.                                                                                                                                                                             |
| 7. DMA-BUF reserved offset                   | [Registration configuration](CONSUMER_READINESS.md#3-make-registration-configurable-and-cache-policy-explicit) and the [source audit](CONSUMER_READINESS.md#registration-and-resource-lifetime-findings) explicitly identify it.                      | Same defect; both probes capture the nonzero reserved offset.                                                                                                                                                                                                                |
| 8. Partial ordering promoted to strict       | [Conversion findings](CONSUMER_READINESS.md#conversion-findings) explicitly identify the composite-mask error.                                                                                                                                        | Same defect. The earlier probe uses `FI_ORDER_RAW`; this report uses `FI_ORDER_WAW`. Both incorrectly round-trip to `511`.                                                                                                                                                   |
| 9. Incomplete atomics                        | [Operation interface](CONSUMER_READINESS.md#1-complete-the-operation-interface-across-frontends) and [RDM atomics](CONSUMER_READINESS.md#rdm-atomics-are-present-but-do-not-carry-the-complete-operation) cover destinations and operand descriptors. | Same implementation gap. The consumer assessment also covers operation flags, validity limits, and operand lifetimes.                                                                                                                                                        |
| 10. Incomplete async RMA                     | [Operation interface](CONSUMER_READINESS.md#1-complete-the-operation-interface-across-frontends) explicitly covers senders and Asio.                                                                                                                  | Same implementation gap. The consumer assessment additionally covers Rust's missing RMA submission interface.                                                                                                                                                                |

### Additional current defects omitted from the original ten findings

The consumer-readiness [conversion findings](CONSUMER_READINESS.md#conversion-findings) include five further issues. A fresh probe against `c94da7b` confirmed each:

| Additional defect                      | Verified behavior                                                                                                                  | Relevant implementation                                                                                              |
| -------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| Completion-order encoding mismatch     | Provider `FI_ORDER_DATA` is not recognized as Loom `completion_ordering::data`; native `comp_order` is wrapped without conversion. | [fabric attributes](src/fabric.cpp), [completion-order constants](include/loom/core/types.hpp)                       |
| Lost provider address formats          | EFA, CXI, UCX, string, and mixed-IP address formats all fail native-to-Loom-to-native round trips.                                 | [address-format conversion](include/loom/detail/conversions.hpp)                                                     |
| Lost unified control progress          | `FI_PROGRESS_CONTROL_UNIFIED` fails to round-trip and the resulting policy reports that manual control progress is not required.   | [progress conversion](include/loom/detail/conversions.hpp), [progress policy](include/loom/core/progress_policy.hpp) |
| Lost source and RMA-event capabilities | Neither `FI_SOURCE` nor `FI_RMA_EVENT` survives capability conversion.                                                             | [capability conversion](include/loom/detail/conversions.hpp)                                                         |
| Lost legacy basic MR semantics         | `FI_MR_BASIC` fails to round-trip; Loom's `mr_mode_flags::basic` is zero, which denotes different modern registration semantics.   | [MR-mode conversion](include/loom/detail/conversions.hpp), [MR-mode constants](include/loom/core/types.hpp)          |

These should be included when using this report as a correctness backlog. The original ten findings are not a complete replacement for the consumer assessment.

Further source checks confirmed that the consumer assessment also retains relevant findings outside the original ten:

- [Provider traits](CONSUMER_READINESS.md#provider-traits-need-narrower-source-backed-guarantees) still contradict the inspected provider implementations. Examples include automatic-data-progress claims for CXI and SHM despite their manual-progress declarations, and native atomics claimed for the UCX adapter despite its unsupported atomic query interface. Provider-bound assumptions must be scoped and established before they authorize static dispatch.
- [CQ compatibility](CONSUMER_READINESS.md#cq-format-and-source-preserving-reads-are-compatibility-requirements) remains incomplete: Loom always requests tagged format, while the inspected usNIC path rejects that format. Normal CQ reads also lack source-address preservation.
- [Rust submission errors](CONSUMER_READINESS.md#rust-needs-structured-backpressure-and-shared-completion-dispatch) still lose numeric status through exception-message conversion, so `is_would_block()` does not reliably identify submission `FI_EAGAIN`.
- [HMEM and DMA-BUF cache lookups](CONSUMER_READINESS.md#3-make-registration-configurable-and-cache-policy-explicit) still create registrations without a reuse lookup. This is distinct from the fixed host-cache invalidation defect.

These checks do not independently re-audit the external consumer repositories or establish hardware support.

### Status corrections for the consumer assessment

The four original defects in [Fix confirmed correctness defects first](CONSUMER_READINESS.md#fix-confirmed-correctness-defects-first) are fixed at the comparison revision. The document labels its reproduction output historical, but only explicitly notes the later iterator and cache changes. A current status table should also record the context fixes:

| Historical defect                                                         | Fix commit | Comparison validation                                                                                                                 |
| ------------------------------------------------------------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| Provider iteration does not advance                                       | `0e66762`  | A constructed two-entry list visits both entries and terminates.                                                                      |
| Context-pool growth relocates outstanding storage                         | `72f6406`  | Growing from one context through 128 additional acquisitions preserves the original context address and its provider-storage address. |
| Moving a typed context retains the source payload pointer                 | `8705a82`  | The moved context's user-data pointer refers to its own payload.                                                                      |
| Cache invalidation permits new acquisition of an active invalidated entry | `567e501`  | A subsequent lookup receives a distinct registration while the prior registration remains alive for its existing holder.              |

Three EQ statements also need updated status:

- The claim in [NCCL connection setup](CONSUMER_READINESS.md#connection-setup-needs-complete-addresses-and-owned-connection-events) that every negative EQ read triggers an unchecked error read was fixed in `19c58a7`. The original audit verified that `-FI_EIO` is preserved without calling `fi_eq_readerr`.
- The generic `fi_eq_entry` decoding defect described under [usNIC](CONSUMER_READINESS.md#the-usnic-path-exposes-prefix-and-asynchronous-av-gaps), and original finding 5 in this report, was fixed in `c94da7b`. Connection-request ownership and private-data handling are separate questions.
- After the pinned comparison probe, `6a54890` added shared ownership of incoming `fi_info` through `fabric_event::connection_info`. Source inspection confirms the fix for original finding 3's discarded-info problem. This does not resolve the readiness document's additional connection-private-data requirement, and this comparison did not validate a complete connection handshake against that later commit.

The readiness document's example of a tautological provider-range assertion still exists, but [test_fabric_iteration.cpp](tests/test_fabric_iteration.cpp) now contains focused enumeration and iterator regressions. That example should not imply the iterator has no meaningful test coverage.

### Comparison evidence and priority implications

The additional comparison probe reported:

```text
completion_data_recognized=0
efa_address_roundtrips=0
cxi_address_roundtrips=0
ucx_address_roundtrips=0
str_address_roundtrips=0
sockaddr_ip_address_roundtrips=0
unified_progress_roundtrips=0 manual_control_required=0
source_capability_preserved=0
rma_event_capability_preserved=0
legacy_basic_mr_preserved=0
iterator_visits_two_entries=1
pool_growth_preserves_storage=1
moved_context_points_to_own_data=1
invalidation_excludes_active_entry=1 both_registrations_alive=1
generic_EQ_context_and_data_preserved=1
```

Temporary artifacts: `/tmp/loom-libfabric-audit/readiness_comparison.cpp` and `/tmp/loom-libfabric-audit/comparison-results.txt`. The probe used an isolated source snapshot at `/tmp/loom-libfabric-audit/comparison-source`; it exercised conversion and ownership behavior with constructed inputs and a mock registration provider. It was not a full test-suite or hardware run.

The consumer assessment's implementation order remains appropriate: prioritize the selected consumer's registration, discovery, operation, and completion contracts, retain regressions for resolved defects, and develop working consumer integrations early. Add the demonstrated key-collision failure to the registration acceptance tests and retain the successful-offset/failing-virtual-address RMA comparison as a regression. Keep collective correctness tracked within the optional collective milestone. None of these findings requires abandoning compile-time specialization; the specialization must represent an established provider/profile contract.
