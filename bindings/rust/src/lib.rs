// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! # loom-fabric
//!
//! Rust bindings for [loom](https://github.com/sielicki/loom), a modern C++26
//! libfabric binding library.
//!
//! This crate provides typed Rust wrappers around loom's C++ API,
//! enabling high-performance RDMA and fabric networking from Rust applications.
//!
//! ## Features
//!
//! - **Type-safe**: Strong types prevent common errors
//! - **Async-ready**: Optional tokio integration for async I/O
//! - **Zero-copy**: Direct access to registered memory regions
//! - **Provider-agnostic**: Works with any libfabric provider (verbs, EFA, TCP, etc.)
//!
//! ## Example
//!
//! ```rust,no_run
//! use loom_fabric::{Fabric, Domain, Endpoint, CompletionQueue, Capabilities, CqBindFlags};
//!
//! fn main() -> loom_fabric::Result<()> {
//!     // Query for a fabric provider
//!     let info = loom_fabric::query_fabric(Capabilities::MSG | Capabilities::TAGGED)?;
//!
//!     // Create fabric resources
//!     let fabric = Fabric::new(&info)?;
//!     let domain = Domain::new(&fabric, &info)?;
//!     let cq = CompletionQueue::new(&domain, 128)?;
//!     let mut endpoint = Endpoint::new(&domain, &info)?;
//!
//!     // Bind and enable
//!     endpoint.bind_cq(&cq, CqBindFlags::TRANSMIT | CqBindFlags::RECV)?;
//!     endpoint.enable()?;
//!
//!     Ok(())
//! }
//! ```

#![warn(missing_docs)]
#![warn(rust_2018_idioms)]

mod discovery;
mod error;
mod ffi;

#[cfg(feature = "tokio")]
mod async_cq;

pub use discovery::{
    query_fabrics, AddressFormat, FabricAttributes, FabricHints, Modes, MrMode, ProgressMode,
    ThreadingMode,
};
pub use error::{Error, Result};
pub use ffi::ffi::{RemoteMemory, RmaCompletion, RmaOptions, RmaTarget};

use cxx::UniquePtr;
use std::rc::Rc;

impl Default for RmaOptions {
    fn default() -> Self {
        Self {
            completion: RmaCompletion::Local,
            fence: false,
        }
    }
}

impl RemoteMemory {
    /// Restrict an exported registration without changing its key or address model.
    pub fn subregion(self, offset: usize, length: usize) -> Result<Self> {
        if offset > self.length || length > self.length - offset {
            return Err(Error::InvalidArgument(
                "remote subregion exceeds registration".into(),
            ));
        }
        let addr = self
            .addr
            .checked_add(offset as u64)
            .filter(|addr| addr.checked_add(length as u64).is_some())
            .ok_or_else(|| Error::InvalidArgument("remote address overflows".into()))?;
        Ok(Self {
            addr,
            key: self.key,
            length,
        })
    }
}

// ============================================================================
// Capability flags
// ============================================================================

bitflags::bitflags! {
    /// Fabric capability flags.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct Capabilities: u64 {
        /// Basic messaging (send/recv).
        const MSG = 0x1;
        /// Remote Memory Access.
        const RMA = 0x2;
        /// Tagged messaging.
        const TAGGED = 0x4;
        /// Atomic operations.
        const ATOMIC = 0x8;
        /// Read capability.
        const READ = 0x10;
        /// Write capability.
        const WRITE = 0x20;
        /// Receive capability.
        const RECV = 0x80;
        /// Send capability.
        const SEND = 0x100;
        /// Remote read capability.
        const REMOTE_READ = 0x200;
        /// Remote write capability.
        const REMOTE_WRITE = 0x400;
        /// Heterogeneous memory capability; registration may still fail for a device.
        const HMEM = 0x10000;
        /// Remote memory access events.
        const RMA_EVENT = 0x10000000;
        /// Completion source addresses.
        const SOURCE = 0x20000000;
    }
}

bitflags::bitflags! {
    /// Completion queue bind flags.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct CqBindFlags: u64 {
        /// Bind for transmit completions.
        const TRANSMIT = 0x1;
        /// Bind for receive completions.
        const RECV = 0x2;
    }
}

bitflags::bitflags! {
    /// Memory region access flags.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct MrAccess: u64 {
        /// Local read access.
        const READ = 0x1;
        /// Local write access.
        const WRITE = 0x2;
        /// Remote read access.
        const REMOTE_READ = 0x4;
        /// Remote write access.
        const REMOTE_WRITE = 0x8;
        /// Send access.
        const SEND = 0x10;
        /// Receive access.
        const RECV = 0x20;
    }
}

/// Endpoint type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum EndpointType {
    /// Unspecified endpoint type.
    Unspec = 0,
    /// Message endpoint (connection-oriented).
    Msg = 1,
    /// Reliable datagram endpoint (connectionless, reliable).
    Rdm = 2,
    /// Datagram endpoint (connectionless, unreliable).
    Dgram = 3,
}

// ============================================================================
// FabricInfo
// ============================================================================

/// Information about a fabric provider.
///
/// Obtained from [`query_fabric`] and used to create fabric resources.
pub struct FabricInfo {
    inner: UniquePtr<ffi::ffi::FabricInfoHandle>,
}

impl FabricInfo {
    fn new(inner: UniquePtr<ffi::ffi::FabricInfoHandle>) -> Self {
        Self { inner }
    }

    /// Owned discovery facts for this entry; unknown native obligations fail explicitly.
    pub fn attributes(&self) -> Result<FabricAttributes> {
        ffi::ffi::fabric_info_attributes(&self.inner)?.try_into()
    }
}

/// Query for fabric providers matching the specified capabilities.
///
/// # Arguments
///
/// * `capabilities` - Required capabilities for the provider.
///
/// # Returns
///
/// The first matching [`FabricInfo`], or an error if none found.
///
/// # Example
///
/// ```rust,no_run
/// use loom_fabric::{query_fabric, Capabilities};
///
/// let info = query_fabric(Capabilities::MSG | Capabilities::TAGGED)?;
/// # Ok::<(), loom_fabric::Error>(())
/// ```
pub fn query_fabric(capabilities: Capabilities) -> Result<FabricInfo> {
    query_fabric_with_hints("", capabilities, EndpointType::Unspec)
}

/// Query for the first provider matching these basic filters.
/// Use [`query_fabrics`] for domain selection and negotiated attributes.
///
/// # Arguments
///
/// * `provider` - Provider name filter (empty for any).
/// * `capabilities` - Required capabilities.
/// * `ep_type` - Endpoint type.
pub fn query_fabric_with_hints(
    provider: &str,
    capabilities: Capabilities,
    ep_type: EndpointType,
) -> Result<FabricInfo> {
    let hints = FabricHints {
        provider_name: Some(provider.to_owned()),
        capabilities,
        endpoint_type: ep_type,
        ..FabricHints::default()
    }
    .to_ffi()?;
    let inner = ffi::ffi::query_fabric(&hints)?;
    Ok(FabricInfo::new(inner))
}

// ============================================================================
// Fabric
// ============================================================================

/// A fabric instance representing a connection to a fabric provider.
///
/// The fabric is the top-level resource from which all other resources
/// are created.
#[derive(Clone)]
pub struct Fabric {
    inner: Rc<UniquePtr<ffi::ffi::FabricHandle>>,
}

impl Fabric {
    /// Create a new fabric from fabric info.
    ///
    /// # Arguments
    ///
    /// * `info` - Fabric information from [`query_fabric`].
    pub fn new(info: &FabricInfo) -> Result<Self> {
        let inner = ffi::ffi::create_fabric(&info.inner)?;
        Ok(Self { inner: Rc::new(inner) })
    }
}

// ============================================================================
// Domain
// ============================================================================

/// A fabric domain representing a resource boundary.
///
/// All resources (endpoints, memory regions, etc.) belong to a domain.
#[derive(Clone)]
pub struct Domain {
    inner: Rc<UniquePtr<ffi::ffi::DomainHandle>>,
    _fabric: Fabric,
}

impl Domain {
    /// Create a new domain from a fabric.
    ///
    /// # Arguments
    ///
    /// * `fabric` - The parent fabric.
    /// * `info` - Fabric information.
    pub fn new(fabric: &Fabric, info: &FabricInfo) -> Result<Self> {
        let inner = ffi::ffi::create_domain(&fabric.inner, &info.inner)?;
        Ok(Self { inner: Rc::new(inner), _fabric: fabric.clone() })
    }
}

// ============================================================================
// CompletionQueue
// ============================================================================

/// A completion event from the completion queue.
#[derive(Debug, Clone, Copy)]
pub struct CompletionEvent {
    /// User-provided context.
    pub context: usize,
    /// Number of bytes transferred.
    pub bytes_transferred: usize,
    /// Completion flags.
    pub flags: u64,
    /// Message tag (for tagged messaging).
    pub tag: u64,
    /// Immediate data.
    pub data: u64,
    /// Fabric error code, if this is an error completion.
    pub error: Option<i32>,
}

impl CompletionEvent {
    fn from_raw(data: ffi::CompletionEventData) -> Self {
        Self {
            context: data.context,
            bytes_transferred: data.bytes_transferred,
            flags: data.flags,
            tag: data.tag,
            data: data.data,
            error: data.has_error.then_some(data.error_code),
        }
    }

    /// Return the completion or an error retaining its operation context.
    pub fn into_result(self) -> Result<Self> {
        match self.error {
            Some(code) => Err(Error::Completion { context: self.context, code }),
            None => Ok(self),
        }
    }
}

/// A completion queue for operation completion notifications.
#[derive(Clone)]
pub struct CompletionQueue {
    inner: Rc<UniquePtr<ffi::ffi::CompletionQueueHandle>>,
    _domain: Domain,
    #[cfg(feature = "tokio")]
    router: Rc<async_cq::CompletionRouter>,
}

impl CompletionQueue {
    /// Create a new completion queue.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `size` - Number of entries in the queue.
    pub fn new(domain: &Domain, size: usize) -> Result<Self> {
        Self::with_options(domain, size, true)
    }

    /// Create a new completion queue with options.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `size` - Number of entries in the queue.
    /// * `wait_obj` - Enable wait object for async notification.
    pub fn with_options(domain: &Domain, size: usize, wait_obj: bool) -> Result<Self> {
        let attr = ffi::CqAttributes { size, wait_obj };
        let inner = ffi::ffi::create_completion_queue(&domain.inner, &attr)?;
        Ok(Self {
            inner: Rc::new(inner),
            _domain: domain.clone(),
            #[cfg(feature = "tokio")]
            router: Rc::new(async_cq::CompletionRouter::default()),
        })
    }

    /// Poll for an unclaimed completion (non-blocking).
    ///
    /// Managed async operations receive their own completions, including when
    /// another clone polls this queue. Unmatched events and completions whose
    /// observers were dropped remain available here, in arrival order.
    /// With Tokio enabled, each call reads at most 64 native entries; `None`
    /// can also mean that this budget was spent routing managed completions.
    pub fn poll(&self) -> Option<CompletionEvent> {
        #[cfg(feature = "tokio")]
        {
            if let Some(event) = self.router.pop_event() {
                return Some(event);
            }
            self.progress(async_cq::PROGRESS_BUDGET);
            self.router.pop_event()
        }
        #[cfg(not(feature = "tokio"))]
        self.poll_native()
    }

    fn poll_native(&self) -> Option<CompletionEvent> {
        let read = ffi::ffi::cq_poll(&self.inner);
        match read.kind {
            ffi::CqReadKind::Empty => None,
            ffi::CqReadKind::Event => Some(CompletionEvent::from_raw(read.event)),
            _ => unreachable!("invalid completion poll result"),
        }
    }

    /// Read and route at most `budget` native completions without blocking.
    ///
    /// Returns the number read. Reaching the budget requires another progress
    /// call before assuming the CQ is drained. Unclaimed events are preserved
    /// for [`Self::poll`] or [`Self::wait`]; managed observers are notified.
    /// Callers must continue progress until accepted work retires, including
    /// after a queue failure. Such a failure does not establish buffer reuse.
    #[cfg(feature = "tokio")]
    pub fn progress(&self, budget: usize) -> usize {
        self.router.progress(budget, || self.poll_native())
    }

    /// Wait for an unclaimed completion with timeout.
    ///
    /// The queue must have been created with `wait_obj` enabled. An empty or
    /// timed-out wait does not report completion or retire an operation.
    /// Managed async completions are routed to their observers without
    /// restarting the timeout. Use async waits on a Tokio executor thread.
    ///
    /// * `timeout_ms` - Timeout in milliseconds, or `None` for infinite.
    pub fn wait(&self, timeout_ms: Option<i32>) -> Result<CompletionEvent> {
        #[cfg(feature = "tokio")]
        {
            self.router.wait(timeout_ms, |timeout| {
                self.decode_wait(ffi::ffi::cq_wait(&self.inner, timeout))
            })
        }

        #[cfg(not(feature = "tokio"))]
        {
            let read = ffi::ffi::cq_wait(&self.inner, timeout_ms.unwrap_or(-1));
            self.decode_wait(read)?.into_result()
        }
    }

    fn decode_wait(&self, read: ffi::CqReadResult) -> Result<CompletionEvent> {
        match read.kind {
            ffi::CqReadKind::Empty => Err(Error::WouldBlock),
            ffi::CqReadKind::Event => Ok(CompletionEvent::from_raw(read.event)),
            ffi::CqReadKind::Failure => Err(error::from_queue_status(read.error_code)),
            _ => unreachable!("invalid completion wait result"),
        }
    }

    /// Get the wait file descriptor for async integration.
    ///
    /// This can be used with tokio's `AsyncFd` for async notification.
    pub fn wait_fd(&self) -> Result<i32> {
        Ok(ffi::ffi::cq_get_wait_fd(&self.inner)?)
    }

    /// Prepare a native descriptor wait. A false result requires more polling.
    /// Call this before every potentially blocking wait on [`Self::wait_fd`].
    pub fn try_wait(&self) -> Result<bool> {
        Ok(ffi::ffi::cq_try_wait(&self.inner, &self._domain._fabric.inner)?)
    }

    /// Check if the completion queue supports blocking wait.
    pub fn supports_wait(&self) -> bool {
        ffi::ffi::cq_supports_wait(&self.inner)
    }
}

// ============================================================================
// AddressVector
// ============================================================================

/// An address vector for peer addressing.
#[derive(Clone)]
pub struct AddressVector {
    inner: Rc<UniquePtr<ffi::ffi::AddressVectorHandle>>,
    _domain: Domain,
}

impl AddressVector {
    /// Create a new address vector.
    pub fn new(domain: &Domain) -> Result<Self> {
        let inner = ffi::ffi::create_address_vector(&domain.inner)?;
        Ok(Self { inner: Rc::new(inner), _domain: domain.clone() })
    }

    /// Insert an address into the address vector.
    ///
    /// Returns the fabric address that can be used for communication.
    ///
    /// # Safety
    ///
    /// `addr` must contain a complete address in this provider's native
    /// format, such as one returned by an endpoint using the same provider.
    pub unsafe fn insert(&mut self, addr: &[u8]) -> Result<u64> {
        Ok(ffi::ffi::av_insert(&self.inner, addr)?)
    }
}

// ============================================================================
// MemoryRegion
// ============================================================================

/// A registered memory region for RDMA operations.
/// Clones share the registration; deregistration follows the last retained clone.
#[derive(Clone)]
pub struct MemoryRegion {
    inner: Rc<UniquePtr<ffi::ffi::MemoryRegionHandle>>,
    _domain: Domain,
}

impl MemoryRegion {
    /// Register a memory region.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the memory region pointed to by `ptr`
    /// remains valid until every clone and accepted operation retaining this
    /// registration has retired. Registration does not own the allocation.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `ptr` - Pointer to the memory to register.
    /// * `len` - Length of the memory region in bytes.
    /// * `access` - Access flags for the region.
    pub unsafe fn register(
        domain: &Domain,
        ptr: *mut u8,
        len: usize,
        access: MrAccess,
    ) -> Result<Self> {
        let inner = ffi::ffi::register_memory(&domain.inner, ptr as usize, len, access.bits())?;
        Ok(Self {
            inner: Rc::new(inner),
            _domain: domain.clone(),
        })
    }

    /// Get the memory region key for remote access.
    pub fn key(&self) -> u64 {
        ffi::ffi::mr_key(&self.inner)
    }

    /// Get the local descriptor for use in operations.
    pub fn descriptor(&self) -> usize {
        ffi::ffi::mr_descriptor(&self.inner)
    }

    /// Export the negotiated remote address, 64-bit key and registered length.
    /// Peers may use this metadata only while this registration remains alive.
    pub fn remote_memory(&self) -> RemoteMemory {
        ffi::ffi::mr_remote_memory(&self.inner)
    }
}

// ============================================================================
// Endpoint
// ============================================================================

/// Stable, zero-initialized provider scratch storage for one native operation.
/// Moving this owner preserves its address. Keep it alive until the operation
/// completes or its endpoint closes successfully; never share it between
/// outstanding operations. A new context starts with fresh provider storage.
pub struct OperationContext {
    inner: UniquePtr<ffi::ffi::OperationContextHandle>,
}

impl OperationContext {
    /// Allocate native FI_CONTEXT2 storage, also suitable for FI_CONTEXT.
    pub fn new() -> Result<Self> {
        Ok(Self {
            inner: ffi::ffi::create_operation_context()?,
        })
    }

    /// Native address to submit and match against a completion event's context.
    pub fn address(&self) -> usize {
        ffi::ffi::operation_context_address(&self.inner)
    }
}

/// A fabric endpoint for communication.
///
/// Context arguments are native addresses, not arbitrary operation IDs.
/// When the selected mode requires FI_CONTEXT or FI_CONTEXT2, they must
/// reference suitably aligned, writable native scratch storage that remains
/// stable until completion or successful close. AsyncEndpoint manages this
/// storage for its own submissions.
///
/// Submitting borrowed buffers requires an explicit lifetime guarantee:
///
/// ```compile_fail
/// fn submit(ep: &mut loom_fabric::Endpoint, data: &[u8]) {
///     ep.send(data, 0).unwrap();
/// }
/// ```
pub struct Endpoint {
    inner: UniquePtr<ffi::ffi::EndpointHandle>,
    _domain: Domain,
    bound_queues: Vec<CompletionQueue>,
    bound_vectors: Vec<AddressVector>,
}

impl Endpoint {
    /// Create a new endpoint.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `info` - Fabric information.
    pub fn new(domain: &Domain, info: &FabricInfo) -> Result<Self> {
        let inner = ffi::ffi::create_endpoint(&domain.inner, &info.inner)?;
        Ok(Self { inner, _domain: domain.clone(), bound_queues: Vec::new(), bound_vectors: Vec::new() })
    }

    /// Enable the endpoint for communication.
    ///
    /// The endpoint must be bound to completion queues and address vectors
    /// before being enabled.
    pub fn enable(&mut self) -> Result<()> {
        Ok(ffi::ffi::endpoint_enable(self.inner.pin_mut())?)
    }

    /// Close the native endpoint. Success discards outstanding operations and
    /// can discard CQ entries without reporting their completions. Failure
    /// retains the endpoint and its bound resources so close can be retried.
    /// A closed endpoint rejects further operations; repeated close succeeds.
    pub fn close(&mut self) -> Result<()> {
        match ffi::ffi::endpoint_close(self.inner.pin_mut()) {
            0 => Ok(()),
            code => Err(Error::Close { code }),
        }
    }

    /// Bind a completion queue to the endpoint.
    ///
    /// # Arguments
    ///
    /// * `cq` - The completion queue to bind.
    /// * `flags` - Bind flags (transmit, recv, or both).
    pub fn bind_cq(&mut self, cq: &CompletionQueue, flags: CqBindFlags) -> Result<()> {
        ffi::ffi::endpoint_bind_cq(self.inner.pin_mut(), &cq.inner, flags.bits())?;
        self.bound_queues.push(cq.clone());
        Ok(())
    }

    /// Bind an address vector to the endpoint.
    pub fn bind_av(&mut self, av: &AddressVector) -> Result<()> {
        ffi::ffi::endpoint_bind_av(self.inner.pin_mut(), &av.inner)?;
        self.bound_vectors.push(av.clone());
        Ok(())
    }

    /// Post a send operation.
    ///
    /// # Arguments
    ///
    /// * `data` - Data to send.
    /// * `context` - User context for completion matching.
    ///
    /// # Safety
    ///
    /// The buffer and any registered memory or operation context must remain
    /// valid until the matching completion is consumed. Receive buffers must
    /// not be accessed, and send buffers must not be mutated, before then.
    /// A successful return only means the operation was submitted.
    pub unsafe fn send(&mut self, data: &[u8], context: usize) -> Result<()> {
        error::from_submission_status(ffi::ffi::endpoint_send(
            self.inner.pin_mut(),
            data,
            context,
        )?)
    }

    /// Post a receive operation.
    ///
    /// # Arguments
    ///
    /// * `buf` - Buffer to receive into.
    /// * `context` - User context for completion matching.
    ///
    /// # Safety
    ///
    /// The buffer and any registered memory or operation context must remain
    /// valid until the matching completion is consumed. Receive buffers must
    /// not be accessed, and send buffers must not be mutated, before then.
    /// A successful return only means the operation was submitted.
    pub unsafe fn recv(&mut self, buf: &mut [u8], context: usize) -> Result<()> {
        error::from_submission_status(ffi::ffi::endpoint_recv(self.inner.pin_mut(), buf, context)?)
    }

    /// Post a tagged send operation.
    ///
    /// # Arguments
    ///
    /// * `data` - Data to send.
    /// * `tag` - Message tag for matching.
    /// * `context` - User context for completion matching.
    ///
    /// # Safety
    ///
    /// The buffer and any registered memory or operation context must remain
    /// valid until the matching completion is consumed. Receive buffers must
    /// not be accessed, and send buffers must not be mutated, before then.
    /// A successful return only means the operation was submitted.
    pub unsafe fn tagged_send(&mut self, data: &[u8], tag: u64, context: usize) -> Result<()> {
        error::from_submission_status(ffi::ffi::endpoint_tagged_send(
            self.inner.pin_mut(),
            data,
            tag,
            context,
        )?)
    }

    /// Post a tagged receive operation.
    ///
    /// # Arguments
    ///
    /// * `buf` - Buffer to receive into.
    /// * `tag` - Message tag for matching.
    /// * `ignore` - Bits to ignore in tag matching.
    /// * `context` - User context for completion matching.
    ///
    /// # Safety
    ///
    /// The buffer and any registered memory or operation context must remain
    /// valid until the matching completion is consumed. Receive buffers must
    /// not be accessed, and send buffers must not be mutated, before then.
    /// A successful return only means the operation was submitted.
    pub unsafe fn tagged_recv(
        &mut self,
        buf: &mut [u8],
        tag: u64,
        ignore: u64,
        context: usize,
    ) -> Result<()> {
        error::from_submission_status(ffi::ffi::endpoint_tagged_recv(
            self.inner.pin_mut(),
            buf,
            tag,
            ignore,
            context,
        )?)
    }

    /// Send a small message without generating a completion.
    ///
    /// This is more efficient for small messages but doesn't provide
    /// completion notification.
    pub fn inject(&mut self, data: &[u8]) -> Result<()> {
        error::from_submission_status(ffi::ffi::endpoint_inject(self.inner.pin_mut(), data)?)
    }

    /// Submit a registered RMA read to an explicit peer, requesting a CQ completion.
    /// Local and remote bounds and registration-domain identity are checked.
    ///
    /// # Safety
    ///
    /// The target must identify a live, readable remote registration and a peer
    /// in this endpoint's bound address vector. Keep the local buffer, registration
    /// and native context valid until completion or successful close. Do not access
    /// the local buffer while the operation may write it. Success means acceptance,
    /// not completion; a later error does not make other accepted work reusable.
    pub unsafe fn read(
        &mut self,
        buffer: &mut [u8],
        registration: &MemoryRegion,
        target: RmaTarget,
        options: RmaOptions,
        context: usize,
    ) -> Result<()> {
        self.check_registration(registration)?;
        error::from_submission_status(ffi::ffi::endpoint_read(
            self.inner.pin_mut(),
            buffer,
            &registration.inner,
            &target,
            &options,
            context,
        )?)
    }

    /// Submit a registered RMA write to an explicit peer, requesting a CQ completion.
    /// Local and remote bounds and registration-domain identity are checked.
    ///
    /// # Safety
    ///
    /// The target must identify a live, writable remote registration and a peer
    /// in this endpoint's bound address vector. Keep the local buffer, registration
    /// and native context valid until completion or successful close. Do not mutate
    /// the local buffer while the operation may read it. Local completion permits
    /// local reuse; it does not establish remote delivery or GPU visibility.
    pub unsafe fn write(
        &mut self,
        buffer: &[u8],
        registration: &MemoryRegion,
        target: RmaTarget,
        options: RmaOptions,
        context: usize,
    ) -> Result<()> {
        self.check_registration(registration)?;
        error::from_submission_status(ffi::ffi::endpoint_write(
            self.inner.pin_mut(),
            buffer,
            &registration.inner,
            &target,
            &options,
            context,
        )?)
    }

    fn check_registration(&self, registration: &MemoryRegion) -> Result<()> {
        if !Rc::ptr_eq(&self._domain.inner, &registration._domain.inner) {
            return Err(Error::InvalidArgument(
                "registration belongs to another domain".into(),
            ));
        }
        Ok(())
    }

    /// Get the local address/name of the endpoint.
    pub fn name(&self) -> Result<Vec<u8>> {
        Ok(ffi::ffi::endpoint_get_name(&self.inner)?)
    }
}

impl Drop for Endpoint {
    fn drop(&mut self) {
        if self.close().is_err() {
            // Native work may still reference the endpoint's entire resource
            // graph, including managed contexts retained by bound queues.
            // Drop cannot report the failure or prove quiescence. Retain the
            // graph rather than release storage the provider can still use.
            std::mem::forget(std::mem::replace(&mut self.inner, UniquePtr::null()));
            std::mem::forget(self._domain.clone());
            std::mem::forget(std::mem::take(&mut self.bound_queues));
            std::mem::forget(std::mem::take(&mut self.bound_vectors));
        }
    }
}

// ============================================================================
// Async support (tokio feature)
// ============================================================================

#[cfg(feature = "tokio")]
pub use async_cq::{AsyncCompletionQueue, AsyncEndpoint};

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "requires the libfabric UDP provider"]
    fn unspecified_discovery_accepts_datagram_only_providers() {
        query_fabric_with_hints("udp", Capabilities::MSG, EndpointType::Unspec)
            .expect("unspecified discovery must not require message endpoints");
        assert!(query_fabric_with_hints("udp", Capabilities::MSG, EndpointType::Msg).is_err());
    }

    #[test]
    #[ignore = "requires the libfabric TCP provider"]
    fn receive_backpressure_survives_the_cpp_bridge() {
        #[repr(C)]
        #[derive(Clone, Default)]
        struct Receive {
            context: [usize; 8],
            buffer: [u8; 1],
        }

        // Each accepted receive owns distinct buffer and FI_CONTEXT2 storage.
        // Declare these before the endpoint so even a panic closes it first.
        let mut receives = vec![Receive::default(); 65_536];
        let mut retry = Receive::default();
        let info = query_fabric_with_hints("tcp", Capabilities::MSG, EndpointType::Msg).unwrap();
        let fabric = Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let cq = CompletionQueue::with_options(&domain, 16, false).unwrap();
        let mut endpoint = Endpoint::new(&domain, &info).unwrap();
        endpoint
            .bind_cq(&cq, CqBindFlags::TRANSMIT | CqBindFlags::RECV)
            .unwrap();
        endpoint.enable().unwrap();
        let mut accepted = 0;
        let mut rejected = None;
        for receive in &mut receives {
            let context = receive.context.as_mut_ptr() as usize;
            // No peer is connected. Accepted buffers stay untouched until
            // endpoint close discards the outstanding receives.
            match unsafe { endpoint.recv(&mut receive.buffer, context) } {
                Ok(()) => accepted += 1,
                Err(error) => {
                    rejected = Some(error);
                    break;
                }
            }
        }
        let retry_error =
            unsafe { endpoint.recv(&mut retry.buffer, retry.context.as_mut_ptr() as usize) }.err();
        drop(endpoint);
        assert!(accepted > 0);
        let error = rejected.expect("TCP receive queue did not report backpressure");
        assert!(
            error.is_would_block(),
            "lost FI_EAGAIN across FFI: {error:?}"
        );
        let again = ffi::ffi::error_again();
        assert!(matches!(error, Error::Submission { code } if code == again));
        assert!(matches!(retry_error, Some(Error::Submission { code }) if code == again));
        eprintln!("TCP accepted {accepted} receives; both subsequent rejections retained {again}");
    }

    #[test]
    #[ignore = "requires the libfabric TCP provider with wait descriptors"]
    fn queue_wait_failures_are_not_completions() {
        let info = query_fabric_with_hints("tcp", Capabilities::MSG, EndpointType::Msg).unwrap();
        let fabric = Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let queue = CompletionQueue::new(&domain, 16).unwrap();
        assert!(queue.poll().is_none());
        let error = queue.wait(Some(1)).unwrap_err();
        assert!(error.is_would_block() || error.is_timeout(), "{error:?}");
        assert!(!matches!(error, Error::Completion { .. }));
        let mut read = ffi::CqReadResult {
            kind: ffi::CqReadKind::Failure,
            event: ffi::CompletionEventData {
                context: 0,
                bytes_transferred: 0,
                flags: 0,
                tag: 0,
                data: 0,
                has_error: true,
                error_code: ffi::ffi::error_timeout(),
            },
            error_code: ffi::ffi::error_timeout(),
        };
        assert!(matches!(queue.decode_wait(read), Err(Error::Timeout)));
        read.error_code = ffi::ffi::error_again();
        assert!(matches!(queue.decode_wait(read), Err(Error::WouldBlock)));
        read.error_code = -1_234_567;
        assert!(matches!(queue.decode_wait(read), Err(Error::Queue { code: -1_234_567 })));
        read.kind = ffi::CqReadKind::Event;
        let error = queue.decode_wait(read).unwrap().into_result().unwrap_err();
        assert!(matches!(error, Error::Completion { context: 0, code } if code == ffi::ffi::error_timeout()));
        assert!(!error.is_timeout());
        read.kind = ffi::CqReadKind::Empty;
        assert!(matches!(queue.decode_wait(read), Err(Error::WouldBlock)));
    }

    #[test]
    fn error_completions_retain_their_context() {
        let event = CompletionEvent::from_raw(ffi::CompletionEventData {
            context: 73,
            bytes_transferred: 4,
            flags: 0,
            tag: 9,
            data: 0,
            has_error: true,
            error_code: -125,
        });
        assert_eq!(event.error, Some(-125));
        assert_eq!(event.tag, 9);
        assert!(matches!(event.into_result(), Err(Error::Completion { context: 73, code: -125 })));
    }

    #[test]
    fn endpoints_retain_bound_resources_and_parents() {
        let Ok(info) = query_fabric_with_hints("tcp", Capabilities::MSG, EndpointType::Msg) else {
            return;
        };
        let fabric = Fabric::new(&info).unwrap();
        let fabric_lifetime = Rc::downgrade(&fabric.inner);
        let domain = Domain::new(&fabric, &info).unwrap();
        let domain_lifetime = Rc::downgrade(&domain.inner);
        let cq = CompletionQueue::with_options(&domain, 16, false).unwrap();
        let cq_lifetime = Rc::downgrade(&cq.inner);
        let mut endpoint = Endpoint::new(&domain, &info).unwrap();
        endpoint.bind_cq(&cq, CqBindFlags::TRANSMIT | CqBindFlags::RECV).unwrap();
        drop(cq);
        drop(domain);
        drop(fabric);
        assert!(cq_lifetime.upgrade().is_some());
        assert!(domain_lifetime.upgrade().is_some());
        assert!(fabric_lifetime.upgrade().is_some());
        drop(endpoint);
        assert!(cq_lifetime.upgrade().is_none());
        assert!(domain_lifetime.upgrade().is_none());
        assert!(fabric_lifetime.upgrade().is_none());
    }

    #[test]
    fn test_capabilities_flags() {
        let caps = Capabilities::MSG | Capabilities::TAGGED;
        assert!(caps.contains(Capabilities::MSG));
        assert!(caps.contains(Capabilities::TAGGED));
        assert!(!caps.contains(Capabilities::RMA));
    }

    #[test]
    fn test_cq_bind_flags() {
        let flags = CqBindFlags::TRANSMIT | CqBindFlags::RECV;
        assert!(flags.contains(CqBindFlags::TRANSMIT));
        assert!(flags.contains(CqBindFlags::RECV));
    }
}
